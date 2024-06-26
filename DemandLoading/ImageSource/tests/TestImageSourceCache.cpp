//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
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

#include "Config.h"
#include "ImageSourceTestConfig.h"

#include <OptiXToolkit/ImageSource/ImageSourceCache.h>
#include <OptiXToolkit/ImageSource/TiledImageSource.h>

#include <gtest/gtest.h>

using namespace imageSource;

class TestImageSourceCache : public testing::Test
{
  protected:
    ImageSourceCache m_cache;
    std::string      m_directoryPrefix{ getSourceDir() + "/Textures/" };
    std::string      m_exrPath{ m_directoryPrefix + "TiledMipMappedFloat.exr" };
    std::string      m_jpgPath{ m_directoryPrefix + "level0.jpg" };
};

TEST_F( TestImageSourceCache, findMissing )
{
    EXPECT_EQ( std::shared_ptr<ImageSource>(), m_cache.find( "missing-file.foo" ) );
}

#if OTK_USE_OPENEXR
TEST_F( TestImageSourceCache, get )
{
    std::shared_ptr<ImageSource> imageSource1( m_cache.get( m_exrPath ) );
    std::shared_ptr<ImageSource> imageSource2( m_cache.get( m_exrPath ) );
    const CacheStatistics        stats{ m_cache.getStatistics() };

    EXPECT_TRUE( imageSource1 );
    EXPECT_TRUE( imageSource2 );
    EXPECT_EQ( imageSource1, imageSource2 );
    EXPECT_EQ( 1, stats.numImageSources );
}

TEST_F( TestImageSourceCache, findPresent )
{
    std::shared_ptr<ImageSource> image = m_cache.get( m_exrPath );

    EXPECT_EQ( image, m_cache.find( m_exrPath ) );
}
#endif

#if OTK_USE_OIIO
TEST_F( TestImageSourceCache, setAdaptedReturnsAdapted )
{
    std::shared_ptr<ImageSource> adapted = std::make_shared<TiledImageSource>( createImageSource( m_jpgPath ) );
    m_cache.set( m_jpgPath, adapted );

    std::shared_ptr<ImageSource> image = m_cache.get( m_jpgPath );

    EXPECT_EQ( adapted, image );
}
#endif
