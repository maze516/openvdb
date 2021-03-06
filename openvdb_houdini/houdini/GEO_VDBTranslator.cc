///////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2012-2018 DreamWorks Animation LLC
//
// All rights reserved. This software is distributed under the
// Mozilla Public License 2.0 ( http://www.mozilla.org/MPL/2.0/ )
//
// Redistributions of source code must retain the above copyright
// and license notice and the following restrictions and disclaimer.
//
// *     Neither the name of DreamWorks Animation nor the names of
// its contributors may be used to endorse or promote products derived
// from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// IN NO EVENT SHALL THE COPYRIGHT HOLDERS' AND CONTRIBUTORS' AGGREGATE
// LIABILITY FOR ALL CLAIMS REGARDLESS OF THEIR BASIS EXCEED US$250.00.
//
///////////////////////////////////////////////////////////////////////////

/*
 * Copyright (c) 2012
 *      Side Effects Software Inc.  All rights reserved.
 *
 * Redistribution and use of Houdini Development Kit samples in source and
 * binary forms, with or without modification, are permitted provided that the
 * following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. The name of Side Effects Software may not be used to endorse or
 *    promote products derived from this software without specific prior
 *    written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY SIDE EFFECTS SOFTWARE `AS IS' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN
 * NO EVENT SHALL SIDE EFFECTS SOFTWARE BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
 * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *----------------------------------------------------------------------------
 */

#include "GU_PrimVDB.h"
#include "Utils.h"

#include <UT/UT_EnvControl.h>
#include <UT/UT_Error.h>
#include <UT/UT_ErrorManager.h>
#include <UT/UT_IOTable.h>
#include <UT/UT_IStream.h>
#include <UT/UT_Version.h>

#include <FS/FS_IStreamDevice.h>
#include <GA/GA_Stat.h>
#include <GU/GU_Detail.h>
#include <SOP/SOP_Node.h>
#include <GEO/GEO_IOTranslator.h>

#include <openvdb/io/Stream.h>
#include <openvdb/io/File.h>
#include <openvdb/Metadata.h>

#include <stdio.h>
#include <iostream>

using namespace openvdb_houdini;
using std::cerr;

namespace {

class GEO_VDBTranslator : public GEO_IOTranslator
{
public:
             GEO_VDBTranslator() {}
    virtual ~GEO_VDBTranslator() {}

    virtual GEO_IOTranslator *duplicate() const;

    virtual const char *formatName() const;

    virtual int         checkExtension(const char *name);
    virtual void        getFileExtensions(UT_StringArray &extensions) const;

    virtual int         checkMagicNumber(unsigned magic);

    virtual bool        fileStat(const char *filename,
                                GA_Stat &stat,
                                uint level);

