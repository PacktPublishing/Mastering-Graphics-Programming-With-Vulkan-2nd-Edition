
#if !defined(__SHARED_TYPES_H__)
#define __SHARED_TYPES_H__

#if defined (__cplusplus)

using uint 		= uint32_t;
using float4x4 	= mat4s;

using float3 	= vec3s;
using float4 	= vec4s;

// Alignment helper for shared structs
#define ALIGN16 alignas(16)

#elif defined (RAPTOR_GLSL)

#define ALIGN16

#elif defined (RAPTOR_SLANG)

#define ALIGN16

#else

#endif // __cplusplus

#endif // __SHARED_TYPES_H__
