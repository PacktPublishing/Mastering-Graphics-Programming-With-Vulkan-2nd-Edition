#version 460

#extension GL_GOOGLE_include_directive : enable

#include "platform.glslh"
#include "frame.h"
#include "mesh.h"
#include "sampling.h"
#include "debug_rendering.h"
#include "../shared_structs.h"

#if defined( COMPUTE_SVGF_ACCUMULATION ) || defined( COMPUTE_SVGF_VARIANCE ) || defined( COMPUTE_SVGF_WAVELET ) || defined(COMPUTE_SVGF_DOWNSAMPLE)

//#define DEBUG_ACCUMULATION 0


#if defined(COMPUTE_SVGF_DOWNSAMPLE)
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

ivec2 svgf_resolution( SVGFConstants constants ) {
    return ivec2( ceil( vec2( frame.resolution ) * constants.output_resolution_scale ) );
}

bool outside_svgf_resolution( SVGFConstants constants, ivec2 p ) {
    return any( lessThan( p, ivec2( 0 ) ) ) || any( greaterThanEqual( p, svgf_resolution( constants ) ) );
}

ivec2 halfres_to_fullres( SVGFConstants constants, ivec2 p ) {
    ivec2 xy = ivec2( ( vec2( p ) + 0.5 ) * constants.output_resolution_scale_rcp );
    return clamp( xy, ivec2( 0 ), ivec2( frame.resolution ) - ivec2( 1 ) );
}

ivec2 choose_svgf_representative_fullres_pixel( SVGFConstants constants, ivec2 denoiser_xy, out float representative_depth ) {
    int scale_rcp = max( 1, int( round( constants.output_resolution_scale_rcp ) ) );

    if ( scale_rcp == 1 ) {
        return clamp( denoiser_xy, ivec2( 0 ), ivec2( frame.resolution ) - ivec2( 1 ) );
    }

    ivec2 base_fullres = ivec2( vec2( denoiser_xy ) * constants.output_resolution_scale_rcp );

    ivec2 best_fullres_xy = clamp( base_fullres, ivec2( 0 ), ivec2( frame.resolution ) - ivec2( 1 ) );
    float best_depth = 1.0;

    for ( int y = 0; y < scale_rcp; ++y ) {
        for ( int x = 0; x < scale_rcp; ++x ) {
            ivec2 fullres_xy = clamp( base_fullres + ivec2( x, y ), ivec2( 0 ), ivec2( frame.resolution ) - ivec2( 1 ) );
            float depth = texelFetch( global_textures[ nonuniformEXT( frame.depth_texture_index ) ], fullres_xy, 0 ).r;

            if ( depth < best_depth ) {
                best_depth = depth;
                best_fullres_xy = fullres_xy;
            }
        }
    }

    representative_depth = best_depth;

    return best_fullres_xy;
}

float blur_variance_3x3( SVGFConstants constants, SVGFOutputs outputs, ivec2 p ) {
    const float kernel[2][2] = {
        { 1.0 / 4.0, 1.0 / 8.0  },
        { 1.0 / 8.0, 1.0 / 16.0 }
    };

    float g = 0.0;
    float sum = 0.0;

    for ( int yy = -1; yy <= 1; ++yy ) {
        for ( int xx = -1; xx <= 1; ++xx ) {
            ivec2 s = p + ivec2( xx, yy );

            if ( outside_svgf_resolution( constants, s ) ) {
                continue;
            }

            float k = kernel[ abs( xx ) ][ abs( yy ) ];
            float v = texelFetch( global_textures[ outputs.variance_texture_index ], s, 0 ).r;

            g += v * k;
            sum += k;
        }
    }

    return sum > 0.0 ? g / sum : 0.0;
}

float compute_normal_depth_weight( SVGFConstants constants, vec3 n_p, vec2 linear_z_dd, ivec2 q, float phi_depth ) {
    // q is already in SVGF/denoiser-resolution space.
    vec2 encoded_normal_q = texelFetch( global_textures[ nonuniformEXT( constants.current_normals_texture_index ) ], q, 0 ).rg;
    vec3 n_q = octahedral_decode( encoded_normal_q );

    float w_n = pow( clamp( dot( n_p, n_q ), 0.0, 1.0 ), svgf_push.sigma_n );

    float z_q = texelFetch( global_textures[ nonuniformEXT( constants.current_linear_z_dd_texture_index ) ], q, 0 ).r;

    float w_z = 0.0;
    if ( phi_depth > 0.0 ) {
        w_z = abs( linear_z_dd.x - z_q ) / phi_depth;
    }

    return exp( -max( w_z, 0.0 ) ) * w_n;
}

