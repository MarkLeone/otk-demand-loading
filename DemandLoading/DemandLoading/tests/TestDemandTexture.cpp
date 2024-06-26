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

#include "TestSparseTexture.h"

#include <OptiXToolkit/Error/cudaErrorCheck.h>
#include "DemandLoaderImpl.h"
#include "Memory/DeviceMemoryManager.h"
#include "PageTableManager.h"
#include "Textures/DemandTextureImpl.h"

#include <OptiXToolkit/DemandLoading/DemandTexture.h>
#include <OptiXToolkit/DemandLoading/SparseTextureDevices.h>
#include <OptiXToolkit/DemandLoading/TextureDescriptor.h>
#include <OptiXToolkit/ImageSource/CheckerBoardImage.h>
#include <OptiXToolkit/Memory/MemoryBlockDesc.h>

#include <gtest/gtest.h>

#include <cuda.h>

#include <memory>
#include <stdexcept>

using namespace demandLoading;
using namespace imageSource;
using namespace otk;

class TestDemandTexture : public testing::Test
{
  public:
    void SetUp()
    {
        // Initialize CUDA.

        OTK_ERROR_CHECK( cuInit( 0 ) );
        OTK_ERROR_CHECK( cudaFree( nullptr ) );
    }

    void initTexture( unsigned int width, unsigned int height, bool useMipMaps = true, bool tiledImage = true )
    {
        m_width = width;
        m_height = height;

        // Use the first capable device.
        m_deviceIndex = getFirstSparseTextureDevice();
        if( m_deviceIndex == demandLoading::MAX_DEVICES )
            throw std::runtime_error( "No devices support demand loading" );

        // FIXME: Which of these can I delete???
        OTK_ERROR_CHECK( cudaSetDevice( m_deviceIndex ) );
        OTK_ERROR_CHECK( cuInit( 0 ) );
        OTK_ERROR_CHECK( cudaFree( nullptr ) );
        OTK_ERROR_CHECK( cudaStreamCreate( &m_stream) );

        // Construct DemandLoaderImpl.  DemandTexture needs it to construct a TextureRequestHandler,
        // and it's provides a PageTableManager that's needed by initSampler().
        demandLoading::Options options{};
        options.useSmallTextureOptimization = true;
        m_loader.reset( new DemandLoaderImpl( options ) );

        // Create TextureDescriptor.
        m_desc.addressMode[0]   = CU_TR_ADDRESS_MODE_CLAMP;
        m_desc.addressMode[1]   = CU_TR_ADDRESS_MODE_CLAMP;
        m_desc.filterMode       = CU_TR_FILTER_MODE_POINT;
        m_desc.mipmapFilterMode = CU_TR_FILTER_MODE_POINT;
        m_desc.maxAnisotropy    = 16;

        // Create CheckerBoardImage.
        std::shared_ptr<ImageSource> image =
            std::make_shared<CheckerBoardImage>( m_width, m_height, /*squaresPerSide*/ 4, useMipMaps, tiledImage );

        // Construct and initialize DemandTexture
        m_texture.reset( new DemandTextureImpl( /*id*/ 0, m_desc, image, m_loader.get() ) );
        m_texture->open();
        m_texture->init();
    }

  protected:
    unsigned int                       m_deviceIndex = 0;
    CUstream                           m_stream{};
    unsigned int                       m_numDevices = 1;
    unsigned int                       m_width      = 256;
    unsigned int                       m_height     = 256;
    TextureDescriptor                  m_desc{};
    Options                            m_options{};
    std::unique_ptr<DemandTextureImpl> m_texture;
    std::unique_ptr<DemandLoaderImpl>  m_loader;
};

TEST_F( TestDemandTexture, TestInit )
{
    initTexture(256, 256);
}

TEST_F( TestDemandTexture, TestInitNonMipMapped )
{
    initTexture(256, 256, false);
}

