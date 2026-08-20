#version 460

#extension GL_GOOGLE_include_directive : enable

#include "platform.glslh"
#include "frame.h"
#include "../shared_structs.h"

#if defined(COMPUTE_TEMPORAL_AA)

layout ( std140, set = MATERIAL_SET, binding = 50 ) uniform TaaConstants {
    GpuTaaConstants taa;
};

// TAA modes
#define TAAModeSimplest                     0
#define TAAModeRaptor                       1

// Velocity sampling modes
#define VelocitySamplingModeSingle          0
#define VelocitySamplingMode3x3             1
#define VelocitySamplingDominantVelocity3x3 2

// History sampling filter
#define HistorySamplingFilterSingle         0
#define HistorySamplingFilterCatmullRom     1

// History clipping mode
#define HistoryConstraintModeNone           0
#define HistoryConstraintModeClamp          1
#define HistoryConstraintModeClip           2
#define HistoryConstraintModeVarianceClip   3
#define HistoryConstraintModeVarianceClipClamp 4

// Current color filter
#define CurrentColorFilterNone              0
#define CurrentColorFilterMitchell          1
#define CurrentColorFilterBlackman          2
#define CurrentColorFilterCatmullRom        3

// Options
bool use_inverse_luminance_filtering() {
    return (taa.options & 1) == 1;
}

bool use_temporal_filtering() {
    return (taa.options & 2) == 2;
}

bool use_luminance_difference_filtering() {
    return (taa.options & 4) == 4;
}

bool use_ycocg() {
    return (taa.options & 8) == 8;
}

// Optimized clip aabb function from Inside game.
vec4 clip_aabb(vec3 aabb_min, vec3 aabb_max, vec4 previous_sample, float average_alpha) {
    // note: only clips towards aabb center (but fast!)
    vec3 p_clip = 0.5 * (aabb_max + aabb_min);
    vec3 e_clip = 0.5 * (aabb_max - aabb_min) + 0.000000001f;

    vec4 v_clip = previous_sample - vec4(p_clip, average_alpha);
    vec3 v_unit = v_clip.xyz / e_clip;
    vec3 a_unit = abs(v_unit);
    float ma_unit = max(a_unit.x, max(a_unit.y, a_unit.z));

    if (ma_unit > 1.0) {
        return vec4(p_clip, average_alpha) + v_clip / ma_unit;
    }
    else {
        // point inside aabb
        return previous_sample;
    }
}

// Utility methods to sample textures.
vec4 sample_color(vec2 uv) {
    vec4 color = textureLod(global_textures[nonuniformEXT(taa.current_color_texture_index)], uv, 0);

    if ( use_ycocg() ) {
        return vec4(rgb_to_ycocg(color.rgb), color.a);
    }

    return color;
}

vec4 sample_history_color(vec2 uv) {
    vec4 color = textureLod(global_textures[nonuniformEXT(taa.history_color_texture_index)], uv, 0);

    if ( use_ycocg() ) {
        return vec4(rgb_to_ycocg(color.rgb), color.a);
    }

    return color;
}

vec4 sample_current_color_point(ivec2 pos) {
    vec4 color = texelFetch(global_textures[nonuniformEXT(taa.current_color_texture_index)], pos, 0);

    if ( use_ycocg() ) {
        return vec4(rgb_to_ycocg(color.rgb), color.a);
    }

    return color;
}

vec4 sample_history_color_point(ivec2 pos) {
    vec4 color = texelFetch(global_textures[nonuniformEXT(taa.history_color_texture_index)], pos, 0);

    if ( use_ycocg() ) {
        return vec4(rgb_to_ycocg(color.rgb), color.a);
    }

    return color;
}

vec2 sample_motion_vector_point( ivec2 pos ) {
    return texelFetch(global_textures[nonuniformEXT(taa.velocity_texture_index)], pos, 0).rg;
}

float sample_current_depth_point( ivec2 pos ) {
    return texelFetch(global_textures[ nonuniformEXT( taa.current_depth_texture_index ) ], pos, 0 ).r;
}

