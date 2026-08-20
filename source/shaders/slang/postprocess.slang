
#if defined(VERTEX_MAIN_TRIANGLE_SLANG) || defined (VERTEX_MAIN_POST_SLANG)

#include "platform.slangh"

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

[shader("vertex")]
VertexOutput main(uint vertexId : SV_VertexID) {
    VertexOutput output;

    output.uv.xy = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(output.uv.xy * 2.0f - 1.0f, 0.0f, 1.0f);
    output.position.y = -output.position.y;

    return output;
}

#endif


#if defined(FRAGMENT_MAIN_POST_SLANG)

#include "platform.slangh"
#include "../shared_structs.h"

struct FragmentInput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

//layout ( std140, set = MATERIAL_SET, binding = 11 ) uniform PostConstants {
ConstantBuffer<GpuPostProcessConstants> post : register(b11, space1);


// All filters taken from https://www.shadertoy.com/view/WdjSW3
float3 rrt_odt_fit(float3 v) {
    float3 a = v*(         v + 0.0245786) - 0.000090537;
    float3 b = v*(0.983729*v + 0.4329510) + 0.238081;
    return a/b;
}



float3 aces_fitted(float3 color) {
    float3x3 ACES_INPUT_MAT = float3x3(0.59719, 0.076, 0.0284, 0.35458, 0.90834, 0.13383, 0.04823, 0.01566, 0.83777);
    float3x3 ACES_OUTPUT_MAT = float3x3(1.60475, -0.10208, -0.00327, -0.53108, 1.10813, -0.07276, -0.07367, -0.00605, 1.07602);

    color = mul(color, ACES_INPUT_MAT);

    // Apply RRT and ODT
    color = rrt_odt_fit(color);

    color = mul(color, ACES_OUTPUT_MAT);

    color = saturate(color);

    return color;
}

float sd_box( in float2 p, in float2 b ) {
    float2 d = abs(p)-b;
    return length(max(d,0.0)) + min(max(d.x,d.y),0.0);
}


[shader("fragment")]
float4 main(FragmentInput input) : SV_Target {

    float4 color = texture_2d( post.input_texture_index ).SampleLevel(g_linear_clamp(), input.uv, 0);

    color.rgb *= post.exposure;

    float input_luminance = luminance(color.rgb);
    float average_luminance = 0.f;

    float2 rcp_resolution = float2(1.f / post.resolution_x, 1.f / post.resolution_y );

    // Sharpen
    for (int x = -1; x <= 1; ++x ) {
        for (int y = -1; y <= 1; ++y ) {
            float3 sampled_color = texture_2d( post.input_texture_index ).SampleLevel(g_linear_clamp(), input.uv + float2( x * rcp_resolution.x, y * rcp_resolution.y ), 0).rgb;
            // Apply exposure
            sampled_color *= post.exposure;
            average_luminance += luminance( sampled_color );
        }
    }

    average_luminance /= 9.0f;

    float sharpened_luminance = input_luminance - average_luminance;
    float final_luminance = input_luminance + sharpened_luminance * post.sharpening_amount;
    color.rgb = color.rgb * (final_luminance / input_luminance);

    if (post.bloom_amount > 0.f) {
        float3 bloom = texture_2d( post.bloom_texture_index ).SampleLevel(g_linear_clamp(), input.uv, 0).rgb;
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
        float2 uv = input.uv.xy;
        float rect_size = 0.07f;
        float2 aspect_ratio = float2( 1.0, post.resolution_x * 1.f / post.resolution_y );
        float rect_dist = sd_box( post.mouse_uv - uv, rect_size * aspect_ratio );
        if (rect_dist < rect_size) {
            color = texture_2d( post.input_texture_index ).SampleLevel(g_linear_clamp(), ( input.uv + post.mouse_uv ) / post.zoom_scale, 0);
        } 
    }
    
    //return float4(input.uv, 0.0, 1.0); // visualize UVs as color
    return color;
}

#endif