    virtual GA_Detail::IOStatus fileLoad(GEO_Detail *gdp, UT_IStream &is, bool ate_magic);
    virtual GA_Detail::IOStatus fileSave(const GEO_Detail *gdp, std::ostream &os);
    virtual GA_Detail::IOStatus fileSaveToFile(const GEO_Detail *gdp, const char *fname);
};

GEO_IOTranslator *
GEO_VDBTranslator::duplicate() const
{
    return new GEO_VDBTranslator();
}

const char *
GEO_VDBTranslator::formatName() const
{
    return "VDB Format";
}

int
GEO_VDBTranslator::checkExtension(const char *name)
{
    return UT_String(name).matchFileExtension(".vdb");
}

void
GEO_VDBTranslator::getFileExtensions(UT_StringArray &extensions) const
{
    extensions.clear();
    extensions.append(".vdb");
}

int
GEO_VDBTranslator::checkMagicNumber(unsigned /*magic*/)
{
    return 0;
}

bool
GEO_VDBTranslator::fileStat(const char *filename, GA_Stat &stat, uint /*level*/)
{
    stat.clear();

    try {
        openvdb::io::File file(filename);

        file.open(/*delayLoad*/false);

        int             nprim = 0;
        UT_BoundingBox  bbox;
        bbox.makeInvalid();

        // Loop over all grids in the file.
        for (openvdb::io::File::NameIterator nameIter = file.beginName();
            nameIter != file.endName(); ++nameIter)
        {
            const std::string& gridName = nameIter.gridName();

            // Read the grid metadata.
            auto grid = file.readGridMetadata(gridName);

            auto stats = grid->getStatsMetadata();

            openvdb::Vec3IMetadata::Ptr         meta_minbbox, meta_maxbbox;
            UT_BoundingBox                      voxelbox;

            voxelbox.initBounds();

            meta_minbbox = stats->getMetadata<openvdb::Vec3IMetadata>("file_bbox_min");
            meta_maxbbox = stats->getMetadata<openvdb::Vec3IMetadata>("file_bbox_max");
            if (meta_minbbox && meta_maxbbox)
            {
                UT_Vector3              minv, maxv;
                minv = UTvdbConvert(meta_minbbox->value());
                maxv = UTvdbConvert(meta_maxbbox->value());
                voxelbox.enlargeBounds(minv);
                voxelbox.enlargeBounds(maxv);
                // We need to convert from corner-sampled (as in VDB)
                // to center-sampled (as our BBOX elsewhere reports)
                voxelbox.expandBounds(0.5, 0.5, 0.5);

                // Transform
                UT_Vector3              voxelpts[8];
                UT_BoundingBox          worldbox;

                worldbox.initBounds();
                voxelbox.getBBoxPoints(voxelpts);
                for (int i = 0; i < 8; i++)
                {
                    worldbox.enlargeBounds(
                            UTvdbConvert( grid->indexToWorld(UTvdbConvert(voxelpts[i])) ) );
                }

                bbox.enlargeBounds(worldbox);
            }

            if (voxelbox.isValid()) {
                stat.appendVolume(nprim, gridName.c_str(),
                    static_cast<int>(voxelbox.size().x()),
                    static_cast<int>(voxelbox.size().y()),
                    static_cast<int>(voxelbox.size().z()));
            } else {
                stat.appendVolume(nprim, gridName.c_str(), 0, 0, 0);
            }
            nprim++;
        }

        // Straightforward correspondence:
        stat.setPointCount(nprim);
        stat.setVertexCount(nprim);
        stat.setPrimitiveCount(nprim);
        stat.setBounds(bbox);

        file.close();
    } catch (std::exception &e) {
        cerr << "Stat failure: " << e.what() << "\n";
        return false;
    }

    return true;
}

GA_Detail::IOStatus
GEO_VDBTranslator::fileLoad(GEO_Detail *geogdp, UT_IStream &is, bool /*ate_magic*/)
{
    UT_WorkBuffer   buf;
    GU_Detail       *gdp = static_cast<GU_Detail*>(geogdp);
    bool            ok = true;

    // Create a std::stream proxy.
    FS_IStreamDevice    reader(&is);
    auto streambuf = new FS_IStreamDeviceBuffer(reader);
    auto stdstream = new std::istream(streambuf);

    try {
        // Create and open a VDB file, but don't read any grids yet.
        openvdb::io::Stream file(*stdstream, /*delayLoad*/false);

        // Read the file-level metadata into global attributes.
        openvdb::MetaMap::Ptr fileMetadata = file.getMetadata();
        if (fileMetadata) {
            GU_PrimVDB::createAttrsFromMetadata(
                GA_ATTRIB_GLOBAL, GA_Offset(0), *fileMetadata, *geogdp);
        }

        // Loop over all grids in the file.
        auto && allgrids = file.getGrids();
        for (auto && grid : *allgrids)
        {
            // Add a new VDB primitive for this grid.
            // Note: this clears the grid's metadata.
            createVdbPrimitive(*gdp, grid);
        }
    } catch (std::exception &e) {
        // Add a warning here instead of an error or else the File SOP's
        // Missing Frame parameter won't be able to suppress cook errors.
        UTaddCommonWarning(UT_ERROR_JUST_STRING, e.what());
        ok = false;
    }

    delete stdstream;
    delete streambuf;

    return ok;
}

template <typename FileT, typename OutputT>
bool
fileSaveVDB(const GEO_Detail *geogdp, OutputT os)
{
    const GU_Detail *gdp = static_cast<const GU_Detail*>(geogdp);
    if (!gdp) return false;

    try {
        // Populate an output GridMap with VDB grid primitives found in the
        // geometry.
        openvdb::GridPtrVec outGrids;
        for (VdbPrimCIterator it(gdp); it; ++it) {
            const GU_PrimVDB* vdb = *it;

            // Create a new grid that shares the primitive's tree and transform
            // and then transfer primitive attributes to the new grid as metadata.
            GridPtr grid = openvdb::ConstPtrCast<Grid>(vdb->getGrid().copyGrid());
            GU_PrimVDB::createMetadataFromGridAttrs(*grid, *vdb, *gdp);
            grid->removeMeta("is_vdb");

            // Retrieve the grid's name from the primitive attribute.
            grid->setName(it.getPrimitiveName().toStdString());

            outGrids.push_back(grid);
        }

        // Add file-level metadata.
        openvdb::MetaMap fileMetadata;

        std::string versionStr = "Houdini ";
        versionStr += UTgetFullVersion();
        versionStr += "/GEO_VDBTranslator";

        fileMetadata.insertMeta("creator", openvdb::StringMetadata(versionStr));

#if defined(SESI_OPENVDB)
        GU_PrimVDB::createMetadataFromAttrs(
            fileMetadata, GA_ATTRIB_GLOBAL, GA_Offset(0), *gdp);
#endif
        // Create a VDB file object.
        FileT file(os);

        // Always enable active mask compression, since it is fast
        // and compresses level sets and fog volumes well.
        uint32_t compression = openvdb::io::COMPRESS_ACTIVE_MASK;

        // Enable Blosc unless backwards compatibility is requested.
        if (openvdb::io::Archive::hasBloscCompression()
            && !UT_EnvControl::getInt(ENV_HOUDINI13_VOLUME_COMPATIBILITY)) {
            compression |= openvdb::io::COMPRESS_BLOSC;
        }
        file.setCompression(compression);

        file.write(outGrids, fileMetadata);

    } catch (std::exception &e) {
        cerr << "Save failure: " << e.what() << "\n";
        return false;
    }

    return true;
}

GA_Detail::IOStatus
GEO_VDBTranslator::fileSave(const GEO_Detail *geogdp, std::ostream &os)
{
    // Saving via io::Stream will NOT save grid offsets, disabling partial
    // reading.
    return fileSaveVDB<openvdb::io::Stream, std::ostream &>(geogdp, os);
}

GA_Detail::IOStatus
GEO_VDBTranslator::fileSaveToFile(const GEO_Detail *geogdp, const char *fname)
{
    // Saving via io::File will save grid offsets that allow for partial
    // reading.
    return fileSaveVDB<openvdb::io::File, const char *>(geogdp, fname);
}

} // unnamed namespace

void
new_VDBGeometryIO(void *)
{
    GU_Detail::registerIOTranslator(new GEO_VDBTranslator());

    // addExtension() will ignore if vdb is already in the list of extensions
    UTgetGeoExtensions()->addExtension("vdb");
}

#ifndef SESI_OPENVDB
void
newGeometryIO(void *data)
{
    // Initialize the version of the OpenVDB library that this library is built against
    // (i.e., not the HDK native OpenVDB library).
    openvdb::initialize();
    // Register a .vdb file translator.
    new_VDBGeometryIO(data);
}
#endif

// Copyright (c) 2012-2018 DreamWorks Animation LLC
// All rights reserved. This software is distributed under the
// Mozilla Public License 2.0 ( http://www.mozilla.org/MPL/2.0/ )