#endif

#if defined( COMPUTE_SVGF_ACCUMULATION )

bool temporal_sample_is_consistent( ivec2 frag_coord, ivec2 prev_frag_coord_offset, uint mesh_id, vec2 depth_normal_fwidth, float z, vec3 normal ) {
    if ( outside_svgf_resolution( svgf, prev_frag_coord_offset ) ) {
        return false;
    }

    uint prev_mesh_id = texelFetch( global_utextures[ nonuniformEXT( svgf.history_mesh_id_texture_index ) ], prev_frag_coord_offset, 0 ).r;
    if ( mesh_id != prev_mesh_id ) {
        return false;
    }

    float prev_z = texelFetch( global_textures[ nonuniformEXT( svgf.history_linear_depth_texture ) ], prev_frag_coord_offset, 0 ).r;
    float depth_diff = abs( prev_z - z ) / ( depth_normal_fwidth.x + 1e-2 );
    if ( depth_diff > svgf.temporal_depth_difference ) {
        return false;
    }

    vec2 prev_encoded_normal = texelFetch( global_textures[ nonuniformEXT( svgf.history_normals_texture_index ) ], prev_frag_coord_offset, 0 ).rg;
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
    vec4 history_output_color = texelFetch( global_textures[ nonuniformEXT( outputs.history_output_texture_index ) ], prev_frag_coord_offset, 0 );
    if ( any( isnan( history_output_color.rgb ) ) || any( isinf( history_output_color.rgb ) ) ) {
        return;
    }

    vec2 moment = texelFetch( global_textures[ nonuniformEXT( outputs.history_moments_texture_index ) ], prev_frag_coord_offset, 0 ).rg;

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

void check_temporal_consistency( ivec2 frag_coord, vec2 prev_frag_coord,
                                 out bool is_consistent_reflections, out vec3 history_color_reflections, out vec2 history_moments_reflections, out uint history_count_reflections,
                                 out bool is_consistent_restirgi, out vec3 history_color_restirgi, out vec2 history_moments_restirgi, out uint history_count_restirgi ) {
    is_consistent_reflections = false;
    is_consistent_restirgi = false;
    history_color_reflections = vec3( 0 );
    history_color_restirgi = vec3( 0 );
    history_moments_reflections = vec2( 0 );
    history_moments_restirgi = vec2( 0 );
    history_count_reflections = 1;
    history_count_restirgi = 1;

    ivec2 base = ivec2( floor( prev_frag_coord ) );
    if ( outside_svgf_resolution( svgf, base ) ) {
        return;
    }

    uint mesh_id = texelFetch( global_utextures[ nonuniformEXT( svgf.current_mesh_id_texture_index ) ], frag_coord, 0 ).r;
    vec2 depth_normal_fwidth = texelFetch( global_textures[ nonuniformEXT( svgf.current_depth_normal_fwidth_texture_index ) ], frag_coord, 0 ).rg;
    float z = texelFetch( global_textures[ nonuniformEXT( svgf.current_linear_z_dd_texture_index ) ], frag_coord, 0 ).r;
    vec2 encoded_normal = texelFetch( global_textures[ nonuniformEXT( svgf.current_normals_texture_index ) ], frag_coord, 0 ).rg;
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

#if defined (DEBUG_SVGF)
    // Enalble this to see convergence of history
    history_count_restirgi = fallback_restirgi ? 1u : 32u;
#endif // DEBUG_SVGF
}

void validate_checkerboard( SVGFConstants constants, SVGFOutputs outputs, ivec2 input_xy, ivec2 offset, inout vec3 color, inout float sum ) {
    vec2 encoded_normal = texelFetch( global_textures[ constants.normals_texture_index ], input_xy, 0 ).rg;
    vec3 n = octahedral_decode( encoded_normal );

    encoded_normal = texelFetch( global_textures[ constants.normals_texture_index ], input_xy + offset, 0 ).rg;
    vec3 other_n = octahedral_decode( encoded_normal );

    if ( dot( n, other_n ) < 0.95 ) {
        return;
    }

    float z = texelFetch( global_textures[ constants.linear_z_dd_texture_index ], input_xy, 0 ).r;
    float other_z = texelFetch( global_textures[ constants.linear_z_dd_texture_index ], input_xy + offset, 0 ).r;

    if ( abs( z - other_z ) >= 0.05 ) {
        return;
    }

    color += texelFetch( global_textures[ outputs.output_texture_index ], input_xy + offset, 0 ).rgb;
    sum += 1.0;
}

vec3 checkerboard_color( SVGFConstants constants, SVGFOutputs outputs, ivec2 input_xy ) {
    if ( constants.output_resolution_scale == constants.input_resolution_scale ) {
        return texelFetch( global_textures[ outputs.output_texture_index ], input_xy, 0 ).rgb;
    } else {
        ivec2 scaled_xy = input_xy * 2;

        vec3 color = texelFetch( global_textures[ outputs.output_texture_index ], scaled_xy, 0 ).rgb;
        float sum = 1;

        validate_checkerboard( constants, outputs, scaled_xy, ivec2( 0, 1 ), color, sum );
        validate_checkerboard( constants, outputs, scaled_xy, ivec2( 1, 0 ), color, sum );
        validate_checkerboard( constants, outputs, scaled_xy, ivec2( 1, 1 ), color, sum );

        return color / sum;
    }
}

void accumulate_signal( SVGFOutputs outputs, ivec2 frag_coord, vec3 output_color, bool is_consistent,
                        vec3 history_output_color, vec2 history_moments, uint moment_history_count ) {
    if ( any( isnan( output_color.rgb ) ) || any( isinf( output_color.rgb ) ) ) {
        output_color = vec3( 0 );
    }

    float u_1 = luminance( output_color );
    vec2 moments = vec2( u_1, u_1 * u_1 );

    vec3 integrated_color_out = vec3( 0 );
    vec2 integrated_moments_out = vec2( 0 );

    // vec2 history_moments = texelFetch( global_textures[ svgf.history_moments_texture_index ], ivec2( prev_frag_coord ), 0 ).rg;
    // uint moment_history_count = uint ( texelFetch( global_textures[ svgf.history_output_texture_index ], ivec2( prev_frag_coord ), 0 ).a );
    
#if DEBUG_ACCUMULATION
    if ( is_consistent && frame.current_frame > 250 ) {
#else
    if ( is_consistent && !( svgf_push.step_size == 1 ) ) {
#endif
        moment_history_count = min( moment_history_count + 1u, 32u );

        float alpha = max( 1.0 / float( moment_history_count ), 0.05 );
#if DEBUG_ACCUMULATION
        integrated_color_out = history_output_color;
#else
        integrated_color_out = output_color * alpha + ( 1 - alpha ) * history_output_color;
#endif
        integrated_moments_out = moments * alpha + ( 1 - alpha ) * history_moments;
    } else {
        integrated_color_out = output_color;
        integrated_moments_out = moments;
        moment_history_count = 1;
    }

    imageStore( global_images_2d[ outputs.integrated_color_texture_index ], frag_coord, vec4( integrated_color_out, float( moment_history_count ) ) );
    imageStore( global_images_2d[ outputs.integrated_moments_texture_index ], frag_coord, vec4( integrated_moments_out, 0, 0 ) );
}

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
void main() {
    ivec2 frag_coord = ivec2( gl_GlobalInvocationID.xy );

    if ( outside_svgf_resolution( svgf, frag_coord ) ) {
        return;
    }

    vec2 motion_vector = texelFetch( global_textures[ nonuniformEXT( svgf.current_motion_vectors_texture_index ) ], frag_coord, 0 ).rg;

    vec2 history_resolution = vec2( svgf_resolution( svgf ) );
    vec2 current_uv = ( vec2( frag_coord ) + 0.5 ) / history_resolution;
    vec2 prev_uv = reproject_uv_from_ndc_motion(current_uv, motion_vector);
    vec2 prev_frag_coord = prev_uv * history_resolution - 0.5;

    bool is_consistent_reflections = false;
    bool is_consistent_restirgi = false;
    vec3 history_color_reflections = vec3( 0 );
    vec3 history_color_restirgi = vec3( 0 );
    vec2 history_moments_reflections = vec2( 0 );
    vec2 history_moments_restirgi = vec2( 0 );
    uint history_count_reflections = 1;
    uint history_count_restirgi = 1;

    check_temporal_consistency( frag_coord, prev_frag_coord,
                                is_consistent_reflections, history_color_reflections, history_moments_reflections, history_count_reflections,
                                is_consistent_restirgi, history_color_restirgi, history_moments_restirgi, history_count_restirgi );

    accumulate_signal( svgf_reflections, frag_coord, checkerboard_color( svgf, svgf_reflections, frag_coord ), is_consistent_reflections, history_color_reflections, history_moments_reflections, history_count_reflections );
    accumulate_signal( svgf_restirgi, frag_coord, checkerboard_color( svgf, svgf_restirgi, frag_coord ), is_consistent_restirgi, history_color_restirgi, history_moments_restirgi, history_count_restirgi );
}

#endif // COMPUTE_SVGF_ACCUMULATION

#if defined( COMPUTE_SVGF_VARIANCE )

void write_signal_variance( SVGFOutputs outputs, ivec2 frag_coord, bool needs_filtering, vec2 moments_sum, float sum_weights ) {
    float variance = 0.0;

    if ( needs_filtering ) {
        if ( sum_weights > 0.0 ) {
            vec2 moments = moments_sum / sum_weights;
            variance = max( moments.y - pow( moments.x, 2.0 ), 0.0 );
        }
    } else {
        vec2 moments = texelFetch( global_textures[ nonuniformEXT( outputs.integrated_moments_texture_index ) ], frag_coord, 0 ).rg;
        variance = max( moments.y - pow( moments.x, 2.0 ), 0.0 );
    }

    imageStore( global_images_2d[ nonuniformEXT( outputs.variance_texture_index ) ], frag_coord, vec4( variance, 0, 0, 0 ) );
}

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
void main() {
    ivec2 frag_coord = ivec2( gl_GlobalInvocationID.xy );

    if ( outside_svgf_resolution( svgf, frag_coord ) ) return;

    uint reflection_history_count = uint( texelFetch( global_textures[ nonuniformEXT( svgf_reflections.integrated_color_texture_index ) ], frag_coord, 0 ).a );
    uint restirgi_history_count = uint( texelFetch( global_textures[ nonuniformEXT( svgf_restirgi.integrated_color_texture_index ) ], frag_coord, 0 ).a );

    bool filter_reflections = reflection_history_count < 4;
    bool filter_restirgi = restirgi_history_count < 4;

    vec2 moments_sum_reflections = vec2( 0 );
    vec2 moments_sum_restirgi = vec2( 0 );
    float sum_weights_reflections = 0.0;
    float sum_weights_restirgi = 0.0;

    if ( filter_reflections || filter_restirgi ) {
        vec2 encoded_normal_p = texelFetch( global_textures[ nonuniformEXT( svgf.current_normals_texture_index ) ], frag_coord, 0 ).rg;
        vec3 normal_p = octahedral_decode( encoded_normal_p );

        vec2 linear_z_dd = texelFetch( global_textures[ nonuniformEXT( svgf.current_linear_z_dd_texture_index ) ], frag_coord, 0 ).rg;
        uint mesh_id_p = texelFetch( global_utextures[ nonuniformEXT( svgf.current_mesh_id_texture_index ) ], frag_coord, 0 ).r;

        int filter_size = 3;
        float phi_depth = svgf_push.sigma_z * max( linear_z_dd.y, 1e-8 ) * filter_size;

        for ( int y = -filter_size; y <= filter_size; ++y ) {
            for ( int x = -filter_size; x <= filter_size; ++x ) {
                ivec2 q = frag_coord + ivec2( x, y );

                if ( outside_svgf_resolution( svgf, q ) ) {
                    continue;
                }

                uint mesh_id_q = texelFetch( global_utextures[ nonuniformEXT( svgf.current_mesh_id_texture_index ) ], q, 0 ).r;
                if ( mesh_id_p != mesh_id_q ) {
                    continue;
                }

                float w_pq = compute_normal_depth_weight( svgf, normal_p, linear_z_dd, q, phi_depth );

                if ( filter_reflections ) {
                    moments_sum_reflections += w_pq * texelFetch( global_textures[ nonuniformEXT( svgf_reflections.integrated_moments_texture_index ) ], q, 0 ).rg;
                    sum_weights_reflections += w_pq;
                }

                if ( filter_restirgi ) {
                    moments_sum_restirgi += w_pq * texelFetch( global_textures[ nonuniformEXT( svgf_restirgi.integrated_moments_texture_index ) ], q, 0 ).rg;
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

// Weights are different from the paper and reflect the falcor implementation
float h[ 3 ] = {
    1.0,
    2.0 / 3.0,
    1.0 / 6.0
};

//#define DEBUG_SVGF

void filter_signal_sample( SVGFOutputs outputs, ivec2 q, float h_q, float normal_depth_weight,
                           float variance_p, float luminance_p, inout vec3 filtered_color,
                           inout float color_weight, inout float updated_variance ) {
    vec3 c_q = texelFetch( global_textures[ nonuniformEXT( outputs.integrated_color_texture_index ) ], q, 0 ).rgb;
    float l_q = luminance( c_q );

    // Luminance/variance weight.
    // variance_p should already be a small blurred variance around p.
    float sigma_l_variance = svgf_push.sigma_l * sqrt( max( variance_p, 0.0 ) ) + 1e-5;
    float w_l = abs( luminance_p - l_q ) / sigma_l_variance;
    float w_pq = exp( -max( w_l, 0.0 ) ) * normal_depth_weight;

    float prev_variance = texelFetch( global_textures[ nonuniformEXT( outputs.variance_texture_index ) ], q, 0 ).r;
    float sample_weight = h_q * w_pq;

    filtered_color += sample_weight * c_q;
    color_weight += sample_weight;
    updated_variance += pow( h_q, 2.0 ) * pow( w_pq, 2.0 ) * prev_variance;
}

void store_filtered_signal( SVGFOutputs outputs, ivec2 frag_coord, vec3 filtered_color, float color_weight, float updated_variance ) {
    filtered_color /= color_weight;
    updated_variance /= pow( color_weight, 2.0 );

    uint moment_history_count = uint( texelFetch( global_textures[ nonuniformEXT( outputs.integrated_color_texture_index ) ], frag_coord, 0 ).a );

#if defined (DEBUG_SVGF)
    if ( svgf_push.step_size == 16u ) {
        filtered_color = vec3( float( moment_history_count ) / 32.0 );
    }
#endif // DEBUG_SVGF

    imageStore( global_images_2d[ nonuniformEXT( outputs.filtered_color_texture_index ) ], frag_coord, vec4( filtered_color, moment_history_count ) );
    imageStore( global_images_2d[ nonuniformEXT( outputs.updated_variance_texture_index ) ], frag_coord, vec4( updated_variance, 0, 0, 0 ) );
}

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
void main() {
    ivec2 frag_coord = ivec2( gl_GlobalInvocationID.xy );

    if ( outside_svgf_resolution( svgf, frag_coord ) ) return;

    float new_variance_reflections = texelFetch( global_textures[ svgf_reflections.variance_texture_index ], frag_coord, 0 ).r;
    float new_variance_restirgi = texelFetch( global_textures[ svgf_restirgi.variance_texture_index ], frag_coord, 0 ).r;

    vec2 encoded_normal_p = texelFetch( global_textures[ nonuniformEXT( svgf.current_normals_texture_index ) ], frag_coord, 0 ).rg;
    vec3 normal_p = octahedral_decode( encoded_normal_p );

    vec2 linear_z_dd = texelFetch( global_textures[ nonuniformEXT( svgf.current_linear_z_dd_texture_index ) ], frag_coord, 0 ).rg;
    uint mesh_id_p = texelFetch( global_utextures[ nonuniformEXT( svgf.current_mesh_id_texture_index ) ], frag_coord, 0 ).r;

    vec3 color_p_reflections = texelFetch( global_textures[ nonuniformEXT( svgf_reflections.integrated_color_texture_index ) ], frag_coord, 0 ).rgb;
    vec3 color_p_restirgi = texelFetch( global_textures[ nonuniformEXT( svgf_restirgi.integrated_color_texture_index ) ], frag_coord, 0 ).rgb;
    float luminance_p_reflections = luminance( color_p_reflections );
    float luminance_p_restirgi = luminance( color_p_restirgi );

    // Reduce kernel at fine scale for performances
    int radius = ( svgf_push.step_size >= 8u ) ? 1 : 2;

    const float phi_depth = svgf_push.sigma_z * max( linear_z_dd.y, 1e-8 ) * svgf_push.step_size;

    vec3 new_filtered_color_reflections = color_p_reflections;
    vec3 new_filtered_color_restirgi = color_p_restirgi;
    float color_weight_reflections = 1.0;
    float color_weight_restirgi = 1.0;

    float variance_p_reflections = blur_variance_3x3( svgf, svgf_reflections, frag_coord );
    float variance_p_restirgi = blur_variance_3x3( svgf, svgf_restirgi, frag_coord );

    for ( int y = -radius; y <= radius; ++y ) {
        for ( int x = -radius; x <= radius; ++x ) {
            ivec2 q = frag_coord + ivec2( x, y ) * int( svgf_push.step_size );

            if ( outside_svgf_resolution( svgf, q ) ) {
                continue;
            }

            if ( x == 0 && y == 0 ) {
                continue;
            }

            uint mesh_id_q = texelFetch( global_utextures[ nonuniformEXT( svgf.current_mesh_id_texture_index ) ], q, 0 ).r;
            if ( mesh_id_p != mesh_id_q ) {
                continue;
            }

            float h_q = h[ abs( x ) ] * h[ abs( y ) ];
            float normal_depth_weight = compute_normal_depth_weight( svgf, normal_p, linear_z_dd, q, phi_depth );

            filter_signal_sample( svgf_reflections, q, h_q, normal_depth_weight, variance_p_reflections, luminance_p_reflections,
                                  new_filtered_color_reflections, color_weight_reflections, new_variance_reflections );
            filter_signal_sample( svgf_restirgi, q, h_q, normal_depth_weight, variance_p_restirgi, luminance_p_restirgi,
                                  new_filtered_color_restirgi, color_weight_restirgi, new_variance_restirgi );
        }
    }

    // if ( svgf_push.step_size == 16u ) {
    //     // Maximum 5x5 kernel weight is ~7.11 including the center.
    //     float support = ( color_weight_restirgi - 1.0 ) / 6.111;

    //     new_filtered_color_restirgi = vec3( clamp( support, 0.0, 1.0 ) );
    //     color_weight_restirgi = 1.0;
    // }

    // if ( svgf_push.step_size == 16u ) {
    //     float variance = texelFetch(
    //         global_textures[
    //             nonuniformEXT( svgf_restirgi.variance_texture_index )
    //         ],
    //         frag_coord,
    //         0 ).r;

    //     new_filtered_color_restirgi = vec3( variance );
    //     color_weight_restirgi = 1.0;
    // }

    store_filtered_signal( svgf_reflections, frag_coord, new_filtered_color_reflections, color_weight_reflections, new_variance_reflections );
    store_filtered_signal( svgf_restirgi, frag_coord, new_filtered_color_restirgi, color_weight_restirgi, new_variance_restirgi );
}

#endif // COMPUTE_SVGF_WAVELET

#if defined(COMPUTE_SVGF_DOWNSAMPLE)

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
void main() {
    ivec2 frag_coord = ivec2( gl_GlobalInvocationID.xy );

    if ( outside_svgf_resolution( svgf, frag_coord ) ) {
        return;
    }

    float representative_depth = -1.f;
    ivec2 fullres_xy = choose_svgf_representative_fullres_pixel( svgf, frag_coord, representative_depth );

    // Write the current-frame representative sample of all guide textures.
    vec2 normals = texelFetch( global_textures[ nonuniformEXT( svgf.normals_texture_index ) ], fullres_xy, 0 ).rg;

    ivec2 base_fullres = frag_coord * 2;
    ivec2 representative_offset = fullres_xy - base_fullres;

    uint representative_index = uint( representative_offset.y * 2 + representative_offset.x );
    // Use empty 2 channels to store raw depth and subpixel
    imageStore( global_images_2d[ nonuniformEXT( svgf.current_normals_texture_index ) ], frag_coord, vec4( normals, representative_depth, float(representative_index) ) );

    uvec4 mesh_id = texelFetch( global_utextures[ nonuniformEXT( svgf.mesh_id_texture_index ) ], fullres_xy, 0 );
    imageStore( global_uimages_2d[ nonuniformEXT( svgf.current_mesh_id_texture_index ) ], frag_coord, mesh_id );

    vec4 linear_z_dd = texelFetch( global_textures[ nonuniformEXT( svgf.linear_z_dd_texture_index ) ], fullres_xy, 0 );
    imageStore( global_images_2d[ nonuniformEXT( svgf.current_linear_z_dd_texture_index ) ], frag_coord, linear_z_dd );

    vec4 depth_normal_fwidth = texelFetch( global_textures[ nonuniformEXT( svgf.depth_normal_fwidth_texture_index ) ], fullres_xy, 0 );
    imageStore( global_images_2d[ nonuniformEXT( svgf.current_depth_normal_fwidth_texture_index ) ], frag_coord, depth_normal_fwidth );

    vec4 motion_vectors = texelFetch( global_textures[ nonuniformEXT( svgf.motion_vectors_texture_index ) ], fullres_xy, 0 );
    imageStore( global_images_2d[ nonuniformEXT( svgf.current_motion_vectors_texture_index ) ], frag_coord, motion_vectors );
}

#endif // COMPUTE_SVGF_DOWNSAMPLE