TEST_F( TestDemandTexture, TestFillTile )
{
    // Skip test if sparse textures not supported
    if( m_deviceIndex == demandLoading::MAX_DEVICES )
        return;

    initTexture(256, 256);

    unsigned int      mipLevel      = 0;
    uint2             tileCoords[4] = {{0, 0}, {0, 3}, {3, 0}, {3, 3}};
    std::vector<char> tileBuffer( TILE_SIZE_IN_BYTES );
    for( unsigned int i = 0; i < 4; ++i )
    {
        // Read tile.
        unsigned int tileX = tileCoords[i].x;
        unsigned int tileY = tileCoords[i].y;
        EXPECT_EQ( true, m_texture->readTile( mipLevel, tileX, tileY, tileBuffer.data(), TILE_SIZE_IN_BYTES, CUstream{} ) );

        // Fill tile.
        TileBlockHandle bh = m_loader->getDeviceMemoryManager()->allocateTileBlock( TILE_SIZE_IN_BYTES );
        m_texture->fillTile( m_stream,
                             mipLevel, tileX, tileY,                  // tile to fill
                             tileBuffer.data(),                       // host ptr
                             CU_MEMORYTYPE_HOST, TILE_SIZE_IN_BYTES,  // data type and size
                             bh.handle, bh.block.offset()             // device data
                             );
    }

    // Set up kernel output buffer.
    const int outWidth  = 4;
    const int outHeight = 4;
    float4*   devOutput;
    size_t    outputSize = outWidth * outHeight * sizeof( float4 );
    OTK_ERROR_CHECK( cuMemAlloc( reinterpret_cast<CUdeviceptr*>( &devOutput ), outWidth * outHeight * sizeof( float4 ) ) );

    // Launch the worker.
    OTK_ERROR_CHECK( cudaDeviceSynchronize() );
    float       lod           = static_cast<float>( mipLevel );
    CUtexObject textureObject = m_texture->getTextureObject();
    launchSparseTextureKernel( textureObject, devOutput, outWidth, outHeight, lod );
    OTK_ERROR_CHECK( cudaDeviceSynchronize() );

    // Copy output buffer to host.
    std::vector<float4> hostOutput( outWidth * outHeight );
    OTK_ERROR_CHECK( cudaMemcpy( hostOutput.data(), devOutput, outputSize, cudaMemcpyDeviceToHost ) );

    // Validate the red channels of the output.  When resident, the red channel is a 0/1 checkerboard.
    // The output channels are -1 if the tile is not resident.
    float expected[4][4] = {{1.f, -1.f, -1.f, 0.f}, {-1.f, -1.f, -1.f, -1.f}, {-1.f, -1.f, -1.f, -1.f}, {0.f, -1.f, -1.f, 1.f}};
    for( int j = 0; j < outHeight; ++j )
    {
        for( int i = 0; i < outWidth; ++i )
        {
            const float4& pixel = hostOutput[j * outWidth + i];
            EXPECT_EQ( expected[j][i], pixel.x );
        }
    }

    OTK_ERROR_CHECK( cuMemFree( reinterpret_cast<CUdeviceptr>( devOutput ) ) );
}

TEST_F( TestDemandTexture, TestReadMipTail )
{
    // Skip test if sparse textures not supported
    if( m_deviceIndex == demandLoading::MAX_DEVICES )
        return;

    initTexture(256, 256); 

    // Read the entire mip tail.
    size_t mipTailSize = m_texture->getMipTailSize();
    std::vector<char> buffer( mipTailSize );
    EXPECT_NO_THROW( m_texture->readMipTail( buffer.data(), mipTailSize, CUstream{} ) );

    // For now we print the levels in the mip tail for visual validation.
    const TextureInfo& info      = m_texture->getInfo();
    EXPECT_TRUE( info.isValid );
    unsigned int       pixelSize = info.numChannels * getBytesPerChannel( info.format );
    size_t             offset    = 0;
    for( unsigned int mipLevel = m_texture->getMipTailFirstLevel(); mipLevel < info.numMipLevels; ++mipLevel )
    {
        const float4* texels = reinterpret_cast<const float4*>( &buffer[offset] );

        uint2 levelDims = m_texture->getMipLevelDims( mipLevel );
        for( unsigned int y = 0; y < levelDims.y; ++y )
        {
            for( unsigned int x = 0; x < levelDims.x; ++x )
            {
                float4 texel = texels[y * levelDims.x + x];

                // Quantize components to [0..9] for compact output.
                printf( "%i%i%i ", static_cast<int>( 9 * texel.x ), static_cast<int>( 9 * texel.y ),
                        static_cast<int>( 9 * texel.z ) );
            }
            printf( "\n" );
        }

        offset += levelDims.x * levelDims.y * pixelSize;
    }
}

