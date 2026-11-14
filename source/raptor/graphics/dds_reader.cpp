#include "dds_reader.hpp"

#include "foundation/assert.hpp"
#include "foundation/file.hpp"
#include "foundation/log.hpp"
#include "foundation/numerics.hpp"

#include "external/bcdec.h"

#include <stdlib.h>
#include <string.h>

typedef u32 UINT;
typedef u8 BYTE;
#if !defined(DWORD)
typedef u32 DWORD;
#endif

#define MAKEFOURCC(ch0, ch1, ch2, ch3)                              \
                ((DWORD)(BYTE)(ch0) | ((DWORD)(BYTE)(ch1) << 8) |   \
                ((DWORD)(BYTE)(ch2) << 16) | ((DWORD)(BYTE)(ch3) << 24 ))

DWORD dx10_fourcc = MAKEFOURCC('D','X','1','0');

struct DDS_PIXELFORMAT {
  DWORD dwSize;
  DWORD dwFlags;
  DWORD dwFourCC;
  DWORD dwRGBBitCount;
  DWORD dwRBitMask;
  DWORD dwGBitMask;
  DWORD dwBBitMask;
  DWORD dwABitMask;
};

typedef struct {
  DWORD           dwSize;
  DWORD           dwFlags;
  DWORD           dwHeight;
  DWORD           dwWidth;
  DWORD           dwPitchOrLinearSize;
  DWORD           dwDepth;
  DWORD           dwMipMapCount;
  DWORD           dwReserved1[11];
  DDS_PIXELFORMAT ddspf;
  DWORD           dwCaps;
  DWORD           dwCaps2;
  DWORD           dwCaps3;
  DWORD           dwCaps4;
  DWORD           dwReserved2;
} DDS_HEADER;

enum D3D10_RESOURCE_DIMENSION {
  D3D10_RESOURCE_DIMENSION_UNKNOWN = 0,
  D3D10_RESOURCE_DIMENSION_BUFFER = 1,
  D3D10_RESOURCE_DIMENSION_TEXTURE1D = 2,
  D3D10_RESOURCE_DIMENSION_TEXTURE2D = 3,
  D3D10_RESOURCE_DIMENSION_TEXTURE3D = 4
};

enum D3D10_RESOURCE_MISC_FLAG {
  D3D10_RESOURCE_MISC_GENERATE_MIPS = 0x1L,
  D3D10_RESOURCE_MISC_SHARED = 0x2L,
  D3D10_RESOURCE_MISC_TEXTURECUBE = 0x4L,
  D3D10_RESOURCE_MISC_SHARED_KEYEDMUTEX = 0x10L,
  D3D10_RESOURCE_MISC_GDI_COMPATIBLE = 0x20L
};

