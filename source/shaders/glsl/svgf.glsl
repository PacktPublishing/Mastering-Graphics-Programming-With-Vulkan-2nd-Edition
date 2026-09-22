#version 460

#extension GL_GOOGLE_include_directive : enable

#include "platform.glslh"
#include "frame.h"
#include "mesh.h"
#include "sampling.h"
#include "debug_rendering.h"
#include "../shared_structs.h"

#if defined( COMPUTE_SVGF_ACCUMULATION ) || defined( COMPUTE_SVGF_VARIANCE ) || defined( COMPUTE_SVGF_WAVELET ) || defined( COMPUTE_SVGF_DOWNSAMPLE )

#if defined( COMPUTE_SVGF_DOWNSAMPLE )
layout( set = MATERIAL_SET, binding = 40 ) uniform SVGFAccumulationConstants {
    SVGFConstants svgf;
};
#else
layout( set = MATERIAL_SET, binding = 40 ) uniform SVGFAccumulationConstants {
    SVGFConstants svgf;
    SVGFOutputs svgf_reflections;
    SVGFOutputs svgf_restirgi;
};
#endif

layout( push_constant ) uniform SVGFPushConstantsBlock {
    SVGFPushConstants svgf_push;
};

// Resource allocation and both input signals must use a resolution scale of 0.5.
ivec2 svgf_resolution( SVGFConstants constants ) {
    return ivec2( ceil( vec2( frame.resolution ) * constants.output_resolution_scale ) );
}

bool outside_svgf_resolution( ivec2 p, ivec2 resolution ) {
    return any( lessThan( p, ivec2( 0 ) ) ) || any( greaterThanEqual( p, resolution ) );
}

bool outside_svgf_resolution( SVGFConstants constants, ivec2 p ) {
    return outside_svgf_resolution( p, svgf_resolution( constants ) );
}

// Half-resolution guides select the nearest sample in a 2x2 full-resolution block.
// Depth uses the conventional range: near = 0, far = 1.
ivec2 choose_svgf_representative_fullres_pixel( ivec2 denoiser_xy, out float representative_depth ) {
    ivec2 base_fullres = denoiser_xy * 2;

    ivec2 best_fullres_xy = clamp( base_fullres, ivec2( 0 ), ivec2( frame.resolution ) - ivec2( 1 ) );
    float best_depth = 1.0;

    for ( int y = 0; y < 2; ++y ) {
        for ( int x = 0; x < 2; ++x ) {
            ivec2 fullres_xy = clamp( base_fullres + ivec2( x, y ), ivec2( 0 ), ivec2( frame.resolution ) - ivec2( 1 ) );
            float depth = texelFetch( global_textures[ ( frame.depth_texture_index ) ], fullres_xy, 0 ).r;

            if ( depth_is_closer( depth, best_depth ) ) {
                best_depth = depth;
                best_fullres_xy = fullres_xy;
            }
        }
    }

    representative_depth = best_depth;

    return best_fullres_xy;
}

// Return the normal weight and depth exponent separately for the combined luminance/depth weight.
void compute_normal_depth_weight( SVGFConstants constants, vec3 n_p, vec2 linear_z_dd, ivec2 q, float phi_depth,
    out float w_n, out float w_z ) {
    vec2 encoded_normal_q = texelFetch( global_textures[ ( constants.current_normals_texture_index ) ], q, 0 ).rg;
    vec3 n_q = octahedral_decode( encoded_normal_q );

    w_n = pow( clamp( dot( n_p, n_q ), 0.0, 1.0 ), svgf_push.sigma_n );

    float z_q = texelFetch( global_textures[ ( constants.current_linear_z_dd_texture_index ) ], q, 0 ).r;

    w_z = 0.0;
    if ( phi_depth > 0.0 ) {
        w_z = max( abs( linear_z_dd.x - z_q ) / phi_depth, 0.0 );
    }
}

#endif

#if defined( COMPUTE_SVGF_ACCUMULATION )