TEST_F( TestDemandTexture, TestFillMipTail )
{
    // Skip test if sparse textures not supported
    if( m_deviceIndex == demandLoading::MAX_DEVICES )
        return;

    initTexture(256, 256);

    // Read the entire mip tail.
    size_t mipTailSize = m_texture->getMipTailSize();
    std::vector<char> buffer( mipTailSize );
    EXPECT_NO_THROW( m_texture->readMipTail( buffer.data(), mipTailSize, CUstream{} ) );

    // Map the backing storage and fill it.
    TileBlockHandle bh = m_loader->getDeviceMemoryManager()->allocateTileBlock( mipTailSize );
    m_texture->fillMipTail( m_stream,
                            buffer.data(), CU_MEMORYTYPE_HOST, mipTailSize,  // host buffer and size
                            bh.handle, bh.block.offset()                     // device buffer
                            );

    // Set up kernel output buffer.
    const int outWidth  = 4;
    const int outHeight = 4;
    float4*   devOutput;
    size_t    outputSize = outWidth * outHeight * sizeof( float4 );
    OTK_ERROR_CHECK( cuMemAlloc( reinterpret_cast<CUdeviceptr*>( &devOutput ), outWidth * outHeight * sizeof( float4 ) ) );

    // Launch the worker.
    OTK_ERROR_CHECK( cudaDeviceSynchronize() );
    float       lod           = static_cast<float>( m_texture->getMipTailFirstLevel() );
    CUtexObject textureObject = m_texture->getTextureObject();
    launchSparseTextureKernel( textureObject, devOutput, outWidth, outHeight, lod );
    OTK_ERROR_CHECK( cudaDeviceSynchronize() );

    // Copy output buffer to host.
    std::vector<float4> hostOutput( outWidth * outHeight );
    OTK_ERROR_CHECK( cudaMemcpy( hostOutput.data(), devOutput, outputSize, cudaMemcpyDeviceToHost ) );

    // Validate output.  The first level of the mip tail is a green/black checkerboard
    unsigned int pattern[4][4] = {{1, 0, 1, 0}, {0, 1, 0, 1}, {1, 0, 1, 0}, {0, 1, 0, 1}};
    for( int j = 0; j < outHeight; ++j )
    {
        for( int i = 0; i < outWidth; ++i )
        {
            const float4& pixel = hostOutput[j * outWidth + i];
            EXPECT_EQ( 0.f, pixel.x );
            EXPECT_EQ( pattern[j][i] ? 1.f : 0.f, pixel.y );
            EXPECT_EQ( 0.f, pixel.z );
            EXPECT_EQ( 0.f, pixel.w );
        }
    }

    OTK_ERROR_CHECK( cuMemFree( reinterpret_cast<CUdeviceptr>( devOutput ) ) );
}