float sample_previous_depth( vec2 uv ) {
    return textureLod(global_textures[ nonuniformEXT( taa.previous_depth_texture_index ) ], uv, 0 ).r;
}

// Find closest fragment position in a 3x3 neighborhood reading depth as metrics.
void find_closest_fragment_3x3(ivec2 pixel, out ivec2 closest_position, out float closest_depth) {

    closest_depth = 1.0f;
    closest_position = ivec2(0,0);

    for (int x = -1; x <= 1; ++x ) {
        for (int y = -1; y <= 1; ++y ) {

            ivec2 pixel_position = pixel + ivec2(x, y);
            pixel_position = clamp(pixel_position, ivec2(0), ivec2(taa.render_resolution.x - 1, taa.render_resolution.y - 1));

            float current_depth = sample_current_depth_point( pixel_position );
            if ( depth_is_closer( current_depth, closest_depth ) ) {
                closest_depth = current_depth;
                closest_position = pixel_position;
            }
        }
    }
}

float depth_priority( float depth ) {
#if defined(RAPTOR_REVERSED_Z)
    return depth;
#else
    return 1.0f - depth;
#endif
}

// Find best sample based on depth and velocity scoring
void find_dominant_velocity_3x3(ivec2 pixel, out vec2 best_velocity, out float best_depth ) {
    best_velocity = vec2( 0.0f );
    best_depth = sample_current_depth_point( pixel );

    float best_score = -1e20f;

    for ( int y = -1; y <= 1; ++y ) {
        for ( int x = -1; x <= 1; ++x ) {

            ivec2 p = clamp(pixel + ivec2( x, y ), ivec2( 0 ),
                            ivec2( taa.render_resolution.x - 1, taa.render_resolution.y - 1 ) );

            float depth = sample_current_depth_point( p );
            vec2 velocity = sample_motion_vector_point( p );
            float velocity_len = length( velocity );

            // Assumes standard depth testing
            float depth_score = depth_priority( depth ) * 10.0f;
            float velocity_score = velocity_len * 100.0f;
            float center_score = all( equal( p, pixel ) ) ? 0.01f : 0.0f;

            float score = depth_score + velocity_score + center_score;

            if ( score > best_score ) {
                best_score = score;
                best_velocity = velocity;
                best_depth = depth;
            }
        }
    }
}

// Compute depth based disocclusion factor
float compute_disocclusion_factor( float current_depth, vec2 reprojected_uv ) {

    if ( any( lessThan( reprojected_uv, vec2( 0.0f ) ) ) ||
         any( greaterThan( reprojected_uv, vec2( 1.0f ) ) ) ) {
        return 0.0f;
    }

    float previous_depth = sample_previous_depth( reprojected_uv );
    float depth_delta = abs( current_depth - previous_depth );

    // Conservative first-pass thresholds.
    return 1.0f - smoothstep( 0.0015f, 0.02f, depth_delta );
}

// https://github.com/TheRealMJP/MSAAFilter/blob/master/MSAAFilter/Resolve.hlsl
float filter_cubic(in float x, in float B, in float C) {
    float y = 0.0f;
    float x2 = x * x;
    float x3 = x * x * x;
    if(x < 1)
        y = (12 - 9 * B - 6 * C) * x3 + (-18 + 12 * B + 6 * C) * x2 + (6 - 2 * B);
    else if (x <= 2)
        y = (-B - 6 * C) * x3 + (6 * B + 30 * C) * x2 + (-12 * B - 48 * C) * x + (8 * B + 24 * C);

    return y / 6.0f;
}

float filter_mitchell(float value) {
    float cubic_value = value;
    return filter_cubic( cubic_value, 1 / 3.0f, 1 / 3.0f );
}

float filter_blackman_harris(float value) {
    float x = 1.0f - value;

    const float a0 = 0.35875f;
    const float a1 = 0.48829f;
    const float a2 = 0.14128f;
    const float a3 = 0.01168f;
    return saturate(a0 - a1 * cos(PI * x) + a2 * cos(2 * PI * x) - a3 * cos(3 * PI * x));
}

