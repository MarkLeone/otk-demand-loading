//
// Copyright (c) 2021, NVIDIA CORPORATION. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//

#include <OptiXToolkit/ImageSource/ImageSource.h>
#include <OptiXToolkit/ImageSource/CheckerBoardImage.h>
#include <OptiXToolkit/ImageSource/CoreEXRReader.h>
#include <OptiXToolkit/ImageSource/DeviceMandelbrotImage.h>

#include "Exception.h"

#include <cstddef>  // for size_t

namespace imageSource {

bool ImageSourceBase::readMipTail( char*        dest,
                                   unsigned int mipTailFirstLevel,
                                   unsigned int numMipLevels,
                                   const uint2* mipLevelDims,
                                   unsigned int pixelSizeInBytes,
                                   CUstream     stream )
{
    size_t offset = 0;
    for( unsigned int mipLevel = mipTailFirstLevel; mipLevel < numMipLevels; ++mipLevel )
    {
        const uint2 levelDims = mipLevelDims[mipLevel];
        readMipLevel( dest + offset, mipLevel, levelDims.x, levelDims.y, stream );

        // Increment offset.
        offset += levelDims.x * levelDims.y * pixelSizeInBytes;
    }

    return true;
}

std::shared_ptr<ImageSource> createImageSource( const std::string& filename, const std::string& directory )
{
    // Special cases
    if( filename == "checkerboard" )
    {
        return std::shared_ptr<ImageSource>( new CheckerBoardImage( 2048, 2048, /*squaresPerSide=*/32, /*useMipmaps=*/true ) );
    }
    else if( filename == "mandelbrot" )
    {
        std::vector<float4> colors = {{1.0f, 1.0f, 1.0f, 0.0f},
                                      {0.0f, 0.0f, 1.0f, 0.0f},
                                      {0.0f, 0.5f, 0.0f, 0.0f},
                                      {1.0f, 0.0f, .0f, 0.0f},
                                      {1.0f, 1.0f, 0.0f, 0.0f}};
        return std::shared_ptr<ImageSource>( new DeviceMandelbrotImage( 2048, 2048, /*xmin=*/-2.0, /*ymin=*/-2.0,
                                                                        /*xmax=*/2.0, /*ymax=*/2.0,
                                                                        /*iterations=*/512, colors ) );
    }

    // Construct ImageSource based on filename extension.
    size_t      dot       = filename.find_last_of( "." );
    std::string extension = dot == std::string::npos ? "" : filename.substr( dot );
    std::string path = directory + '/' + filename;

    if( extension == ".exr" )
    {
        return std::shared_ptr<ImageSource>( new CoreEXRReader( path ) );
    }
    else
    {
        std::string msg= "Image file not supported: ";
        throw Exception( ( msg + filename ).c_str() );
    }
}


}  // namespace imageSource