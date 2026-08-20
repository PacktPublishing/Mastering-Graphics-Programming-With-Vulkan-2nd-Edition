#version 460

#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_ray_tracing : enable

#include "platform.glslh"

#if defined(VERTEX_MAIN_TRIANGLE) || defined (VERTEX_MAIN_POST)

layout (location = 0) out vec2 vTexCoord;
layout (location = 1) flat out uint out_texture_id;

void main() {

    vTexCoord.xy = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(vTexCoord.xy * 2.0f - 1.0f, 0.0f, 1.0f);
    gl_Position.y = -gl_Position.y;

    out_texture_id = gl_InstanceIndex;
}

#endif // VERTEX_MAIN

#if defined(FRAGMENT_MAIN_TRIANGLE)

layout (location = 0) in vec2 vTexCoord;
layout (location = 1) flat in uint texture_id;

layout (location = 0) out vec4 out_color;

void main() {
    vec4 color = texture(global_textures[nonuniformEXT(texture_id)], vTexCoord.xy);
    out_color = color;
}

#endif // FRAGMENT_MAIN


#if defined(FRAGMENT_MAIN_POST)

#include "../shared_structs.h"

layout (location = 0) in vec2 vTexCoord;
layout (location = 1) flat in uint texture_id;

layout (location = 0) out vec4 out_color;


layout ( std140, set = MATERIAL_SET, binding = 11 ) uniform PostConstants {
    GpuPostProcessConstants post;
};

// All filters taken from https://www.shadertoy.com/view/WdjSW3
vec3 rrt_odt_fit(vec3 v) {
    vec3 a = v*(         v + 0.0245786) - 0.000090537;
    vec3 b = v*(0.983729*v + 0.4329510) + 0.238081;
    return a/b;
}

mat3 mat3_from_rows(vec3 c0, vec3 c1, vec3 c2) {
    mat3 m = mat3(c0, c1, c2);
    m = transpose(m);

    return m;
}

vec3 aces_fitted(vec3 color) {
    mat3 ACES_INPUT_MAT = mat3(0.59719, 0.076, 0.0284, 0.35458, 0.90834, 0.13383, 0.04823, 0.01566, 0.83777);
    mat3 ACES_OUTPUT_MAT = mat3(1.60475, -0.10208, -0.00327, -0.53108, 1.10813, -0.07276, -0.07367, -0.00605, 1.07602);

    color = ACES_INPUT_MAT * color;

    // Apply RRT and ODT
    color = rrt_odt_fit(color);

    color = ACES_OUTPUT_MAT * color;

    color = saturate(color);

    return color;
}

float sd_box( in vec2 p, in vec2 b ) {
    vec2 d = abs(p)-b;
    return length(max(d,0.0)) + min(max(d.x,d.y),0.0);
}

void main() {
    vec4 color = texture(global_textures[nonuniformEXT(texture_id)], vTexCoord.xy);
    color.rgb *= post.exposure;

    // Safe input luminance
    float input_luminance = max( luminance(color.rgb), 0.0001f );
    float average_luminance = 0.f;

    vec2 rcp_resolution = vec2(1.f / post.resolution_x, 1.f / post.resolution_y );

    // Sharpen
    for (int x = -1; x <= 1; ++x ) {
        for (int y = -1; y <= 1; ++y ) {
            vec3 sampled_color = texture(global_textures[nonuniformEXT(texture_id)], 
                vTexCoord.xy + vec2( x * rcp_resolution.x, y * rcp_resolution.y )).rgb;
            
            sampled_color *= post.exposure;
            average_luminance += luminance( sampled_color );
        }
    }

    average_luminance /= 9.0f;

    float sharpened_luminance = input_luminance - average_luminance;
    float final_luminance = input_luminance + sharpened_luminance * post.sharpening_amount;
    color.rgb = color.rgb * (final_luminance / input_luminance);

    // Apply bloom
    if (post.bloom_amount > 0.f) {
        vec3 bloom = texture(global_textures[nonuniformEXT(post.bloom_texture_index)], vTexCoord.xy).rgb;
        color.rgb += bloom * post.bloom_amount;
    }

    // Tonemapping
    switch (post.tonemap_type) {
        case 1:
            color.rgb = aces_fitted(color.rgb);
            break;
    }

    // Add a zoom rectangle to debug pixels
    if ( post.enable_zoom > 0 ) {
        vec2 uv = vTexCoord.xy;
        float rect_size = 0.07f;
        vec2 aspect_ratio = vec2( 1.0, post.resolution_x * 1.f / post.resolution_y );
        float rect_dist = sd_box( post.mouse_uv - uv, rect_size * aspect_ratio );
        if (rect_dist < rect_size)
             color = texture(global_textures[nonuniformEXT(texture_id)], (vTexCoord.xy + post.mouse_uv) / post.zoom_scale );
    }

    out_color = color;
}

#endif // FRAGMENT_MAIN


#if defined(COMPUTE_BILATERAL_WEIGHTS)


