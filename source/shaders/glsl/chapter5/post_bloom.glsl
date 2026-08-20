#version 460

#extension GL_GOOGLE_include_directive : enable

#include "platform.glslh"

struct GpuBloomConstants {
    uint                source_texture_index;
    uint                destination_texture_index;
    float               filter_radius;
    uint                pad001;

    vec2                rcp_source_texture_size;
    vec2                rcp_destination_texture_size;
};

layout(std140, set = MATERIAL_SET, binding = 1) uniform bloom_locals {
    GpuBloomConstants   bloom;
};

#if defined(COMPUTE_BLOOM_DOWNSAMPLE)

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
void main() {
    ivec3 pos = ivec3(gl_GlobalInvocationID.xyz);
    vec2 uv = uv_from_compute_dispatch(pos.xy, bloom.rcp_destination_texture_size);
    //image2D source_image = global_images_2d[nonuniformEXT(bloom.source_texture_index)];

    float x = bloom.rcp_source_texture_size.x;
    float y = bloom.rcp_source_texture_size.y;

    // Take 13 samples around current texel:
    // a - b - c
    // - j - k -
    // d - e - f
    // - l - m -
    // g - h - i
    // === ('e' is the current texel) ===
    vec3 a = textureLod(global_textures[nonuniformEXT(bloom.source_texture_index)], vec2(uv.x - 2*x, uv.y + 2*y), 0).rgb;
    vec3 b = textureLod(global_textures[nonuniformEXT(bloom.source_texture_index)], vec2(uv.x,       uv.y + 2*y), 0).rgb;
    vec3 c = textureLod(global_textures[nonuniformEXT(bloom.source_texture_index)], vec2(uv.x + 2*x, uv.y + 2*y), 0).rgb;
    vec3 d = textureLod(global_textures[nonuniformEXT(bloom.source_texture_index)], vec2(uv.x - 2*x, uv.y), 0).rgb;
    vec3 e = textureLod(global_textures[nonuniformEXT(bloom.source_texture_index)], vec2(uv.x,       uv.y), 0).rgb;
    vec3 f = textureLod(global_textures[nonuniformEXT(bloom.source_texture_index)], vec2(uv.x + 2*x, uv.y), 0).rgb;
    vec3 g = textureLod(global_textures[nonuniformEXT(bloom.source_texture_index)], vec2(uv.x - 2*x, uv.y - 2*y), 0).rgb;
    vec3 h = textureLod(global_textures[nonuniformEXT(bloom.source_texture_index)], vec2(uv.x,       uv.y - 2*y), 0).rgb;
    vec3 i = textureLod(global_textures[nonuniformEXT(bloom.source_texture_index)], vec2(uv.x + 2*x, uv.y - 2*y), 0).rgb;
    vec3 j = textureLod(global_textures[nonuniformEXT(bloom.source_texture_index)], vec2(uv.x - x,   uv.y + y), 0).rgb;
    vec3 k = textureLod(global_textures[nonuniformEXT(bloom.source_texture_index)], vec2(uv.x + x,   uv.y + y), 0).rgb;
    vec3 l = textureLod(global_textures[nonuniformEXT(bloom.source_texture_index)], vec2(uv.x - x,   uv.y - y), 0).rgb;
    vec3 m = textureLod(global_textures[nonuniformEXT(bloom.source_texture_index)], vec2(uv.x + x,   uv.y - y), 0).rgb;

    // Apply weighted distribution:
    // 0.5 + 0.125 + 0.125 + 0.125 + 0.125 = 1
    // a,b,d,e * 0.125
    // b,c,e,f * 0.125
    // d,e,g,h * 0.125
    // e,f,h,i * 0.125
    // j,k,l,m * 0.5
    // This shows 5 square areas that are being sampled. But some of them overlap,
    // so to have an energy preserving downsample we need to make some adjustments.
    // The weights are the distributed, so that the sum of j,k,l,m (e.g.)
    // contribute 0.5 to the final color output. The code below is written
    // to effectively yield this sum. We get:
    // 0.125*5 + 0.03125*4 + 0.0625*4 = 1
    vec3 downsample = e*0.125;
    downsample += (a+c+g+i)*0.03125;
    downsample += (b+d+f+h)*0.0625;
    downsample += (j+k+l+m)*0.125;

    // NaN protection
    downsample = max(downsample, 0.0001f);

    vec4 color = vec4(downsample, 1.f);
    //color = vec4(uv, 0, 1.0f);

    imageStore(global_images_2d[bloom.destination_texture_index], pos.xy, color);
}

#endif // COMPUTE_BLOOM_DOWNSAMPLE

#if defined(COMPUTE_BLOOM_UPSAMPLE)

layout(rgba16f, set = GLOBAL_SET, binding = BINDLESS_IMAGES ) uniform image2D global_images_2drw[];

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
void main() {
    ivec3 pos = ivec3(gl_GlobalInvocationID.xyz);
    vec2 uv = uv_from_compute_dispatch(pos.xy, bloom.rcp_destination_texture_size);

    vec3 current_color = imageLoad(global_images_2drw[bloom.destination_texture_index], pos.xy).rgb;

    // The filter kernel is applied with a radius, specified in texture
    // coordinates, so that the radius will vary across mip resolutions.
    // filter_radius in texels
    vec2 texel_radius = bloom.rcp_source_texture_size * bloom.filter_radius;
    float x = texel_radius.x;
    float y = texel_radius.y;

    // Take 9 samples around current texel:
    // a - b - c
    // d - e - f
    // g - h - i
    // === ('e' is the current texel) ===
    vec3 a = textureLod(global_textures[nonuniformEXT(bloom.source_texture_index)], vec2(uv.x - x, uv.y + y), 0).rgb;
    vec3 b = textureLod(global_textures[nonuniformEXT(bloom.source_texture_index)], vec2(uv.x,     uv.y + y), 0).rgb;
    vec3 c = textureLod(global_textures[nonuniformEXT(bloom.source_texture_index)], vec2(uv.x + x, uv.y + y), 0).rgb;
    vec3 d = textureLod(global_textures[nonuniformEXT(bloom.source_texture_index)], vec2(uv.x - x, uv.y), 0).rgb;
    vec3 e = textureLod(global_textures[nonuniformEXT(bloom.source_texture_index)], vec2(uv.x,     uv.y), 0).rgb;
    vec3 f = textureLod(global_textures[nonuniformEXT(bloom.source_texture_index)], vec2(uv.x + x, uv.y), 0).rgb;
    vec3 g = textureLod(global_textures[nonuniformEXT(bloom.source_texture_index)], vec2(uv.x - x, uv.y - y), 0).rgb;
    vec3 h = textureLod(global_textures[nonuniformEXT(bloom.source_texture_index)], vec2(uv.x,     uv.y - y), 0).rgb;
    vec3 i = textureLod(global_textures[nonuniformEXT(bloom.source_texture_index)], vec2(uv.x + x, uv.y - y), 0).rgb;

    // Apply weighted distribution, by using a 3x3 tent filter:
    //  1   | 1 2 1 |
    // -- * | 2 4 2 |
    // 16   | 1 2 1 |
    vec3 upsample = e*4.0;
    upsample += (b+d+f+h)*2.0;
    upsample += (a+c+g+i);
    upsample *= 1.0 / 16.0;

    vec4 color = vec4(upsample + current_color, 1.0f);
    imageStore(global_images_2d[bloom.destination_texture_index], pos.xy, color);
}

#endif // COMPUTE_BLOOM_UPSAMPLE