// Accept history taps whose mesh ID, depth and normal match the current guide.
bool temporal_sample_is_consistent( ivec2 frag_coord, ivec2 prev_frag_coord_offset, uint mesh_id, vec2 depth_normal_fwidth, float z, vec3 normal ) {
    if ( outside_svgf_resolution( svgf, prev_frag_coord_offset ) ) {
        return false;
    }

    uint prev_mesh_id = texelFetch( global_utextures[ ( svgf.history_mesh_id_texture_index ) ], prev_frag_coord_offset, 0 ).r;
    if ( mesh_id != prev_mesh_id ) {
        return false;
    }

    float prev_z = texelFetch( global_textures[ ( svgf.history_linear_depth_texture ) ], prev_frag_coord_offset, 0 ).r;
    float depth_diff = abs( prev_z - z ) / ( depth_normal_fwidth.x + 1e-2 );
    if ( depth_diff > svgf.temporal_depth_difference ) {
        return false;
    }

    vec2 prev_encoded_normal = texelFetch( global_textures[ ( svgf.history_normals_texture_index ) ], prev_frag_coord_offset, 0 ).rg;
    vec3 prev_normal = octahedral_decode( prev_encoded_normal );

    float normal_diff = distance( normal, prev_normal ) / ( depth_normal_fwidth.y + 1e-2 );
    if ( normal_diff > svgf.temporal_normal_difference ) {
        return false;
    }

    return true;
}

void accumulate_history_sample( SVGFOutputs outputs, ivec2 prev_frag_coord_offset, float weight,
                                inout vec3 color_sum, inout vec2 moments_sum, inout float weight_sum,
                                inout uint count, inout float best_count_weight, inout uint best_history_count ) {

    if ( weight <= 0.0 ) {
        return;
    }

    vec4 history_output_color = texelFetch( global_textures[ ( outputs.history_output_texture_index ) ], prev_frag_coord_offset, 0 );

    if ( any( isnan( history_output_color ) ) ||
        any( isinf( history_output_color ) ) ) {
        return;
    }

    vec2 moment = texelFetch( global_textures[ ( outputs.history_moments_texture_index ) ], prev_frag_coord_offset, 0 ).rg;

    if ( any( isnan( moment ) ) || any( isinf( moment ) ) ) {
        return;
    }

    color_sum += history_output_color.rgb * weight;
    moments_sum += moment * weight;
    weight_sum += weight;
    count += 1;

    uint previous_count = uint( clamp( history_output_color.a, 1.0, 32.0 ) );
    if ( weight > best_count_weight ) {
        best_count_weight = weight;
        best_history_count = previous_count;
    }
}

void resolve_history( bool used_fallback, vec3 color_sum, vec2 moments_sum, float weight_sum,
                      uint count, uint best_history_count, out bool is_consistent,
                      out vec3 history_color, out vec2 history_moments, out uint history_count ) {
    is_consistent = ( count != 0 ) && ( weight_sum > 1e-2 );
    if ( is_consistent ) {
        history_color = color_sum / weight_sum;
        history_moments = moments_sum / weight_sum;
        history_count = used_fallback ? min( best_history_count, 4u ) : best_history_count;
    }
}