TEST_F( TestDemandTexture, TestDenseTexture )
{
    initTexture( 32, 32 ); // small enough to be a dense texture
    const TextureInfo& info = m_texture->getInfo();
    EXPECT_TRUE( info.isValid );

    // Make sure it is using a dense texture
    EXPECT_FALSE( m_texture->useSparseTexture() );

    // Read the entire texture (as a mip tail), and fill it.
    size_t mipTailSize = TILE_SIZE_IN_BYTES;
    std::vector<char> buffer( mipTailSize );
    EXPECT_NO_THROW( m_texture->readMipTail( buffer.data(), mipTailSize, CUstream{} ) );
    m_texture->fillDenseTexture( m_stream, buffer.data(), info.width, info.height, true );

    // Set up kernel output buffer.
    const int outWidth  = 4;
    const int outHeight = 4;
    float4*   devOutput;
    size_t    outputSize = outWidth * outHeight * sizeof( float4 );
    OTK_ERROR_CHECK( cuMemAlloc( reinterpret_cast<CUdeviceptr*>( &devOutput ), outWidth * outHeight * sizeof( float4 ) ) );

    // Launch the worker.
    OTK_ERROR_CHECK( cudaDeviceSynchronize() );
    float       lod           = 0;
    CUtexObject textureObject = m_texture->getTextureObject();
    launchSparseTextureKernel( textureObject, devOutput, outWidth, outHeight, lod );
    OTK_ERROR_CHECK( cudaDeviceSynchronize() );

    // Copy output buffer to host.
    std::vector<float4> hostOutput( outWidth * outHeight );
    OTK_ERROR_CHECK( cudaMemcpy( hostOutput.data(), devOutput, outputSize, cudaMemcpyDeviceToHost ) );

    // Validate output. (Red checkerboard)
    float4 pattern[16] = {
        {1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f},  //
        {0.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f},  //
        {1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f},  //
        {0.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f}};

    for( int j = 0; j < outHeight; ++j )
    {
        for( int i = 0; i < outWidth; ++i )
        {
            int idx = j * outWidth + i;
            EXPECT_EQ( pattern[idx].x, hostOutput[idx].x );
            EXPECT_EQ( pattern[idx].y, hostOutput[idx].y );
            EXPECT_EQ( pattern[idx].z, hostOutput[idx].z );
            EXPECT_EQ( pattern[idx].w, hostOutput[idx].w );
        }
    }

    OTK_ERROR_CHECK( cuMemFree( reinterpret_cast<CUdeviceptr>( devOutput ) ) );
}

TEST_F( TestDemandTexture, TestDenseNonMipMappedTexture )
{
    initTexture( 256, 256, false, false ); 
    const TextureInfo& info = m_texture->getInfo();
    EXPECT_TRUE( info.isValid );

    // Make sure it is using a dense texture
    EXPECT_FALSE( m_texture->useSparseTexture() );

    // Read the entire texture, and fill it.
    std::vector<char> buffer( 256 * 256 * imageSource::getBytesPerChannel( info.format ) * info.numChannels );
    EXPECT_NO_THROW( m_texture->readNonMipMappedData( buffer.data(), buffer.size(), CUstream{} ) );
    m_texture->fillDenseTexture( m_stream, buffer.data(), info.width, info.height, true );

    // Set up kernel output buffer.
    const int outWidth  = 4;
    const int outHeight = 4;
    float4*   devOutput;
    size_t    outputSize = outWidth * outHeight * sizeof( float4 );
    OTK_ERROR_CHECK( cuMemAlloc( reinterpret_cast<CUdeviceptr*>( &devOutput ), outWidth * outHeight * sizeof( float4 ) ) );

    // Launch the worker.
    OTK_ERROR_CHECK( cudaDeviceSynchronize() );
    float       lod           = 0;
    CUtexObject textureObject = m_texture->getTextureObject();
    launchSparseTextureKernel( textureObject, devOutput, outWidth, outHeight, lod );
    OTK_ERROR_CHECK( cudaDeviceSynchronize() );

    // Copy output buffer to host.
    std::vector<float4> hostOutput( outWidth * outHeight );
    OTK_ERROR_CHECK( cudaMemcpy( hostOutput.data(), devOutput, outputSize, cudaMemcpyDeviceToHost ) );

    // Validate output. (Red checkerboard)
    float4 pattern[16] = {
        {1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f},  //
        {0.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f},  //
        {1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f},  //
        {0.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f}};

    for( int j = 0; j < outHeight; ++j )
    {
        for( int i = 0; i < outWidth; ++i )
        {
            int idx = j * outWidth + i;
            EXPECT_EQ( pattern[idx].x, hostOutput[idx].x );
            EXPECT_EQ( pattern[idx].y, hostOutput[idx].y );
            EXPECT_EQ( pattern[idx].z, hostOutput[idx].z );
            EXPECT_EQ( pattern[idx].w, hostOutput[idx].w );
        }
    }

    OTK_ERROR_CHECK( cuMemFree( reinterpret_cast<CUdeviceptr>( devOutput ) ) );
}