enum DXGI_FORMAT {
  DXGI_FORMAT_UNKNOWN = 0,
  DXGI_FORMAT_R32G32B32A32_TYPELESS = 1,
  DXGI_FORMAT_R32G32B32A32_FLOAT = 2,
  DXGI_FORMAT_R32G32B32A32_UINT = 3,
  DXGI_FORMAT_R32G32B32A32_SINT = 4,
  DXGI_FORMAT_R32G32B32_TYPELESS = 5,
  DXGI_FORMAT_R32G32B32_FLOAT = 6,
  DXGI_FORMAT_R32G32B32_UINT = 7,
  DXGI_FORMAT_R32G32B32_SINT = 8,
  DXGI_FORMAT_R16G16B16A16_TYPELESS = 9,
  DXGI_FORMAT_R16G16B16A16_FLOAT = 10,
  DXGI_FORMAT_R16G16B16A16_UNORM = 11,
  DXGI_FORMAT_R16G16B16A16_UINT = 12,
  DXGI_FORMAT_R16G16B16A16_SNORM = 13,
  DXGI_FORMAT_R16G16B16A16_SINT = 14,
  DXGI_FORMAT_R32G32_TYPELESS = 15,
  DXGI_FORMAT_R32G32_FLOAT = 16,
  DXGI_FORMAT_R32G32_UINT = 17,
  DXGI_FORMAT_R32G32_SINT = 18,
  DXGI_FORMAT_R32G8X24_TYPELESS = 19,
  DXGI_FORMAT_D32_FLOAT_S8X24_UINT = 20,
  DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS = 21,
  DXGI_FORMAT_X32_TYPELESS_G8X24_UINT = 22,
  DXGI_FORMAT_R10G10B10A2_TYPELESS = 23,
  DXGI_FORMAT_R10G10B10A2_UNORM = 24,
  DXGI_FORMAT_R10G10B10A2_UINT = 25,
  DXGI_FORMAT_R11G11B10_FLOAT = 26,
  DXGI_FORMAT_R8G8B8A8_TYPELESS = 27,
  DXGI_FORMAT_R8G8B8A8_UNORM = 28,
  DXGI_FORMAT_R8G8B8A8_UNORM_SRGB = 29,
  DXGI_FORMAT_R8G8B8A8_UINT = 30,
  DXGI_FORMAT_R8G8B8A8_SNORM = 31,
  DXGI_FORMAT_R8G8B8A8_SINT = 32,
  DXGI_FORMAT_R16G16_TYPELESS = 33,
  DXGI_FORMAT_R16G16_FLOAT = 34,
  DXGI_FORMAT_R16G16_UNORM = 35,
  DXGI_FORMAT_R16G16_UINT = 36,
  DXGI_FORMAT_R16G16_SNORM = 37,
  DXGI_FORMAT_R16G16_SINT = 38,
  DXGI_FORMAT_R32_TYPELESS = 39,
  DXGI_FORMAT_D32_FLOAT = 40,
  DXGI_FORMAT_R32_FLOAT = 41,
  DXGI_FORMAT_R32_UINT = 42,
  DXGI_FORMAT_R32_SINT = 43,
  DXGI_FORMAT_R24G8_TYPELESS = 44,
  DXGI_FORMAT_D24_UNORM_S8_UINT = 45,
  DXGI_FORMAT_R24_UNORM_X8_TYPELESS = 46,
  DXGI_FORMAT_X24_TYPELESS_G8_UINT = 47,
  DXGI_FORMAT_R8G8_TYPELESS = 48,
  DXGI_FORMAT_R8G8_UNORM = 49,
  DXGI_FORMAT_R8G8_UINT = 50,
  DXGI_FORMAT_R8G8_SNORM = 51,
  DXGI_FORMAT_R8G8_SINT = 52,
  DXGI_FORMAT_R16_TYPELESS = 53,
  DXGI_FORMAT_R16_FLOAT = 54,
  DXGI_FORMAT_D16_UNORM = 55,
  DXGI_FORMAT_R16_UNORM = 56,
  DXGI_FORMAT_R16_UINT = 57,
  DXGI_FORMAT_R16_SNORM = 58,
  DXGI_FORMAT_R16_SINT = 59,
  DXGI_FORMAT_R8_TYPELESS = 60,
  DXGI_FORMAT_R8_UNORM = 61,
  DXGI_FORMAT_R8_UINT = 62,
  DXGI_FORMAT_R8_SNORM = 63,
  DXGI_FORMAT_R8_SINT = 64,
  DXGI_FORMAT_A8_UNORM = 65,
  DXGI_FORMAT_R1_UNORM = 66,
  DXGI_FORMAT_R9G9B9E5_SHAREDEXP = 67,
  DXGI_FORMAT_R8G8_B8G8_UNORM = 68,
  DXGI_FORMAT_G8R8_G8B8_UNORM = 69,
  DXGI_FORMAT_BC1_TYPELESS = 70,
  DXGI_FORMAT_BC1_UNORM = 71,
  DXGI_FORMAT_BC1_UNORM_SRGB = 72,
  DXGI_FORMAT_BC2_TYPELESS = 73,
  DXGI_FORMAT_BC2_UNORM = 74,
  DXGI_FORMAT_BC2_UNORM_SRGB = 75,
  DXGI_FORMAT_BC3_TYPELESS = 76,
  DXGI_FORMAT_BC3_UNORM = 77,
  DXGI_FORMAT_BC3_UNORM_SRGB = 78,
  DXGI_FORMAT_BC4_TYPELESS = 79,
  DXGI_FORMAT_BC4_UNORM = 80,
  DXGI_FORMAT_BC4_SNORM = 81,
  DXGI_FORMAT_BC5_TYPELESS = 82,
  DXGI_FORMAT_BC5_UNORM = 83,
  DXGI_FORMAT_BC5_SNORM = 84,
  DXGI_FORMAT_B5G6R5_UNORM = 85,
  DXGI_FORMAT_B5G5R5A1_UNORM = 86,
  DXGI_FORMAT_B8G8R8A8_UNORM = 87,
  DXGI_FORMAT_B8G8R8X8_UNORM = 88,
  DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM = 89,
  DXGI_FORMAT_B8G8R8A8_TYPELESS = 90,
  DXGI_FORMAT_B8G8R8A8_UNORM_SRGB = 91,
  DXGI_FORMAT_B8G8R8X8_TYPELESS = 92,
  DXGI_FORMAT_B8G8R8X8_UNORM_SRGB = 93,
  DXGI_FORMAT_BC6H_TYPELESS = 94,
  DXGI_FORMAT_BC6H_UF16 = 95,
  DXGI_FORMAT_BC6H_SF16 = 96,
  DXGI_FORMAT_BC7_TYPELESS = 97,
  DXGI_FORMAT_BC7_UNORM = 98,
  DXGI_FORMAT_BC7_UNORM_SRGB = 99,
  DXGI_FORMAT_AYUV = 100,
  DXGI_FORMAT_Y410 = 101,
  DXGI_FORMAT_Y416 = 102,
  DXGI_FORMAT_NV12 = 103,
  DXGI_FORMAT_P010 = 104,
  DXGI_FORMAT_P016 = 105,
  DXGI_FORMAT_420_OPAQUE = 106,
  DXGI_FORMAT_YUY2 = 107,
  DXGI_FORMAT_Y210 = 108,
  DXGI_FORMAT_Y216 = 109,
  DXGI_FORMAT_NV11 = 110,
  DXGI_FORMAT_AI44 = 111,
  DXGI_FORMAT_IA44 = 112,
  DXGI_FORMAT_P8 = 113,
  DXGI_FORMAT_A8P8 = 114,
  DXGI_FORMAT_B4G4R4A4_UNORM = 115,
  DXGI_FORMAT_P208 = 130,
  DXGI_FORMAT_V208 = 131,
  DXGI_FORMAT_V408 = 132,
  DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE = 189,
  DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE = 190,
  DXGI_FORMAT_A4B4G4R4_UNORM = 191,
  DXGI_FORMAT_FORCE_UINT = 0xffffffff
};