void check_temporal_consistency( ivec2 frag_coord, vec2 prev_frag_coord, out bool is_consistent_reflections, 
                                 out vec3 history_color_reflections, out vec2 history_moments_reflections, out uint history_count_reflections,
                                 out bool is_consistent_restirgi, out vec3 history_color_restirgi, out vec2 history_moments_restirgi,
                                 out uint history_count_restirgi ) {

    is_consistent_reflections = false;
    is_consistent_restirgi = false;
    history_color_reflections = vec3( 0 );
    history_color_restirgi = vec3( 0 );
    history_moments_reflections = vec2( 0 );
    history_moments_restirgi = vec2( 0 );
    history_count_reflections = 1;
    history_count_restirgi = 1;

    if ( any( isnan( prev_frag_coord ) ) ||
        any( isinf( prev_frag_coord ) ) ) {
        return;
    }

    ivec2 base = ivec2( floor( prev_frag_coord ) );

    uint mesh_id = texelFetch( global_utextures[ ( svgf.current_mesh_id_texture_index ) ], frag_coord, 0 ).r;
    vec2 depth_normal_fwidth = texelFetch( global_textures[ ( svgf.current_depth_normal_fwidth_texture_index ) ], frag_coord, 0 ).rg;
    float z = texelFetch( global_textures[ ( svgf.current_linear_z_dd_texture_index ) ], frag_coord, 0 ).r;
    vec2 encoded_normal = texelFetch( global_textures[ ( svgf.current_normals_texture_index ) ], frag_coord, 0 ).rg;
    vec3 normal = octahedral_decode( encoded_normal );

    vec3 color_sum_reflections = vec3( 0 );
    vec3 color_sum_restirgi = vec3( 0 );
    vec2 moments_sum_reflections = vec2( 0 );
    vec2 moments_sum_restirgi = vec2( 0 );
    float weight_sum_reflections = 0.0;
    float weight_sum_restirgi = 0.0;
    uint count_reflections = 0;
    uint count_restirgi = 0;
    float best_count_weight_reflections = 0.0;
    float best_count_weight_restirgi = 0.0;
    uint best_history_count_reflections = 1u;
    uint best_history_count_restirgi = 1u;

    vec2 f = fract( prev_frag_coord );

    for ( int y = 0; y <= 1; ++y ) {
        for ( int x = 0; x <= 1; ++x ) {
            ivec2 prev_frag_coord_offset = base + ivec2( x, y );
            if ( !temporal_sample_is_consistent( frag_coord, prev_frag_coord_offset, mesh_id, depth_normal_fwidth, z, normal ) ) {
                continue;
            }

            float wx = ( x == 0 ) ? 1.0 - f.x : f.x;
            float wy = ( y == 0 ) ? 1.0 - f.y : f.y;
            float w = wx * wy;

            accumulate_history_sample( svgf_reflections, prev_frag_coord_offset, w, color_sum_reflections, moments_sum_reflections, weight_sum_reflections, count_reflections, best_count_weight_reflections, best_history_count_reflections );
            accumulate_history_sample( svgf_restirgi, prev_frag_coord_offset, w, color_sum_restirgi, moments_sum_restirgi, weight_sum_restirgi, count_restirgi, best_count_weight_restirgi, best_history_count_restirgi );
        }
    }

    // Use a 3x3 fallback if the valid bilinear taps have insufficient total weight.
    bool fallback_reflections = count_reflections == 0 || weight_sum_reflections < 0.25;
    bool fallback_restirgi = count_restirgi == 0 || weight_sum_restirgi < 0.25;

    if ( fallback_reflections ) {
        color_sum_reflections = vec3( 0 );
        moments_sum_reflections = vec2( 0 );
        weight_sum_reflections = 0.0;
        count_reflections = 0;
        best_count_weight_reflections = 0.0;
        best_history_count_reflections = 1u;
    }

    if ( fallback_restirgi ) {
        color_sum_restirgi = vec3( 0 );
        moments_sum_restirgi = vec2( 0 );
        weight_sum_restirgi = 0.0;
        count_restirgi = 0;
        best_count_weight_restirgi = 0.0;
        best_history_count_restirgi = 1u;
    }

    if ( fallback_reflections || fallback_restirgi ) {
        for ( int y = -1; y <= 1; ++y ) {
            for ( int x = -1; x <= 1; ++x ) {
                ivec2 prev_frag_coord_offset = base + ivec2( x, y );
                if ( !temporal_sample_is_consistent( frag_coord, prev_frag_coord_offset, mesh_id, depth_normal_fwidth, z, normal ) ) {
                    continue;
                }

                if ( fallback_reflections ) {
                    accumulate_history_sample( svgf_reflections, prev_frag_coord_offset, 1.0, color_sum_reflections, moments_sum_reflections, weight_sum_reflections, count_reflections, best_count_weight_reflections, best_history_count_reflections );
                }
                if ( fallback_restirgi ) {
                    accumulate_history_sample( svgf_restirgi, prev_frag_coord_offset, 1.0, color_sum_restirgi, moments_sum_restirgi, weight_sum_restirgi, count_restirgi, best_count_weight_restirgi, best_history_count_restirgi );
                }
            }
        }
    }

    resolve_history( fallback_reflections, color_sum_reflections, moments_sum_reflections, weight_sum_reflections, count_reflections, best_history_count_reflections,
                     is_consistent_reflections, history_color_reflections, history_moments_reflections, history_count_reflections );

    resolve_history( fallback_restirgi, color_sum_restirgi, moments_sum_restirgi, weight_sum_restirgi, count_restirgi, best_history_count_restirgi,
                     is_consistent_restirgi, history_color_restirgi, history_moments_restirgi, history_count_restirgi );

}