float filter_catmull_rom(float value) {
    return filter_cubic(value, 0, 0.5f);
}

// Choose between different filters.
float subsample_filter(float value) {
    // Cubic filters works on [-2, 2] domain, thus scale the value by 2
    // for Mitchell, Blackmann and Catmull-Rom.
    switch (taa.current_color_filter) {
        case CurrentColorFilterNone:
            return 1.0f;
        case CurrentColorFilterMitchell:
            return filter_mitchell( value );
        case CurrentColorFilterBlackman:
            return filter_blackman_harris( value );
        case CurrentColorFilterCatmullRom:
            return filter_catmull_rom( value );
    }

    return 1.0f;
}

// Samples a texture with Catmull-Rom filtering, using 9 texture fetches instead of 16.
// See http://vec3.ca/bicubic-filtering-in-fewer-taps/ for more details
vec3 sample_texture_catmull_rom(vec2 uv, uint texture_index, vec2 texture_resolution) {
    // We're going to sample a a 4x4 grid of texels surrounding the target UV coordinate. We'll do this by rounding
    // down the sample location to get the exact center of our "starting" texel. The starting texel will be at
    // location [1, 1] in the grid, where [0, 0] is the top left corner.
    vec2 sample_position = uv * texture_resolution;
    vec2 tex_pos_1 = floor(sample_position - 0.5f) + 0.5f;

    // Compute the fractional offset from our starting texel to our original sample location, which we'll
    // feed into the Catmull-Rom spline function to get our filter weights.
    vec2 f = sample_position - tex_pos_1;

    // Compute the Catmull-Rom weights using the fractional offset that we calculated earlier.
    // These equations are pre-expanded based on our knowledge of where the texels will be located,
    // which lets us avoid having to evaluate a piece-wise function.
    vec2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
    vec2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
    vec2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
    vec2 w3 = f * f * (-0.5f + 0.5f * f);

    // Work out weighting factors and sampling offsets that will let us use bilinear filtering to
    // simultaneously evaluate the middle 2 samples from the 4x4 grid.
    vec2 w12 = w1 + w2;
    vec2 offset_12 = w2 / (w1 + w2);

    // Compute the final UV coordinates we'll use for sampling the texture
    vec2 tex_pos_0 = tex_pos_1 - 1;
    vec2 tex_pos_3 = tex_pos_1 + 2;
    vec2 tex_pos_12 = tex_pos_1 + offset_12;

    tex_pos_0 /= texture_resolution;
    tex_pos_3 /= texture_resolution;
    tex_pos_12 /= texture_resolution;

    vec3 result = vec3(0);
    result += textureLod(global_textures[nonuniformEXT(texture_index)], vec2(tex_pos_0.x, tex_pos_0.y), 0).rgb * w0.x * w0.y;
    result += textureLod(global_textures[nonuniformEXT(texture_index)], vec2(tex_pos_12.x, tex_pos_0.y), 0).rgb * w12.x * w0.y;
    result += textureLod(global_textures[nonuniformEXT(texture_index)], vec2(tex_pos_3.x, tex_pos_0.y), 0).rgb * w3.x * w0.y;

    result += textureLod(global_textures[nonuniformEXT(texture_index)], vec2(tex_pos_0.x, tex_pos_12.y), 0).rgb * w0.x * w12.y;
    result += textureLod(global_textures[nonuniformEXT(texture_index)], vec2(tex_pos_12.x, tex_pos_12.y), 0).rgb * w12.x * w12.y;
    result += textureLod(global_textures[nonuniformEXT(texture_index)], vec2(tex_pos_3.x, tex_pos_12.y), 0).rgb * w3.x * w12.y;

    result += textureLod(global_textures[nonuniformEXT(texture_index)], vec2(tex_pos_0.x, tex_pos_3.y), 0).rgb * w0.x * w3.y;
    result += textureLod(global_textures[nonuniformEXT(texture_index)], vec2(tex_pos_12.x, tex_pos_3.y), 0).rgb * w12.x * w3.y;
    result += textureLod(global_textures[nonuniformEXT(texture_index)], vec2(tex_pos_3.x, tex_pos_3.y), 0).rgb * w3.x * w3.y;

    if ( use_ycocg() ) {
        result = rgb_to_ycocg( result.rgb );
    }

    return result;
}