TEST_F( TestDemandTexture, TestSparseNonMipmappedTexture )
{
    // Skip test if sparse textures not supported
    if( m_deviceIndex == demandLoading::MAX_DEVICES )
        return;

    initTexture(256, 256, false, true);

    // Read and fill the corner tiles, leaving the others non-resident.
    unsigned int      mipLevel      = 0;
    uint2             tileCoords[4] = {{0, 0}, {0, 3}, {3, 0}, {3, 3}};
    std::vector<char> tileBuffer( TILE_SIZE_IN_BYTES );
    for( unsigned int i = 0; i < 4; ++i )
    {
        // Read tile.
        unsigned int tileX = tileCoords[i].x;
        unsigned int tileY = tileCoords[i].y;
        EXPECT_EQ( true, m_texture->readTile( mipLevel, tileX, tileY, tileBuffer.data(), TILE_SIZE_IN_BYTES, CUstream{} ) );

        // Fill tile.
        TileBlockHandle bh = m_loader->getDeviceMemoryManager()->allocateTileBlock( TILE_SIZE_IN_BYTES );
        m_texture->fillTile( m_stream,
                             mipLevel, tileX, tileY,                  // tile coordinates
                             tileBuffer.data(),                       // source data
                             CU_MEMORYTYPE_HOST, TILE_SIZE_IN_BYTES,  // src type and size
                             bh.handle, bh.block.offset()             // dest
                             );
    }

    // Set up kernel output buffer.
    const int outWidth  = 4;
    const int outHeight = 4;
    float4*   devOutput;
    size_t    outputSize = outWidth * outHeight * sizeof( float4 );
    OTK_ERROR_CHECK( cuMemAlloc( reinterpret_cast<CUdeviceptr*>( &devOutput ), outWidth * outHeight * sizeof( float4 ) ) );

    // Launch the worker.
    OTK_ERROR_CHECK( cudaDeviceSynchronize() );
    float       lod           = 1.0f; // Give a non-zero lod, and test if level 0 is still retrieved in tex2D
    CUtexObject textureObject = m_texture->getTextureObject();
    launchSparseTextureKernel( textureObject, devOutput, outWidth, outHeight, lod );
    OTK_ERROR_CHECK( cudaDeviceSynchronize() );

    // Copy output buffer to host.
    std::vector<float4> hostOutput( outWidth * outHeight );
    OTK_ERROR_CHECK( cudaMemcpy( hostOutput.data(), devOutput, outputSize, cudaMemcpyDeviceToHost ) );

    // Validate the red channels of the output.  When resident, the red channel is a 0/1 checkerboard.
    // The output channels are -1 if the tile is not resident.
    float expected[4][4] = {{1.f, -1.f, -1.f, 0.f}, {-1.f, -1.f, -1.f, -1.f}, {-1.f, -1.f, -1.f, -1.f}, {0.f, -1.f, -1.f, 1.f}};
    for( int j = 0; j < outHeight; ++j )
    {
        for( int i = 0; i < outWidth; ++i )
        {
            const float4& pixel = hostOutput[j * outWidth + i];
            EXPECT_EQ( expected[j][i], pixel.x );
        }
    }

    OTK_ERROR_CHECK( cuMemFree( reinterpret_cast<CUdeviceptr>( devOutput ) ) );
}