// Accumulate radiance and luminance moments; radiance alpha stores the history count.
// During accumulation, step_size is the CPU-provided history-reset flag.
void accumulate_signal( SVGFOutputs outputs, ivec2 frag_coord, vec3 output_color, bool is_consistent,
                        vec3 history_output_color, vec2 history_moments, uint moment_history_count ) {

    if ( any( isnan( output_color.rgb ) ) || any( isinf( output_color.rgb ) ) ) {
        output_color = vec3( 0 );
    }

    float u_1 = luminance( output_color );
    vec2 moments = vec2( u_1, u_1 * u_1 );

    vec3 integrated_color_out = vec3( 0 );
    vec2 integrated_moments_out = vec2( 0 );

    if ( is_consistent && svgf_push.step_size != 1u ) {
        moment_history_count = min( moment_history_count + 1u, 32u );

        float alpha = max( 1.0 / float( moment_history_count ), 0.05 );

        integrated_color_out = output_color * alpha + ( 1 - alpha ) * history_output_color;

        integrated_moments_out = moments * alpha + ( 1 - alpha ) * history_moments;
    } else {
        integrated_color_out = output_color;
        integrated_moments_out = moments;
        moment_history_count = 1;
    }

    imageStore( global_images_2d[ outputs.integrated_color_texture_index ], frag_coord, vec4( integrated_color_out, float( moment_history_count ) ) );
    imageStore( global_images_2d[ outputs.integrated_moments_texture_index ], frag_coord, vec4( integrated_moments_out, 0, 0 ) );
}

layout( local_size_x = 8, local_size_y = 8, local_size_z = 1 ) in;
void main() {
    ivec2 frag_coord = ivec2( gl_GlobalInvocationID.xy );

    if ( outside_svgf_resolution( svgf, frag_coord ) ) {
        return;
    }

    vec2 motion_vector = texelFetch( global_textures[ ( svgf.current_motion_vectors_texture_index ) ], frag_coord, 0 ).rg;

    vec2 history_resolution = vec2( svgf_resolution( svgf ) );
    vec2 current_uv = ( vec2( frag_coord ) + 0.5 ) / history_resolution;
    vec2 prev_uv = reproject_uv_from_ndc_motion( current_uv, motion_vector );
    vec2 prev_frag_coord = prev_uv * history_resolution - 0.5;

    bool is_consistent_reflections = false;
    bool is_consistent_restirgi = false;
    vec3 history_color_reflections = vec3( 0 );
    vec3 history_color_restirgi = vec3( 0 );
    vec2 history_moments_reflections = vec2( 0 );
    vec2 history_moments_restirgi = vec2( 0 );
    uint history_count_reflections = 1;
    uint history_count_restirgi = 1;

    check_temporal_consistency( frag_coord, prev_frag_coord, is_consistent_reflections, history_color_reflections, 
                                history_moments_reflections, history_count_reflections, is_consistent_restirgi,
                                history_color_restirgi, history_moments_restirgi, history_count_restirgi );

    // Raw signals and SVGF guides use the same resolution (see svgf_set_common_constants).
    vec3 output_color_reflections = texelFetch( global_textures[ svgf_reflections.output_texture_index ], frag_coord, 0 ).rgb;
    vec3 output_color_restirgi = texelFetch( global_textures[ svgf_restirgi.output_texture_index ], frag_coord, 0 ).rgb;

    accumulate_signal( svgf_reflections, frag_coord, output_color_reflections, is_consistent_reflections, history_color_reflections, history_moments_reflections, history_count_reflections );
    accumulate_signal( svgf_restirgi, frag_coord, output_color_restirgi, is_consistent_restirgi, history_color_restirgi, history_moments_restirgi, history_count_restirgi );
}

