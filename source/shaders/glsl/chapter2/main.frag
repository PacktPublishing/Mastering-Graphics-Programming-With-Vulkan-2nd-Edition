#version 450

uint DrawFlags_AlphaMask = 1 << 0;
uint DrawFlags_HasTangents = 1 << 6;

struct DrawData {
    mat4        model;
    mat4        model_inverse;

    // x = diffuse index, y = roughness index, z = normal index, w = occlusion index.
    uvec4       textures;
    vec4        base_color_factor;
    vec4        metallic_roughness_occlusion_factor;
    float       alpha_cutoff;
    uint        flags;
    uint        padding_0;
    uint        padding_1;
};

layout ( std430, set = 1, binding = 0 ) readonly buffer SceneBuffer {
    mat4        view_projection;
    vec4        eye;
    vec4        light;
    float       light_range;
    float       light_intensity;
    uint        padding_0;
    uint        padding_1;
    DrawData    draws[];
} scene;

// Bindless support
// Enable non uniform qualifier extension
#extension GL_EXT_nonuniform_qualifier : enable
// Global bindless support. This should go in a common file.

layout ( set = 0, binding = 10 ) uniform sampler2D global_textures[];
// Alias textures to use the same binding point, as bindless texture is shared
// between all kind of textures: 1d, 2d, 3d.
layout ( set = 0, binding = 10 ) uniform sampler3D global_textures_3d[];


layout (location = 0) in vec2 vTexcoord0;
layout (location = 1) in vec3 vNormal;
layout (location = 2) in vec3 vTangent;
layout (location = 3) in vec3 vBiTangent;
layout (location = 4) in vec3 vPosition;
layout (location = 5) flat in uint vDrawIndex;

layout (location = 0) out vec4 frag_color;

#define PI 3.1415926538
#define INVALID_TEXTURE_INDEX 65535

vec3 decode_srgb( vec3 c ) {
    vec3 result;
    if ( c.r <= 0.04045) {
        result.r = c.r / 12.92;
    } else {
        result.r = pow( ( c.r + 0.055 ) / 1.055, 2.4 );
    }

    if ( c.g <= 0.04045) {
        result.g = c.g / 12.92;
    } else {
        result.g = pow( ( c.g + 0.055 ) / 1.055, 2.4 );
    }

    if ( c.b <= 0.04045) {
        result.b = c.b / 12.92;
    } else {
        result.b = pow( ( c.b + 0.055 ) / 1.055, 2.4 );
    }

    return clamp( result, 0.0, 1.0 );
}

vec3 encode_srgb( vec3 c ) {
    vec3 result;
    if ( c.r <= 0.0031308) {
        result.r = c.r * 12.92;
    } else {
        result.r = 1.055 * pow( c.r, 1.0 / 2.4 ) - 0.055;
    }

    if ( c.g <= 0.0031308) {
        result.g = c.g * 12.92;
    } else {
        result.g = 1.055 * pow( c.g, 1.0 / 2.4 ) - 0.055;
    }

    if ( c.b <= 0.0031308) {
        result.b = c.b * 12.92;
    } else {
        result.b = 1.055 * pow( c.b, 1.0 / 2.4 ) - 0.055;
    }

    return clamp( result, 0.0, 1.0 );
}

float heaviside( float v ) {
    if ( v > 0.0 ) return 1.0;
    else return 0.0;
}

vec3 diffuse_brdf( vec3 color ) {
    return ( 1.0 / PI ) * color;
}

vec3 specular_brdf( float alpha, vec3 N, vec3 L, vec3 H, vec3 V ) {
    float alpha_squared = alpha * alpha;
    float NdotH = clamp( dot( N, H ), 0, 1.0 );
    float NdotL = clamp( dot( N, L ), 0, 1.0 );
    float NdotV = clamp( dot( N, V ), 0, 1.0 );
    float HdotL = clamp( dot( H, L ), 0, 1.0 );
    float HdotV = clamp( dot( H, V ), 0, 1.0 );
    float ggx = ( alpha_squared * heaviside( NdotH ) ) /
        ( PI * (
            pow( ( ( NdotH * NdotH ) * ( alpha_squared - 1.0 ) + 1 ), 2 )
        ) );

    float visiblity = ( heaviside( HdotL ) / ( abs( NdotL ) + sqrt( alpha_squared + ( 1 - alpha_squared ) * ( NdotL * NdotL ) ) ) ) *
                      ( heaviside( HdotV ) / ( abs( NdotV ) + sqrt( alpha_squared + ( 1 - alpha_squared ) * ( NdotV * NdotV ) ) ) );

    return vec3( visiblity * ggx );
}