typedef struct  {
  DXGI_FORMAT              dxgiFormat;
  D3D10_RESOURCE_DIMENSION resourceDimension;
  UINT                     miscFlag;
  UINT                     arraySize;
  UINT                     miscFlags2;
} DDS_HEADER_DXT10;

#define DDSD_CAPS 0x1
#define DDSD_HEIGHT 0x2
#define DDSD_WIDTH 0x4
#define DDSD_PITCH 0x8
#define DDSD_PIXELFORMAT 0x1000
#define DDSD_MIPMAPCOUNT 0x20000
#define DDSD_LINEARSIZE 0x80000
#define DDSD_DEPTH 0x800000

#define DDSCAPS_COMPLEX 0x8
#define DDSCAPS_MIPMAP 0x400000
#define DDSCAPS_TEXTURE 0x1000

#define DDSCAPS2_CUBEMAP 0x200
#define DDSCAPS2_CUBEMAP_POSITIVEX 0x400
#define DDSCAPS2_CUBEMAP_NEGATIVEX 0x800
#define DDSCAPS2_CUBEMAP_POSITIVEY 0x1000
#define DDSCAPS2_CUBEMAP_NEGATIVEY 0x2000
#define DDSCAPS2_CUBEMAP_POSITIVEZ 0x4000
#define DDSCAPS2_CUBEMAP_NEGATIVEZ 0x8000
#define DDSCAPS2_VOLUME 0x200000

#define DDPF_ALPHAPIXELS 0x1
#define DDPF_ALPHA 0x2
#define DDPF_FOURCC 0x4
#define DDPF_RGB 0x40
#define DDPF_YUV 0x200
#define DDPF_LUMINANCE 0x20000