#endif // COMPUTE_SVGF_ACCUMULATION

#if defined( COMPUTE_SVGF_VARIANCE )

// Estimate variance from spatially filtered moments for short histories, otherwise temporal moments.
void write_signal_variance( SVGFOutputs outputs, ivec2 frag_coord, bool needs_filtering, vec2 moments_sum, float sum_weights ) {
    float variance = 0.0;

    if ( needs_filtering ) {
        if ( sum_weights > 0.0 ) {
            vec2 moments = moments_sum / sum_weights;
            variance = max( moments.y - moments.x * moments.x, 0.0 );
        }
    } else {
        vec2 moments = texelFetch( global_textures[ ( outputs.integrated_moments_texture_index ) ], frag_coord, 0 ).rg;
        variance = max( moments.y - moments.x * moments.x, 0.0 );
    }

    imageStore( global_images_2d[ ( outputs.variance_texture_index ) ], frag_coord, vec4( variance, 0, 0, 0 ) );
}

layout( local_size_x = 8, local_size_y = 8, local_size_z = 1 ) in;
void main() {
    ivec2 frag_coord = ivec2( gl_GlobalInvocationID.xy );

    ivec2 resolution = svgf_resolution( svgf );
    if ( outside_svgf_resolution( frag_coord, resolution ) ) return;

    uint reflection_history_count = uint( texelFetch( global_textures[ ( svgf_reflections.integrated_color_texture_index ) ], frag_coord, 0 ).a );
    uint restirgi_history_count = uint( texelFetch( global_textures[ ( svgf_restirgi.integrated_color_texture_index ) ], frag_coord, 0 ).a );

    bool filter_reflections = reflection_history_count < 4;
    bool filter_restirgi = restirgi_history_count < 4;

    vec2 moments_sum_reflections = vec2( 0 );
    vec2 moments_sum_restirgi = vec2( 0 );
    float sum_weights_reflections = 0.0;
    float sum_weights_restirgi = 0.0;

    if ( filter_reflections || filter_restirgi ) {
        vec2 encoded_normal_p = texelFetch( global_textures[ ( svgf.current_normals_texture_index ) ], frag_coord, 0 ).rg;
        vec3 normal_p = octahedral_decode( encoded_normal_p );

        vec2 linear_z_dd = texelFetch( global_textures[ ( svgf.current_linear_z_dd_texture_index ) ], frag_coord, 0 ).rg;
        uint mesh_id_p = texelFetch( global_utextures[ ( svgf.current_mesh_id_texture_index ) ], frag_coord, 0 ).r;

        int filter_size = 3;
        float phi_depth = svgf_push.sigma_z * max( linear_z_dd.y, 1e-8 ) * filter_size;

        for ( int y = -filter_size; y <= filter_size; ++y ) {
            for ( int x = -filter_size; x <= filter_size; ++x ) {
                ivec2 q = frag_coord + ivec2( x, y );

                if ( outside_svgf_resolution( q, resolution ) ) {
                    continue;
                }

                uint mesh_id_q = texelFetch( global_utextures[ ( svgf.current_mesh_id_texture_index ) ], q, 0 ).r;
                if ( mesh_id_p != mesh_id_q ) {
                    continue;
                }

                float w_n, w_z;
                compute_normal_depth_weight( svgf, normal_p, linear_z_dd, q, phi_depth, w_n, w_z );
                float w_pq = exp( -w_z ) * w_n;

                if ( filter_reflections ) {
                    moments_sum_reflections += w_pq * texelFetch( global_textures[ ( svgf_reflections.integrated_moments_texture_index ) ], q, 0 ).rg;
                    sum_weights_reflections += w_pq;
                }

                if ( filter_restirgi ) {
                    moments_sum_restirgi += w_pq * texelFetch( global_textures[ ( svgf_restirgi.integrated_moments_texture_index ) ], q, 0 ).rg;
                    sum_weights_restirgi += w_pq;
                }
            }
        }
    }

    write_signal_variance( svgf_reflections, frag_coord, filter_reflections, moments_sum_reflections, sum_weights_reflections );
    write_signal_variance( svgf_restirgi, frag_coord, filter_restirgi, moments_sum_restirgi, sum_weights_restirgi );
}