layout( push_constant ) uniform PushConstants {

    uint        depth_pyramid_texture_index;
    uint        output_index;
};

float gaussian(float sigma, float x) {
    return exp(-(x*x) / (2.0 * sigma*sigma));
}

void bilateral_weight_write_output(ivec2 pos, vec2 value) {
    imageStore( global_images_2d[output_index], pos.xy, vec4(value, 0, 0) );
}

// Gather layout
// w z
// x y

#define GATHER_UOFFSET_X vec2(0, 1)
#define GATHER_UOFFSET_Y vec2(1, 1)
#define GATHER_UOFFSET_Z vec2(1, 0)
#define GATHER_UOFFSET_W vec2(0, 0)


#define GATHER_SOFFSET_X vec2(-1, 1)
#define GATHER_SOFFSET_Y vec2(1, 1)
#define GATHER_SOFFSET_Z vec2(1, -1)
#define GATHER_SOFFSET_W vec2(-1,-1)

void ProcessQuad(vec4 lowResSamples, float origDepth, inout vec2 offsets, inout float minDepthDiff) {
    vec4 depthDiff = abs(lowResSamples - vec4(origDepth));

    if (depthDiff.x < minDepthDiff)
    {
        minDepthDiff = depthDiff.x;
        offsets = GATHER_SOFFSET_X * 0.5f;
    }

    if (depthDiff.y < minDepthDiff)
    {
        minDepthDiff = depthDiff.y;
        offsets = GATHER_SOFFSET_Y * 0.5f;
    }

    if (depthDiff.z < minDepthDiff)
    {
        minDepthDiff = depthDiff.z;
        offsets = GATHER_SOFFSET_Z * 0.5f;
    }

    if (depthDiff.w < minDepthDiff)
    {
        minDepthDiff = depthDiff.w;
        offsets = GATHER_SOFFSET_W * 0.5f;
    }
}

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
void main() {
    ivec3 pos = ivec3(gl_GlobalInvocationID.xyz);

    vec2 uv = uv_nearest( pos.xy, frame.resolution );

    vec4 low_resolution_depths = textureGather(global_textures[nonuniformEXT(depth_pyramid_texture_index)], uv, 0);
    float full_resolution_depth = texelFetch(global_textures[nonuniformEXT(frame.depth_texture_index)], pos.xy, 0).r;

    vec4 depth_difference = abs(low_resolution_depths - vec4(full_resolution_depth));
    vec2 fullResOffsets = vec2((gl_GlobalInvocationID.x & 1) == 1 ? 0.25 : -0.25, (gl_GlobalInvocationID.y & 1) == 1 ? 0.25 : -0.25);
    vec4 depthDiffNormalized = depth_difference / (max(full_resolution_depth, 0.000001f));
    const float acceptanceThreshold = 0.03f;

    bvec4 test = bvec4(depthDiffNormalized.x < acceptanceThreshold,
                       depthDiffNormalized.y < acceptanceThreshold,
                       depthDiffNormalized.z < acceptanceThreshold,
                       depthDiffNormalized.w < acceptanceThreshold);
    // first, check if bilinear is ok
    if (all(test)) {
        //return float2(0.0f, 0.0f);
        bilateral_weight_write_output(pos.xy, vec2(0,0));
    }

    const float BILAT_PACK = 0.25f;
    // then check edges if bilinear on 1 edge is ok
    // w z
    // x y
    if (all(test.wz)) {
        //return float2(0.0f, -0.5f + fullResOffsets.y) * BILAT_PACK;
        bilateral_weight_write_output(pos.xy, vec2(0, -0.5f + fullResOffsets.y) * BILAT_PACK);
        return;
    }
    if (all(test.xy)) {
        //return float2(0.0f, 0.5f + fullResOffsets.y) * BILAT_PACK;
        bilateral_weight_write_output(pos.xy, vec2(0, 0.5f + fullResOffsets.y) * BILAT_PACK);
        return;
    }
    if (all(test.wx)) {
        //return float2(-0.5f + fullResOffsets.x, 0.0f) * BILAT_PACK;
        bilateral_weight_write_output(pos.xy, vec2(-0.5f + fullResOffsets.x,0) * BILAT_PACK);
        return;
    }
    if (all(test.zy)) {
        //return float2(0.5f + fullResOffsets.x, 0.0f) * BILAT_PACK;
        bilateral_weight_write_output(pos.xy, vec2(0.5f + fullResOffsets.x,0) * BILAT_PACK);
        return;
    }


    float smallestDepthDiff = 10000.0f;
    vec2 smallResOffsets = vec2(0.0f);

    // finally, find closest point
    ProcessQuad(low_resolution_depths, full_resolution_depth, smallResOffsets, smallestDepthDiff);

    bilateral_weight_write_output(pos.xy, (fullResOffsets + smallResOffsets) * BILAT_PACK);
}

#endif // COMPUTE_BILATERAL_WEIGHTS