void main() {
    DrawData draw_data = scene.draws[ vDrawIndex ];

    vec4 base_colour = texture( global_textures[ nonuniformEXT( draw_data.textures.x ) ], vTexcoord0 ) *
                       draw_data.base_color_factor;

    bool useAlphaMask = ( draw_data.flags & DrawFlags_AlphaMask ) != 0;
    if ( useAlphaMask && base_colour.a < draw_data.alpha_cutoff ) {
        base_colour.a = 0.0;
    }

    vec3 normal = normalize( vNormal );
    vec3 tangent = normalize( vTangent );
    vec3 bitangent = normalize( vBiTangent );

    if ( ( draw_data.flags & DrawFlags_HasTangents ) == 0 ) {
        // NOTE(marco): taken from https://community.khronos.org/t/computing-the-tangent-space-in-the-fragment-shader/52861
        vec3 Q1 = dFdx( vPosition.xyz );
        vec3 Q2 = dFdy( vPosition.xyz );
        vec2 st1 = dFdx( vTexcoord0 );
        vec2 st2 = dFdy( vTexcoord0 );

        tangent = normalize(  Q1 * st2.t - Q2 * st1.t );
        tangent = normalize( mat3( draw_data.model ) * tangent );
        bitangent = normalize( -Q1 * st2.s + Q2 * st1.s );
    }

    if (gl_FrontFacing == false)
    {
        tangent *= -1.0;
        bitangent *= -1.0;
        normal *= -1.0;
    }

    if ( draw_data.textures.z != INVALID_TEXTURE_INDEX ) {
        // NOTE(marco): normal textures are encoded to [0, 1] but need to be mapped to [-1, 1] value
        vec3 bump_normal = normalize( texture( global_textures[ nonuniformEXT( draw_data.textures.z ) ], vTexcoord0 ).rgb * 2.0 - 1.0 );
        mat3 TBN = mat3(
            tangent,
            bitangent,
            normal
        );

        normal = normalize(TBN * normalize(bump_normal));
    }

    vec3 V = normalize( scene.eye.xyz - vPosition );
    vec3 L = normalize( scene.light.xyz - vPosition );
    vec3 N = normal;
    vec3 H = normalize( L + V );

    float metalness = draw_data.metallic_roughness_occlusion_factor.x;
    float roughness = draw_data.metallic_roughness_occlusion_factor.y;

    if ( draw_data.textures.y != INVALID_TEXTURE_INDEX ) {
        vec4 rm = texture( global_textures[ nonuniformEXT( draw_data.textures.y ) ], vTexcoord0 );

        // Green channel contains roughness values
        roughness *= rm.g;

        // Blue channel contains metalness
        metalness *= rm.b;
    }

    float alpha = pow(roughness, 2.0);

    float occlusion = draw_data.metallic_roughness_occlusion_factor.z;
    if ( draw_data.textures.w != INVALID_TEXTURE_INDEX ) {
        vec4 o = texture( global_textures[ nonuniformEXT( draw_data.textures.w ) ], vTexcoord0 );
        // Red channel for occlusion value
        occlusion *= o.r;
    }

    base_colour.rgb = decode_srgb( base_colour.rgb );

    // https://www.khronos.org/registry/glTF/specs/2.0/glTF-2.0.html#specular-brdf
    float alpha_squared = alpha * alpha;

    float NdotL = dot(N, L);
    float HdotV = clamp( dot(H, L), 0, 1 );

    float distance = length( scene.light.xyz - vPosition );
    float intensity = scene.light_intensity *
                      max( min( 1.0 - pow( distance / scene.light_range, 4.0 ), 1.0 ), 0.0 ) /
                      pow( distance, 2.0 );

    vec3 material_colour = vec3(0, 0, 0);
    if (NdotL > 0.0)
    {
        vec3 specular = intensity * NdotL * specular_brdf( alpha_squared, N, L, H, V );
        float coeff = pow( 1 - abs( HdotV ), 5 );
        vec3 metal_brdf =  specular * ( base_colour.rgb + ( 1 - base_colour.rgb ) * coeff );

        // assumes IOR = 1.5
        float fresnel_mix = 0.04 + ( 1 - 0.04 ) * coeff;
        vec3 diffuse = intensity * NdotL * diffuse_brdf( base_colour.rgb );
        vec3 dielectric_brdf = mix( diffuse, specular, fresnel_mix );

        material_colour = mix( dielectric_brdf, metal_brdf, metalness );
    }

    frag_color = vec4( encode_srgb( material_colour ), base_colour.a );
}