#endif // COMPUTE_SVGF_VARIANCE

#if defined( COMPUTE_SVGF_WAVELET )

// Relative 5-tap binomial weights, with a center weight of one.
const float kernel_weights[ 3 ] = {
    1.0,
    2.0 / 3.0,
    1.0 / 6.0
};

// Five wavelet iterations use steps 1, 2, 4, 8 and 16. Keep in sync with k_num_passes.
const uint k_svgf_last_wavelet_step = 16u;

bool svgf_propagate_variance() {
    return svgf_push.step_size != k_svgf_last_wavelet_step;
}

void filter_signal_sample( SVGFOutputs outputs, ivec2 q, float h_q, float w_n, float w_z,
                           float rcp_sigma_l_variance, float luminance_p, inout vec3 filtered_color,
                           inout float color_weight, inout float updated_variance ) {
    
    vec3 c_q = texelFetch( global_textures[ ( outputs.integrated_color_texture_index ) ], q, 0 ).rgb;
    float l_q = luminance( c_q );

    float w_l = abs( luminance_p - l_q ) * rcp_sigma_l_variance;
    float w_pq = exp( -w_z - max( w_l, 0.0 ) ) * w_n;

    float sample_weight = h_q * w_pq;

    filtered_color += sample_weight * c_q;
    color_weight += sample_weight;

    if ( svgf_propagate_variance() ) {
        float prev_variance = texelFetch( global_textures[ outputs.variance_texture_index ], q, 0 ).r;

        updated_variance += pow( h_q, 2.0 ) * pow( w_pq, 2.0 ) * prev_variance;
    }
}

void store_filtered_signal( SVGFOutputs outputs, ivec2 frag_coord, vec3 filtered_color,
                            float color_weight, float updated_variance, uint moment_history_count ) {

    filtered_color /= color_weight;

    imageStore( global_images_2d[ ( outputs.filtered_color_texture_index ) ], frag_coord, vec4( filtered_color, float( moment_history_count ) ) );

    if ( svgf_propagate_variance() ) {
        updated_variance /= pow( color_weight, 2.0 );

        imageStore( global_images_2d[ ( outputs.updated_variance_texture_index ) ], frag_coord, vec4( updated_variance, 0, 0, 0 ) );
    }
}

// An 8x8 workgroup plus a one-texel halo: 10x10 pairs of variances (800 bytes).
shared vec2 svgf_variance_tile[100];

// Every invocation participates in the cooperative load and barrier, including out-of-bounds lanes.
void load_svgf_variance_tile() {
    ivec2 resolution = svgf_resolution( svgf );
    ivec2 origin = ivec2( gl_WorkGroupID.xy ) * 8 - ivec2( 1 );

    for ( uint i = gl_LocalInvocationIndex; i < 100u; i += 64u ) {
        ivec2 tile_xy = ivec2( int( i % 10u ), int( i / 10u ) );
        ivec2 q = origin + tile_xy;

        vec2 value = vec2( 0.0 );

        if ( all( greaterThanEqual( q, ivec2( 0 ) ) ) &&
            all( lessThan( q, resolution ) ) ) {
            value.x = texelFetch( global_textures[ ( svgf_reflections.variance_texture_index ) ], q, 0 ).r;

            value.y = texelFetch( global_textures[ ( svgf_restirgi.variance_texture_index ) ], q, 0 ).r;
        }

        svgf_variance_tile[i] = value;
    }

    barrier();
}