/*
Resource Format 	dwFlags 	dwRGBBitCount 	dwRBitMask 	dwGBitMask 	dwBBitMask 	dwABitMask
DXGI_FORMAT_R8G8B8A8_UNORM
D3DFMT_A8B8G8R8
DDS_RGBA 	32 	0xff 	0xff00 	0xff0000 	0xff000000
DXGI_FORMAT_R16G16_UNORM
D3DFMT_G16R16
	DDS_RGBA 	32 	0xffff 	0xffff0000
**
DXGI_FORMAT_R10G10B10A2_UNORM
D3DFMT_A2B10G10R10
	DDS_RGBA 	32 	0x3ff 	0xffc00 	0x3ff00000
DXGI_FORMAT_R16G16_UNORM
D3DFMT_G16R16
	DDS_RGB 	32 	0xffff 	0xffff0000
DXGI_FORMAT_B5G5R5A1_UNORM
D3DFMT_A1R5G5B5
	DDS_RGBA 	16 	0x7c00 	0x3e0 	0x1f 	0x8000
DXGI_FORMAT_B5G6R5_UNORM
D3FMT_R5G6B5
	DDS_RGB 	16 	0xf800 	0x7e0 	0x1f
DXGI_A8_UNORM
D3DFMT_A8
	DDS_ALPHA 	8 				0xff
D3DFMT_A8R8G8B8
	DDS_RGBA 	32 	0xff0000 	0xff00 	0xff 	0xff000000
D3DFMT_X8R8G8B8
	DDS_RGB 	32 	0xff0000 	0xff00 	0xff
D3DFMT_X8B8G8R8
	DDS_RGB 	32 	0xff 	0xff00 	0xff0000
**
D3DFMT_A2R10G10B10
	DDS_RGBA 	32 	0x3ff00000 	0xffc00 	0x3ff 	0xc0000000
D3DFMT_R8G8B8
	DDS_RGB 	24 	0xff0000 	0xff00 	0xff
D3DFMT_X1R5G5B5
	DDS_RGB 	16 	0x7c00 	0x3e0 	0x1f
D3DFMT_A4R4G4B4
	DDS_RGBA 	16 	0xf00 	0xf0 	0xf 	0xf000
D3DFMT_X4R4G4B4
	DDS_RGB 	16 	0xf00 	0xf0 	0xf
D3DFMT_A8R3G3B2
	DDS_RGBA 	16 	0xe0 	0x1c 	0x3 	0xff00
D3DFMT_A8L8
	DDS_LUMINANCE 	16 	0xff 			0xff00
D3DFMT_L16
	DDS_LUMINANCE 	16 	0xffff
D3DFMT_L8
	DDS_LUMINANCE 	8 	0xff
D3DFMT_A4L4
	DDS_LUMINANCE 	8 	0xf 			0xf0

Resource Format 	dwFlags 	dwFourCC
DXGI_FORMAT_BC1_UNORM
D3DFMT_DXT1
	DDS_FOURCC 	"DXT1"
DXGI_FORMAT_BC2_UNORM
D3DFMT_DXT3
	DDS_FOURCC 	"DXT3"
DXGI_FORMAT_BC3_UNORM
D3DFMT_DXT5
	DDS_FOURCC 	"DXT5"
*
DXGI_FORMAT_BC4_UNORM
	DDS_FOURCC 	"BC4U"
*
DXGI_FORMAT_BC4_SNORM
	DDS_FOURCC 	"BC4S"
*
DXGI_FORMAT_BC5_UNORM
	DDS_FOURCC 	"ATI2"
*
DXGI_FORMAT_BC5_SNORM
	DDS_FOURCC 	"BC5S"
DXGI_FORMAT_R8G8_B8G8_UNORM
D3DFMT_R8G8_B8G8
	DDS_FOURCC 	"RGBG"
DXGI_FORMAT_G8R8_G8B8_UNORM
D3DFMT_G8R8_G8B8
	DDS_FOURCC 	"GRGB"
*
DXGI_FORMAT_R16G16B16A16_UNORM
D3DFMT_A16B16G16R16
	DDS_FOURCC 	36
*
DXGI_FORMAT_R16G16B16A16_SNORM
D3DFMT_Q16W16V16U16
	DDS_FOURCC 	110
*
DXGI_FORMAT_R16_FLOAT
D3DFMT_R16F
	DDS_FOURCC 	111
*
DXGI_FORMAT_R16G16_FLOAT
D3DFMT_G16R16F
	DDS_FOURCC 	112
*
DXGI_FORMAT_R16G16B16A16_FLOAT
D3DFMT_A16B16G16R16F
	DDS_FOURCC 	113
*
DXGI_FORMAT_R32_FLOAT
D3DFMT_R32F
	DDS_FOURCC 	114
*
DXGI_FORMAT_R32G32_FLOAT
D3DFMT_G32R32F
	DDS_FOURCC 	115
*
DXGI_FORMAT_R32G32B32A32_FLOAT
D3DFMT_A32B32G32R32F
	DDS_FOURCC 	116
D3DFMT_DXT2
	DDS_FOURCC 	"DXT2"
D3DFMT_DXT4
	DDS_FOURCC 	"DXT4"
D3DFMT_UYVY
	DDS_FOURCC 	"UYVY"
D3DFMT_YUY2
	DDS_FOURCC 	"YUY2"
D3DFMT_CxV8U8
	DDS_FOURCC 	117
Any DXGI format 	DDS_FOURCC 	"DX10"
*/

