
#if !defined(__SHARED_CONVERTER_H__)
#define __SHARED_CONVERTER_H__

#if defined (__cplusplus)

#include <cstdint>

#define uint uint32_t
#define float4x4 mat4s

#define float3 vec3s
#define float4 vec4s

struct alignas( 16 ) uint4 {
	uint32_t x;
	uint32_t y;
	uint32_t z;
	uint32_t w;

	uint32_t& operator[]( uint32_t index ) {
		return ( &x )[ index ];
	}

	const uint32_t& operator[]( uint32_t index ) const {
		return ( &x )[ index ];
	}
};

#else

#endif // __cplusplus

#endif // __SHARED_CONVERTER_H__


#if defined (__cplusplus)

namespace gpu {

#else

#endif // __cplusplus