vec2 svgf_center_variance_shared() {
    ivec2 local_xy = ivec2( gl_LocalInvocationID.xy ) + ivec2( 1 );
    return svgf_variance_tile[local_xy.y * 10 + local_xy.x];
}

// Gaussian 3x3 variance blur, renormalized over valid image pixels.
vec2 blur_svgf_variance_shared( ivec2 p ) {
    ivec2 resolution = svgf_resolution( svgf );
    ivec2 local_xy = ivec2( gl_LocalInvocationID.xy ) + ivec2( 1 );

    vec2 result = vec2( 0.0 );
    float weight_sum = 0.0;

    for ( int y = -1; y <= 1; ++y ) {
        for ( int x = -1; x <= 1; ++x ) {
            ivec2 q = p + ivec2( x, y );

            if ( any( lessThan( q, ivec2( 0 ) ) ) ||
                any( greaterThanEqual( q, resolution ) ) ) {
                continue;
            }

            float wx = ( x == 0 ) ? 0.5 : 0.25;
            float wy = ( y == 0 ) ? 0.5 : 0.25;
            float weight = wx * wy;

            ivec2 s = local_xy + ivec2( x, y );

            result += svgf_variance_tile[s.y * 10 + s.x] * weight;
            weight_sum += weight;
        }
    }

    return result / weight_sum;
}

layout( local_size_x = 8, local_size_y = 8, local_size_z = 1 ) in;
void main() {
    ivec2 frag_coord = ivec2( gl_GlobalInvocationID.xy );

    load_svgf_variance_tile();

    if ( outside_svgf_resolution( svgf, frag_coord ) ) {
        return;
    }

    ivec2 resolution = svgf_resolution( svgf );

    vec2 center_variance = svgf_center_variance_shared();

    float new_variance_reflections = svgf_propagate_variance() ? center_variance.x : 0.0;
    float new_variance_restirgi = svgf_propagate_variance() ? center_variance.y : 0.0;

    vec2 encoded_normal_p = texelFetch( global_textures[ ( svgf.current_normals_texture_index ) ], frag_coord, 0 ).rg;
    vec3 normal_p = octahedral_decode( encoded_normal_p );

    vec2 linear_z_dd = texelFetch( global_textures[ ( svgf.current_linear_z_dd_texture_index ) ], frag_coord, 0 ).rg;
    uint mesh_id_p = texelFetch( global_utextures[ ( svgf.current_mesh_id_texture_index ) ], frag_coord, 0 ).r;

    vec4 center_p_reflections = texelFetch( global_textures[ ( svgf_reflections.integrated_color_texture_index ) ], frag_coord, 0 );
    vec4 center_p_restirgi = texelFetch( global_textures[ ( svgf_restirgi.integrated_color_texture_index ) ], frag_coord, 0 );

    vec3 color_p_reflections = center_p_reflections.rgb;
    vec3 color_p_restirgi = center_p_restirgi.rgb;
    float luminance_p_reflections = luminance( color_p_reflections );
    float luminance_p_restirgi = luminance( color_p_restirgi );

    // Use a smaller kernel at the two coarsest wavelet scales.
    int radius = ( svgf_push.step_size >= 8u ) ? 1 : 2;

    const float phi_depth = svgf_push.sigma_z * max( linear_z_dd.y, 1e-8 ) * svgf_push.step_size;

    vec3 new_filtered_color_reflections = color_p_reflections;
    vec3 new_filtered_color_restirgi = color_p_restirgi;
    float color_weight_reflections = 1.0;
    float color_weight_restirgi = 1.0;

    vec2 blurred_variance = blur_svgf_variance_shared( frag_coord );

    float variance_p_reflections = blurred_variance.x;
    float variance_p_restirgi = blurred_variance.y;

    // The luminance normalization is constant across the kernel for each signal.
    float rcp_sigma_l_reflections = 1.0 / ( svgf_push.sigma_l * sqrt( max( variance_p_reflections, 0.0 ) ) + 1e-5 );
    float rcp_sigma_l_restirgi = 1.0 / ( svgf_push.sigma_l * sqrt( max( variance_p_restirgi, 0.0 ) ) + 1e-5 );

    for ( int y = -radius; y <= radius; ++y ) {
        for ( int x = -radius; x <= radius; ++x ) {
            ivec2 q = frag_coord + ivec2( x, y ) * int( svgf_push.step_size );

            if ( outside_svgf_resolution( q, resolution ) ) {
                continue;
            }

            if ( x == 0 && y == 0 ) {
                continue;
            }

            uint mesh_id_q = texelFetch( global_utextures[ ( svgf.current_mesh_id_texture_index ) ], q, 0 ).r;
            if ( mesh_id_p != mesh_id_q ) {
                continue;
            }

            float h_q = kernel_weights[ abs( x ) ] * kernel_weights[ abs( y ) ];

            float w_n, w_z;
            compute_normal_depth_weight( svgf, normal_p, linear_z_dd, q, phi_depth, w_n, w_z );

            filter_signal_sample( svgf_reflections, q, h_q, w_n, w_z, rcp_sigma_l_reflections, luminance_p_reflections,
                new_filtered_color_reflections, color_weight_reflections, new_variance_reflections );
            filter_signal_sample( svgf_restirgi, q, h_q, w_n, w_z, rcp_sigma_l_restirgi, luminance_p_restirgi,
                new_filtered_color_restirgi, color_weight_restirgi, new_variance_restirgi );
        }
    }

    store_filtered_signal( svgf_reflections, frag_coord, new_filtered_color_reflections, color_weight_reflections, new_variance_reflections, uint( center_p_reflections.a ) );
    store_filtered_signal( svgf_restirgi, frag_coord, new_filtered_color_restirgi, color_weight_restirgi, new_variance_restirgi, uint( center_p_restirgi.a ) );
}