namespace raptor {

static VkFormat translate_dxgi_format( DXGI_FORMAT format ) {
    switch (format) {
        case DXGI_FORMAT_BC7_UNORM_SRGB:
        case DXGI_FORMAT_BC7_UNORM:
            return VK_FORMAT_BC7_UNORM_BLOCK;
        default:
            rprint( "Unsupported DDS DXGI format: %u\n", ( u32 )format );
            return VK_FORMAT_UNDEFINED;
    }
}

u64 dds_mip_size( u32 width, u32 height ) {
    // TODO(marco): get correct block size based on format
    return u64( ( width + 3 ) / 4 ) * ( ( height + 3 ) / 4 ) * 16;
}

static bool read_dds_header( FileHandle handle, int& width, int& height, u32& mip_levels, u32& size, VkFormat& format ) {
    u32 magic = 0;
    DDS_HEADER header{};
    if ( file_read( &magic, sizeof( magic ), 1, handle ) != 1 || magic != 0x20534444) {
        return false;
    }

    if ( file_read( &header, sizeof( header ), 1, handle ) != 1 ||
         header.dwSize != 124 || header.ddspf.dwSize != 32 ) {
        return false;
    }

    if ( ( header.ddspf.dwFlags & DDPF_FOURCC ) == DDPF_FOURCC ) {
        if ( header.ddspf.dwFourCC == dx10_fourcc  ) {
            DDS_HEADER_DXT10 dx10{};

            if ( file_read( &dx10, sizeof( dx10 ), 1, handle ) != 1 ) {
                return false;
            }

            if ( dx10.resourceDimension != D3D10_RESOURCE_DIMENSION_TEXTURE2D || dx10.arraySize != 1 ) {
                return false;
            }

            format = translate_dxgi_format( dx10.dxgiFormat );
            if ( format == VK_FORMAT_UNDEFINED ) {
                return false;
            }
        }
    } else {
        // TODO(marco): uncompressed data
        RASSERT( false );
        return false;
    }

    width = ( int )header.dwWidth;
    height = ( int )header.dwHeight;
    mip_levels = ( header.dwFlags & DDSD_MIPMAPCOUNT ) == DDSD_MIPMAPCOUNT ? raptor::max( 1u, ( u32 )header.dwMipMapCount ) : 1;

    // Compute every mip from its block dimensions; pitch/linear size is optional metadata.
    u64 total_size = 0;
    for ( u32 mip = 0; mip < mip_levels; ++mip ) {
        total_size += dds_mip_size( raptor::max( 1u, ( u32 )header.dwWidth >> mip ),
                                    raptor::max( 1u, ( u32 )header.dwHeight >> mip ) );
    }
    size = ( u32 )total_size;
    return file_size( handle ) >= file_tell( handle ) + size;
}

bool dds_info( cstring path, int& width, int& height, u32& mip_levels, u32& size, VkFormat& format ) {
    width = height = 0;
    mip_levels = size = 0;
    format = VK_FORMAT_UNDEFINED;

    FileHandle handle{};
    file_open( path, "rb", &handle );
    if ( !handle ) {
        rprint( "Failed to read DDS %s\n", path );
        return false;
    }

    const bool valid = read_dds_header( handle, width, height, mip_levels, size, format );
    file_close( handle );

    if ( !valid ) {
        width = height = 0;
        mip_levels = size = 0;
        format = VK_FORMAT_UNDEFINED;
        rprint( "Invalid or unsupported DDS: %s\n", path );
    }
    return valid;
}

u8* dds_load( cstring path, u32 size ) {
    FileHandle handle{};
    file_open( path, "rb", &handle );
    if ( !handle ) {
        rprint( "Failed to read DDS %s\n", path );
        return nullptr;
    }

    int width, height;
    u32 mip_levels, expected_size;
    VkFormat format;
    if ( !read_dds_header( handle, width, height, mip_levels, expected_size, format ) || expected_size != size ) {
        rprint( "Invalid DDS payload: %s\n", path );
        file_close( handle );
        return nullptr;
    }

    u8* data = ( u8* )malloc( size );
    if ( data && file_read( data, 1, size, handle ) != size ) {
        free( data );
        data = nullptr;
    }
    file_close( handle );

    return data;
}

void dds_free( u8* data ) {
    free( data );
}

} // namespace raptor