// Simples implementation of TAA, left for learning purposes.
vec3 taa_simplest( ivec2 pos ) {
    const vec2 velocity_ndc = sample_motion_vector_point( pos );
    const vec2 screen_uv = uv_nearest(pos, frame.resolution);
    const vec2 reprojected_uv = reproject_uv_from_ndc_motion( screen_uv, velocity_ndc );

    vec3 history_color = sample_history_color(reprojected_uv).rgb;
    vec3 current_color = sample_color(screen_uv.xy).rgb;

    return mix(current_color, history_color, 0.9f);
}

vec3 taa_raptor( ivec2 swapchain_pos ) {

    // Calculate render pos from swapchain pos
    vec2 swapchain_uv = (vec2(swapchain_pos) + 0.5f ) / taa.swapchain_resolution;
    ivec2 render_pos = ivec2(swapchain_uv * taa.render_resolution);

    // Velocity sampling: find coordinates to sample motion vectors.
    float closest_depth = 1.0f;
    ivec2 closest_position = ivec2(0,0);
    float current_depth = sample_current_depth_point( render_pos );
    vec2 velocity_ndc = sample_motion_vector_point( closest_position );

    switch (taa.velocity_sampling_mode) {

        case VelocitySamplingDominantVelocity3x3: {
            float dominant_depth = current_depth;
            find_dominant_velocity_3x3( render_pos, velocity_ndc, dominant_depth );
            break;
        }

        case VelocitySamplingMode3x3: {
            find_closest_fragment_3x3( render_pos.xy, closest_position, closest_depth );
            velocity_ndc = sample_motion_vector_point( closest_position );
            break;
        }

        case VelocitySamplingModeSingle:
        default: {
            velocity_ndc = sample_motion_vector_point( render_pos );
            break;
        }
    }

    const vec2 reprojected_uv = reproject_uv_from_ndc_motion( swapchain_uv, velocity_ndc );

    // Disocclusion handling based on reprojected previous depth difference with current depth.
    float disocclusion_factor = compute_disocclusion_factor( current_depth, reprojected_uv );
    if ( disocclusion_factor <= 0.0f ) {
        return sample_current_color_point( render_pos ).rgb;
    }

    // History sampling: read history samples and optionally apply a filter to it.
    vec3 history_color = vec3(0);
    switch (taa.history_sampling_filter) {
        case HistorySamplingFilterSingle:
            history_color = sample_history_color( reprojected_uv ).rgb;
            break;

        case HistorySamplingFilterCatmullRom:
            history_color = sample_texture_catmull_rom( reprojected_uv, taa.history_color_texture_index, taa.swapchain_resolution );
            break;

        default:
            history_color = sample_history_color( reprojected_uv ).rgb;
            break;
    }

    // Current sampling: read a 3x3 neighborhood and cache color and other data to process history and final resolve.
    // Accumulate current sample and weights.
    vec3 current_sample_total = vec3(0);
    float current_sample_weight = 0.0f;
    // Min and Max used for history clipping
    vec3 neighborhood_min = vec3(10000);
    vec3 neighborhood_max = vec3(-10000);
    // Cache of moments used in the resolve phase
    vec3 m1 = vec3(0);
    vec3 m2 = vec3(0);

    for (int x = -1; x <= 1; ++x ) {
        for (int y = -1; y <= 1; ++y ) {

            ivec2 sample_position = render_pos + ivec2(x, y);
            sample_position = clamp(sample_position, ivec2(0), ivec2(taa.render_resolution.x - 1, taa.render_resolution.y - 1));

            vec2 sample_uv = (vec2(sample_position) + 0.5f) / taa.render_resolution;
            //vec3 current_sample = sample_current_color_point(sample_position).rgb;
            vec3 current_sample = sample_color(sample_uv).rgb;

            vec2 subsample_position = vec2(x * 1.f, y * 1.f) / (sqrt(2.0f) * taa.scale_factor);
            float subsample_distance = length( subsample_position );

            float subsample_weight = subsample_filter( subsample_distance );

            current_sample_total += current_sample * subsample_weight;
            current_sample_weight += subsample_weight;

            neighborhood_min = min( neighborhood_min, current_sample );
            neighborhood_max = max( neighborhood_max, current_sample );

            m1 += current_sample;
            m2 += current_sample * current_sample;
        }
    }

    // Compute motion and neighborhood factors
    float motion_len = length( velocity_ndc );

    vec3 neighborhood_extent = neighborhood_max - neighborhood_min;
    float neighborhood_max_extent = max( neighborhood_extent.r, max( neighborhood_extent.g, neighborhood_extent.b ) );

    float motion_sharpness = 1.0f - smoothstep( 0.002f, 0.05f, motion_len );
    float neighborhood_sharpness = 1.0f - smoothstep( 0.05f, 0.6f, neighborhood_max_extent );

    // Calculate current sample color
    vec3 current_center_sample = sample_color(swapchain_uv).rgb;
    vec3 current_filtered_sample = current_sample_total / max( current_sample_weight, 0.0001f );

    // Preserve more local sample
    float adaptive_sharpness = taa.current_sample_sharpness;
    adaptive_sharpness = mix( 0.4f * taa.current_sample_sharpness, taa.current_sample_sharpness, motion_sharpness * neighborhood_sharpness );

    vec3 current_sample = mix( current_filtered_sample, current_center_sample, adaptive_sharpness );

    // Guard for outside sampling
    if (any(lessThan(reprojected_uv, vec2(0.0f))) || any(greaterThan(reprojected_uv, vec2(1.0f)))) {
        return current_sample;
    }

    // shrink chroma min-max
    if ( use_ycocg() ) {
        vec2 chroma_extent = vec2( 0.125 * (neighborhood_max.r - neighborhood_min.r) );
        vec2 chroma_center = current_sample.gb;
        neighborhood_min.yz = chroma_center - chroma_extent;
        neighborhood_max.yz = chroma_center + chroma_extent;
    }

    // History confidence based on different factors
    // 1) Luma based history validity
    float current_luma = use_ycocg() ? current_sample.r : luminance( current_sample );
    float history_luma = use_ycocg() ? history_color.r : luminance( history_color );

    float luma_diff = abs( current_luma - history_luma ) /
                      max( max( current_luma, history_luma ), 0.05f );

    // Soften similarity with smoothstep
    float luma_similarity = 1.0f - smoothstep( 0.05f, 0.5f, luma_diff );
    
    // 2) Disocclusion validity
    float history_validity = disocclusion_factor * luma_similarity;

    // 3) Motion validity
    float motion_validity = 1.0f - smoothstep( 0.002f, 0.05f, motion_len );

    // 4) Neighborhood validity
    float neighborhood_validity = 1.0f - smoothstep( 0.05f, 0.6f, neighborhood_max_extent );

    history_validity *= motion_validity * neighborhood_validity;
    history_validity = clamp( history_validity, 0.0f, 1.0f );

    // History constraint
    switch (taa.history_constraint_mode) {
        case HistoryConstraintModeNone:
            break;

        case HistoryConstraintModeClamp:
            history_color.rgb = clamp(history_color.rgb, neighborhood_min, neighborhood_max);
            break;

        case HistoryConstraintModeClip:
            history_color.rgb = clip_aabb(neighborhood_min, neighborhood_max, vec4(history_color, 1.0f), 1.0f).rgb;
            break;

        case HistoryConstraintModeVarianceClip: {
            float rcp_sample_count = 1.0f / 9.0f;
            float gamma = mix( 0.75f, 2.0f, history_validity );
            vec3 mu = m1 * rcp_sample_count;
            vec3 sigma = sqrt(abs((m2 * rcp_sample_count) - (mu * mu)));
            vec3 minc = mu - gamma * sigma;
            vec3 maxc = mu + gamma * sigma;

            history_color.rgb = clip_aabb(minc, maxc, vec4(history_color, 1), 1.0f).rgb;

            break;
        }
        case HistoryConstraintModeVarianceClipClamp:
        default: {
            float rcp_sample_count = 1.0f / 9.0f;
            float gamma = mix( 0.75f, 2.0f, history_validity );
            vec3 mu = m1 * rcp_sample_count;
            vec3 sigma = sqrt(abs((m2 * rcp_sample_count) - (mu * mu)));
            vec3 minc = mu - gamma * sigma;
            vec3 maxc = mu + gamma * sigma;

            vec3 clamped_history_color = clamp( history_color.rgb, neighborhood_min, neighborhood_max );
            history_color.rgb = clip_aabb(minc, maxc, vec4(clamped_history_color, 1), 1.0f).rgb;

            break;
        }
    }

    // Resolve: combine history and current colors for final pixel color.
    vec3 current_weight = vec3(0.1f);
    vec3 history_weight = vec3(1.0 - current_weight);

    // Temporal filtering
    if (use_temporal_filtering() ) {
        vec3 temporal_extent = abs( neighborhood_max - neighborhood_min );
        vec3 temporal_weight = clamp( temporal_extent / max( current_sample, vec3( 0.001f ) ), vec3( 0.0f ), vec3( 1.0f ) );

        history_weight = clamp( mix( vec3( 0.25f ), vec3( 0.85f ), temporal_weight ), vec3( 0.0f ), vec3( 1.0f ) );
        current_weight = vec3( 1.0f ) - history_weight;
    }

    // Inverse luminance filtering
    if (use_inverse_luminance_filtering() || use_luminance_difference_filtering() ) {
        // Calculate compressed colors and luminances
        vec3 compressed_source = current_sample / (max(max(current_sample.r, current_sample.g), current_sample.b) + 1.0f);
        vec3 compressed_history = history_color / (max(max(history_color.r, history_color.g), history_color.b) + 1.0f);
        
        float luminance_source = use_ycocg() ? compressed_source.r : luminance(compressed_source);
        float luminance_history = use_ycocg() ? compressed_history.r : luminance(compressed_history);

        if ( use_luminance_difference_filtering() ) {
            float unbiased_diff = abs(luminance_source - luminance_history) / max(luminance_source, max(luminance_history, 0.2));
            
            float unbiased_weight = 1.0 - unbiased_diff;
            float unbiased_weight_sqr = unbiased_weight * unbiased_weight;
            
            float k_feedback = mix(0.05f, 0.95f, unbiased_weight_sqr);

            // If current and history are similar, trust history more.
            history_weight = vec3( k_feedback );
            current_weight = vec3( 1.0f - k_feedback );
        }

        if ( use_inverse_luminance_filtering() ) {
            float luminance_for_history = max( luminance_source, luminance_history );
            history_weight *= 1.0f / ( 1.0f + luminance_for_history );
        }
    }

    // Apply reprojection confidence.
    history_weight *= history_validity;

    vec3 weight_sum = max( current_weight + history_weight, vec3( 0.00001f ) );
    vec3 result = ( current_sample * current_weight + history_color * history_weight ) / weight_sum;
    
    return result;
}


layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
void main() {
    ivec3 swapchain_pos = ivec3(gl_GlobalInvocationID.xyz);

    if (any(greaterThanEqual(swapchain_pos.xy, ivec2(taa.swapchain_resolution)))) {
        return;
    }

    vec3 final_color = vec3(0);

    if ( taa.taa_modes == 0 ) {
        vec2  swapchain_uv = (vec2(swapchain_pos) + 0.5f) / taa.swapchain_resolution;
        ivec2 render_pos = ivec2(swapchain_uv * taa.render_resolution);
        final_color = taa_simplest( render_pos );
    }
    else if ( taa.taa_modes == 1 ) {
        final_color = taa_raptor( swapchain_pos.xy );
    }

    if ( use_ycocg() ) {
        final_color = ycocg_to_rgb( final_color );
    }

    imageStore( global_images_2d[taa.taa_output_texture_index], swapchain_pos.xy, vec4(final_color, 1) );
}

#endif // COMPUTE_TEMPORAL_AA