#endif // COMPUTE_SVGF_WAVELET

#if defined( COMPUTE_SVGF_DOWNSAMPLE )

layout( local_size_x = 8, local_size_y = 8, local_size_z = 1 ) in;
void main() {
    ivec2 frag_coord = ivec2( gl_GlobalInvocationID.xy );

    if ( outside_svgf_resolution( svgf, frag_coord ) ) {
        return;
    }

    float representative_depth;
    ivec2 fullres_xy = choose_svgf_representative_fullres_pixel( frag_coord, representative_depth );

    vec2 normals = texelFetch( global_textures[ ( svgf.normals_texture_index ) ], fullres_xy, 0 ).rg;

    ivec2 base_fullres = frag_coord * 2;
    ivec2 representative_offset = fullres_xy - base_fullres;

    uint representative_index = uint( representative_offset.y * 2 + representative_offset.x );

    // Guide channels: octahedral normal RG, raw depth B, representative sample index A.
    imageStore( global_images_2d[ ( svgf.current_normals_texture_index ) ], frag_coord, vec4( normals, representative_depth, float( representative_index ) ) );

    uvec4 mesh_id = texelFetch( global_utextures[ ( svgf.mesh_id_texture_index ) ], fullres_xy, 0 );
    imageStore( global_uimages_2d[ ( svgf.current_mesh_id_texture_index ) ], frag_coord, mesh_id );

    vec4 linear_z_dd = texelFetch( global_textures[ ( svgf.linear_z_dd_texture_index ) ], fullres_xy, 0 );
    imageStore( global_images_2d[ ( svgf.current_linear_z_dd_texture_index ) ], frag_coord, linear_z_dd );

    vec4 depth_normal_fwidth = texelFetch( global_textures[ ( svgf.depth_normal_fwidth_texture_index ) ], fullres_xy, 0 );
    imageStore( global_images_2d[ ( svgf.current_depth_normal_fwidth_texture_index ) ], frag_coord, depth_normal_fwidth );

    vec4 motion_vectors = texelFetch( global_textures[ ( svgf.motion_vectors_texture_index ) ], fullres_xy, 0 );
    imageStore( global_images_2d[ ( svgf.current_motion_vectors_texture_index ) ], frag_coord, motion_vectors );
}

#endif // COMPUTE_SVGF_DOWNSAMPLE