//-------------------------------------------------------------------------

uint8_t quantize( float f )
{
    float value = 255 * std::min( 1.0f, std::max( 0.0f, f ));
    return uint8_t( value );
}

void writePPM( const char* filename, float* pixels, int width, int height, int numComponents )
{
    FILE* fp = fopen( filename, "wb" ); /* b - binary mode */
    fprintf( fp, "P6\n%d %d\n255\n", width, height );
    for( int j = height-1; j >= 0; --j )
    {
        for( int i = 0; i < width; ++i )
        {
            float r = (numComponents >= 1) ? pixels[ (j*width + i) * numComponents + 0 ] : 0;
            float g = (numComponents >= 2) ? pixels[ (j*width + i) * numComponents + 1 ] : 0;
            float b = (numComponents >= 3) ? pixels[ (j*width + i) * numComponents + 2 ] : 0;
            uint8_t color[3] = {quantize( r ), quantize( g ), quantize( b )};
            fwrite( color, sizeof( uint8_t ), 3, fp );
        }
    }
    fclose( fp );
}

TEST_F( TestDemandTexture, TestNonOptixTexturing )
{
    // Skip test if sparse textures not supported
    if( m_deviceIndex == demandLoading::MAX_DEVICES )
        return;

    // This initializes the demand loader, and creates a texture, but not managed by the demand loader
    initTexture( m_width, m_height );

    // Create a texture that is managed by the demand loader
    TextureDescriptor desc{};
    desc.addressMode[0]   = CU_TR_ADDRESS_MODE_CLAMP;
    desc.addressMode[1]   = CU_TR_ADDRESS_MODE_CLAMP;
    desc.filterMode       = CU_TR_FILTER_MODE_LINEAR;
    desc.mipmapFilterMode = CU_TR_FILTER_MODE_LINEAR;
    desc.maxAnisotropy    = 16;
    std::shared_ptr<ImageSource> image = std::make_shared<CheckerBoardImage>( m_width, m_height, 16, true, true );
    const demandLoading::DemandTexture& texture = m_loader->createTexture( image, desc );
    unsigned int textureId = texture.getId();

    // Make output buffer for host and device
    const int outWidth  = 150;
    const int outHeight = 150;
    float4*   devOutput;
    size_t    outputSize = outWidth * outHeight * sizeof( float4 );
    OTK_ERROR_CHECK( cuMemAlloc( reinterpret_cast<CUdeviceptr*>( &devOutput ), outputSize ) );
    float4*   hostOutput = (float4*) malloc( outputSize );

    // Perform launches
    unsigned int totalRequests = 0;
    const unsigned int numLaunches = 4;
    for( unsigned int launchNum = 0; launchNum < numLaunches; ++launchNum )
    {
        OTK_ERROR_CHECK( cudaSetDevice( m_deviceIndex ) );
        demandLoading::DeviceContext context;
        m_loader->launchPrepare( m_stream, context );
        launchTextureDrawKernel( m_stream, context, textureId, devOutput, outWidth, outHeight );
        Ticket ticket = m_loader->processRequests( m_stream, context );
        ticket.wait();
        totalRequests += ticket.numTasksTotal();
        printf( "Launch %d: %d requests.\n", launchNum, ticket.numTasksTotal() );
    }

    // 1 sampler request plus 20 tile requests
    EXPECT_EQ( totalRequests, 21 );

    OTK_ERROR_CHECK( cudaMemcpy( hostOutput, devOutput, outputSize, cudaMemcpyDeviceToHost ) );
    writePPM( "noOptix.ppm", (float*)hostOutput, outWidth, outHeight, 4 );

    // Free output buffers
    free( hostOutput );
    OTK_ERROR_CHECK( cuMemFree( reinterpret_cast<CUdeviceptr>( devOutput ) ) );
}
