#version 460

#extension GL_GOOGLE_include_directive : enable

#include "platform.glslh"
#include "frame.h"
#include "mesh.h"
#include "sampling.h"
#include "debug_rendering.h"

// Common Raytracing code ////////////////////////////////////////////////
#if defined (RAYGEN_REFLECTIONS_RT) || defined (CLOSEST_HIT_REFLECTIONS_RT) || defined (MISS_REFLECTIONS_RT)

#include "lighting.h"

#extension GL_EXT_ray_tracing : enable

struct RayPayload {
    int instance_id;
    int geometry_id;
    int primitive_id;
    vec2 barycentric_weights;
    mat4x3 object_to_world;
    uint triangle_facing;
    float t;
};

struct ReflectionConstants {
    uint sbt_offset; // shader binding table offset
    uint sbt_stride; // shader binding table stride
    uint miss_index;
    uint out_image_index;

    uvec4 gbuffer_texures; // x = roughness, y= normals, z = indirect lighting
};

layout( set = MATERIAL_SET, binding = 40 ) uniform ReflectionsConstants {
    ReflectionConstants reflections;
};

struct ReflectionPushConstants {
    float resolution_scale_rcp;
};

layout( push_constant ) uniform ReflectionPushConstantsBlock {
    ReflectionPushConstants reflections_push;
};

layout( set = MATERIAL_SET, binding = 26 ) uniform accelerationStructureEXT as;

#endif

#if defined (RAYGEN_REFLECTIONS_RT)

layout( location = 0 ) rayPayloadEXT RayPayload payload;

#define REFLECTION_USE_GUIDE 1

ivec2 choose_reflection_representative_fullres_pixel( ivec2 denoiser_xy ) {
    int scale_rcp = max( 1, int( round( reflections_push.resolution_scale_rcp ) ) );

    if ( scale_rcp == 1 ) {
        return clamp( denoiser_xy, ivec2( 0 ), ivec2( frame.resolution ) - ivec2( 1 ) );
    }

    ivec2 base_fullres = ivec2( vec2( denoiser_xy ) * reflections_push.resolution_scale_rcp );

    ivec2 best_fullres_xy = clamp( base_fullres, ivec2( 0 ), ivec2( frame.resolution ) - ivec2( 1 ) );
    float best_depth = 1.0;

    for ( int y = 0; y < scale_rcp; ++y ) {
        for ( int x = 0; x < scale_rcp; ++x ) {
            ivec2 fullres_xy = clamp( base_fullres + ivec2( x, y ), ivec2( 0 ), ivec2( frame.resolution ) - ivec2( 1 ) );
            float depth = texelFetch( global_textures[ nonuniformEXT( frame.depth_texture_index ) ], fullres_xy, 0 ).r;

            // Assumes standard Z: smaller depth is closer.
            if ( depth < best_depth ) {
                best_depth = depth;
                best_fullres_xy = fullres_xy;
            }
        }
    }

    return best_fullres_xy;
}

const ivec2 representative_offsets[ 4 ] = ivec2[](
    ivec2( 0, 0 ),
    ivec2( 1, 0 ),
    ivec2( 0, 1 ),
    ivec2( 1, 1 )
);

void main() {
    ivec2 xy = ivec2( gl_LaunchIDEXT.xy );
    ivec2 test_fragment = ivec2( frame.resolution ) / 2;

    bool render_debug_line = false;//( xy == test_fragment );
    vec4 white =  vec4( 1 );
    vec4 black =  vec4( 0, 0, 0, 1 );
    vec4 yellow = vec4( 1, 1, 0, 1 );
    vec4 red =    vec4( 1, 0, 0, 1 );
    vec4 green =  vec4( 0, 1, 0, 1 );
    vec4 blue =   vec4( 0, 0, 1, 1 );

#if REFLECTION_USE_GUIDE
    // Use half res guide texture
    vec4 guide = texelFetch( global_textures[ reflections.gbuffer_texures.y ], xy, 0 );
    vec2 encoded_normal = guide.rg;
    float raw_depth = guide.b;
    uint representative_index = uint( round( guide.a ) );

    ivec2 fullres_xy = xy * 2 + representative_offsets[ representative_index ];
#else
    // Manually choose best representative pixel
    ivec2 fullres_xy = choose_reflection_representative_fullres_pixel( xy );
    vec2 encoded_normal = texelFetch( global_textures[ reflections.gbuffer_texures.y ], fullres_xy, 0 ).rg;
    float raw_depth = texelFetch( global_textures[ frame.depth_texture_index ], fullres_xy, 0 ).r;
#endif // REFLECTION_USE_GUIDE

    float roughness = frame.forced_roughness > 0.0 ? frame.forced_roughness : texelFetch( global_textures[ reflections.gbuffer_texures.x ], fullres_xy, 0 ).y;
    roughness = max( 0.001, roughness);

    uint rng_state = seed( gl_LaunchIDEXT.xy ) + frame.current_frame;

    float rnd_normalizer = 1.0 / float( 0xFFFFFFFFu );

    // Rand should be in [0..1] values
    //vec2 U = vec2( pcg2d( gl_LaunchIDEXT.xy + uvec2( frame.current_frame ) ) ) * rnd_normalizer;
    vec2 U = interleaved_gradient_noise2( xy, frame.current_frame );

    vec3 reflection_colour = vec3( 0 );

    // Debug
    //U = vec2(.5, .5);

    if ( roughness <= 0.666 ) {

        vec3 normal = octahedral_decode( encoded_normal );

        vec2 screen_uv = uv_nearest( fullres_xy, frame.resolution );
        vec3 world_pos = world_position_from_depth( screen_uv, raw_depth, frame.inverse_view_projection );

        vec3 incoming = normalize( frame.camera_position.xyz - world_pos );

        float alpha = roughness * roughness;
        mat3 local_frame = make_tangent_frame( normal );
        vec3 wo_local = world_to_local( local_frame, incoming );
        vec3 vndf_normal_local = sampleGGXVNDF( wo_local, alpha, alpha, U.x, U.y );
        vec3 vndf_normal_world = normalize( local_frame * vndf_normal_local );

        vec3 reflected_ray = normalize( reflect( -incoming, vndf_normal_world ) );
        //vec3 mirror_ray = normalize( reflect( -incoming, normal ) );

        traceRayEXT( as, // topLevel
                 gl_RayFlagsOpaqueEXT, // rayFlags
                 0xff, // cullMask
                 reflections.sbt_offset, // sbtRecordOffset
                 reflections.sbt_stride, // sbtRecordStride
                 reflections.miss_index, // missIndex
                 world_pos, // origin
                 0.05, // Tmin
                 reflected_ray, // direction
                 100.0, // Tmax
                 0 // payload index
                );

        if ( payload.instance_id != -1 ) {
            uint mesh_instance_index = payload.instance_id + payload.geometry_id;
            MeshInstanceDraw instance = mesh_instance_draws[ mesh_instance_index ];
            uint mesh_index = instance.mesh_draw_index;
            MeshDraw mesh = mesh_draws[ mesh_index ];

            int_array_type index_buffer = int_array_type( mesh.index_buffer );
            int i0 = index_buffer[ payload.primitive_id * 3 ].v;
            int i1 = index_buffer[ payload.primitive_id * 3 + 1 ].v;
            int i2 = index_buffer[ payload.primitive_id * 3 + 2 ].v;

            float_array_type vertex_buffer = float_array_type( mesh.position_buffer );
            vec4 p0 = vec4(
                vertex_buffer[ i0 * 3 + 0 ].v,
                vertex_buffer[ i0 * 3 + 1 ].v,
                vertex_buffer[ i0 * 3 + 2 ].v,
                1.0
            );
            vec4 p1 = vec4(
                vertex_buffer[ i1 * 3 + 0 ].v,
                vertex_buffer[ i1 * 3 + 1 ].v,
                vertex_buffer[ i1 * 3 + 2 ].v,
                1.0
            );
            vec4 p2 = vec4(
                vertex_buffer[ i2 * 3 + 0 ].v,
                vertex_buffer[ i2 * 3 + 1 ].v,
                vertex_buffer[ i2 * 3 + 2 ].v,
                1.0
            );

            vec4 p0_world = vec4( payload.object_to_world * p0, 1.0 );
            vec4 p1_world = vec4( payload.object_to_world * p1, 1.0 );
            vec4 p2_world = vec4( payload.object_to_world * p2, 1.0 );

            float flip_normal = payload.triangle_facing == gl_HitKindFrontFacingTriangleEXT ? 1 : -1;
            vec3 triangle_normal = normalize( cross( p1_world.xyz - p0_world.xyz, p2_world.xyz - p0_world.xyz ) ) * flip_normal;

            vec4 p0_screen = frame.view_projection * p0_world;
            vec4 p1_screen = frame.view_projection * p1_world;
            vec4 p2_screen = frame.view_projection * p2_world;

            ivec2 texture_size = textureSize( global_textures[ nonuniformEXT( mesh.textures.x ) ], 0 );

            vec2_array_type uv_buffer = vec2_array_type( mesh.uv_buffer );
            vec2 uv0 = uv_buffer[ i0 ].v;
            vec2 uv1 = uv_buffer[ i1 ].v;
            vec2 uv2 = uv_buffer[ i2 ].v;

            // TODO(marco): use ray differentials
            float texel_area = texture_size.x * texture_size.y * abs( ( uv1.x - uv0.x ) * ( uv2.y - uv0.y ) - ( uv2.x - uv0.x ) * ( uv1.y - uv0.y ) );
            float triangle_area = abs( ( p1_screen.x - p0_screen.x ) * ( p2_screen.y - p0_screen.y ) - ( p2_screen.x - p0_screen.x ) * ( p1_screen.y - p0_screen.y ) );
            float lod = floor( 0.5 * log2( texel_area / triangle_area ) );

            float b = payload.barycentric_weights.x;
            float c = payload.barycentric_weights.y;
            float a = 1 - b - c;

            vec2 uv = ( a * uv0 + b * uv1 + c * uv2 );
            vec3 p_world = world_pos + reflected_ray * payload.t;

            if ( render_debug_line ) {
                debug_draw_line( world_pos, p_world, white, yellow );
                debug_draw_line( p_world, p_world + ( triangle_normal * 2 ), white, red );
            }

            float lights_importance[ NUM_LIGHTS ];
            float total_importance = 0.0;

            for ( uint l = 0; l < frame.active_lights; ++l ) {
                // Compute light importance by using something similar to "Importance Sampling of Many Lights on the GPU"
                Light light = lights[ l ];
                vec3 p_to_light = light.world_position - p_world.xyz;

                float point_light_angle = dot( normalize( p_to_light ), triangle_normal );

                float distance_sq = dot( p_to_light, p_to_light );
                float r_sq = light.radius * light.radius;

                bool light_active = ( point_light_angle > 1e-4 ) && ( distance_sq <= r_sq );
                float theta_u = asin( light.radius / sqrt( distance_sq ) );

                // TODO(marco): can we avoid using acos?
                float theta_i = acos( point_light_angle );

                float theta_prime = max( 0, theta_i - theta_u );
                float orientation = abs( cos( theta_prime ) );

                float importance = ( light.intensity * orientation ) / distance_sq;

                float final_value = light_active ? importance : 0.0;
                lights_importance[ l ] = final_value;

                total_importance += final_value;
            }

            for ( uint l = 0; l < frame.active_lights; ++l ) {
                lights_importance[ l ] /= total_importance;
            }

            float rnd_value = rand_pcg( rng_state ) * rnd_normalizer;

            uint light_index = 0;
            float accum_probability = 0.0;
            for ( ; light_index < frame.active_lights; ++light_index ) {
                accum_probability += lights_importance[ light_index ];

                if ( accum_probability > rnd_value ) {
                    break;
                }
            }

            if ( light_index < frame.active_lights ) {
                Light light = lights[ light_index ];
                vec3 p_to_light = light.world_position - p_world.xyz;
                vec3 l = normalize( p_to_light );
                float light_distance = sqrt( dot( p_to_light, p_to_light ) );

                traceRayEXT( as, // topLevel
                    gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT,
                    0xff, // cullMask
                    reflections.sbt_offset, // sbtRecordOffset
                    reflections.sbt_stride, // sbtRecordStride
                    reflections.miss_index, // missIndex
                    p_world.xyz, // origin
                    0.05, // Tmin
                    l, // direction
                    light_distance, // Tmax
                    0 // payload index
                    );

                float shadow_term = payload.instance_id == -1 ? 1.0 : 0.0;

                if ( render_debug_line ) {
                    debug_draw_line( p_world, p_world + l * light_distance, yellow, yellow );
                }

                // TODO(marco): refactor this to use calculate_point_light_contribution
                float attenuation = attenuation_square_falloff( p_to_light, 1.0f / light.radius ) * shadow_term;
                float NoL = clamp(dot( triangle_normal, l ), 0.0, 1.0);

                if ( attenuation > 0.0001f  && NoL > 0.0001f ) {
                    vec3 orm = calculate_pbr_parameters( mesh.metallic_roughness_occlusion_factor.x, mesh.metallic_roughness_occlusion_factor.y,
                                                        mesh.textures.y, mesh.metallic_roughness_occlusion_factor.z, mesh.textures.w, uv );

                    vec3 view = normalize( world_pos - p_world.xyz );
                    float NoV = saturate( dot( triangle_normal, view ));

                    float roughness = frame.forced_roughness > 0.0 ? frame.forced_roughness : orm.g * orm.g;
                    float metallic = frame.forced_metalness > 0.0 ? frame.forced_metalness : orm.b;

                    vec4 albedo = textureLod( global_textures[ nonuniformEXT( mesh.textures.x ) ], uv, lod );

                    vec3 light_intensity = NoL * light.intensity * attenuation * light.color;

                    reflection_colour = albedo.rgb * light_intensity;
                }

                // reflection_colour = vec3( shadow_term, 0, 0 );
            }

            // Add raytraced light contribution
            if ( is_raytrace_shadow_point_light() ) {

            }
            else {
                vec3 l = normalize( raytraced_shadow_light_position );
                float NoL = (dot( triangle_normal, l ));

                if ( NoL >= 0.0 ) {

                    // Reuse instance id - setting a value that is not -1 as the closest hit is not called.
                    // Default to hit, and if missing, only the miss shader will be set and instance id will be set to -1.
                    payload.instance_id = 0;
                    vec3 secondary_ray_start = p_world.xyz + triangle_normal * 0.02;

                    traceRayEXT( as,
                        gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT,
                        0xff,
                        reflections.sbt_offset, reflections.sbt_stride, reflections.miss_index, secondary_ray_start, 0.01, l, 100.0, 0 );

                    float shadow_term = payload.instance_id == -1 ? 1.0 : 0.0;

                    if ( render_debug_line ) {
                        debug_draw_line( p_world, p_world + l * 20.0, yellow, yellow );
                    }

                    if ( shadow_term > 0.0 ) {
                        uint packed_color = raytraced_shadow_light_color_type & 0x00ffffffu;
                        vec4 albedo = textureLod( global_textures[ nonuniformEXT( mesh.textures.x ) ], uv, lod );
                        vec3 light_intensity = NoL * raytraced_shadow_light_intensity * shadow_term * unpack_color_rgba(raytraced_shadow_light_color_type).rgb;

                        reflection_colour += albedo.rgb * light_intensity;
                    }
                }
            }
            // Indirect light sampling
            // vec3 indirect_color = sample_irradiance( p_world.xyz, triangle_normal, camera_position.xyz );
            // reflection_colour += indirect_color;
        }
    }

    imageStore( global_images_2d[ reflections.out_image_index ], ivec2( gl_LaunchIDEXT.xy ), vec4( reflection_colour, 1 ) );
}

#endif

#if defined (CLOSEST_HIT_REFLECTIONS_RT)

layout( location = 0 ) rayPayloadInEXT RayPayload payload;
hitAttributeEXT vec2 barycentric_weights;

void main() {

    payload.instance_id = gl_InstanceCustomIndexEXT;
    payload.geometry_id = gl_GeometryIndexEXT;
    payload.primitive_id = gl_PrimitiveID;
    payload.barycentric_weights = barycentric_weights;
    payload.object_to_world = gl_ObjectToWorldEXT;
    payload.t = gl_HitTEXT;
    payload.triangle_facing = gl_HitKindEXT;
}

#endif

#if defined (MISS_REFLECTIONS_RT)

layout( location = 0 ) rayPayloadInEXT RayPayload payload;

void main() {
    payload.instance_id = -1;
    payload.geometry_id = -1;
}

#endif

#if defined( COMPUTE_SVGF_ACCUMULATION ) || defined( COMPUTE_SVGF_VARIANCE ) || defined( COMPUTE_SVGF_WAVELET ) || defined(COMPUTE_SVGF_DOWNSAMPLE)

#define DEBUG_ACCUMULATION 0

struct SVGFConstants {
    uint motion_vectors_texture_index;
    uint mesh_id_texture_index;
    uint normals_texture_index;
    uint depth_normal_fwidth_texture_index;

    uint current_motion_vectors_texture_index;
    uint current_mesh_id_texture_index;
    uint current_normals_texture_index;
    uint current_depth_normal_fwidth_texture_index;

    uint current_linear_z_dd_texture_index;
    uint history_mesh_id_texture_index;
    uint history_normals_texture_index;
    uint history_linear_depth_texture;

    uint output_texture_index;
    uint history_output_texture_index;
    uint history_moments_texture_index;
    uint integrated_color_texture_index;

    uint integrated_moments_texture_index;
    uint variance_texture_index;
    uint filtered_color_texture_index;
    uint updated_variance_texture_index;

    uint linear_z_dd_texture_index;
    float output_resolution_scale;
    float output_resolution_scale_rcp;
    float temporal_depth_difference;

    float temporal_normal_difference;
    float input_resolution_scale;
    float input_resolution_scale_rcp;
    float pad002;
};

layout( set = MATERIAL_SET, binding = 40 ) uniform SVGFAccumulationConstants {
    SVGFConstants svgf;
};

struct SVGFPushConstants {
    uint step_size;
    float sigma_z;
    float sigma_n;
    float sigma_l;
};

layout( push_constant ) uniform SVGFPushConstantsBlock {
    SVGFPushConstants svgf_push;
};

ivec2 svgf_resolution() {
    return ivec2( ceil( vec2( frame.resolution ) * svgf.output_resolution_scale ) );
}

bool outside_svgf_resolution( ivec2 p ) {
    return any( lessThan( p, ivec2( 0 ) ) ) || any( greaterThanEqual( p, svgf_resolution() ) );
}

ivec2 halfres_to_fullres( ivec2 p ) {
    ivec2 xy = ivec2( ( vec2( p ) + 0.5 ) * svgf.output_resolution_scale_rcp );
    return clamp( xy, ivec2( 0 ), ivec2( frame.resolution ) - ivec2( 1 ) );
}

ivec2 choose_svgf_representative_fullres_pixel( ivec2 denoiser_xy, out float representative_depth ) {
    int scale_rcp = max( 1, int( round( svgf.output_resolution_scale_rcp ) ) );

    if ( scale_rcp == 1 ) {
        return clamp( denoiser_xy, ivec2( 0 ), ivec2( frame.resolution ) - ivec2( 1 ) );
    }

    ivec2 base_fullres = ivec2( vec2( denoiser_xy ) * svgf.output_resolution_scale_rcp );

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

float blur_variance_3x3( ivec2 p ) {
    const float kernel[2][2] = {
        { 1.0 / 4.0, 1.0 / 8.0  },
        { 1.0 / 8.0, 1.0 / 16.0 }
    };

    float g = 0.0;
    float sum = 0.0;

    for ( int yy = -1; yy <= 1; ++yy ) {
        for ( int xx = -1; xx <= 1; ++xx ) {
            ivec2 s = p + ivec2( xx, yy );

            if ( outside_svgf_resolution( s ) ) {
                continue;
            }

            float k = kernel[ abs( xx ) ][ abs( yy ) ];
            float v = texelFetch( global_textures[ svgf.variance_texture_index ], s, 0 ).r;

            g += v * k;
            sum += k;
        }
    }

    return sum > 0.0 ? g / sum : 0.0;
}

float compute_w_fast( vec3 n_p, vec2 linear_z_dd, float l_p, float l_q, ivec2 q, float phi_depth, float variance_p ) {
    // q is already in SVGF/denoiser-resolution space.
    vec2 encoded_normal_q = texelFetch( global_textures[ nonuniformEXT( svgf.current_normals_texture_index ) ], q, 0 ).rg;
    vec3 n_q = octahedral_decode( encoded_normal_q );

    float w_n = pow( clamp( dot( n_p, n_q ), 0.0, 1.0 ), svgf_push.sigma_n );

    float z_q = texelFetch( global_textures[ nonuniformEXT( svgf.current_linear_z_dd_texture_index ) ], q, 0 ).r;

    float w_z = 0.0;
    if ( phi_depth > 0.0 ) {
        w_z = abs( linear_z_dd.x - z_q ) / phi_depth;
    }

    // Luminance/variance weight.
    // variance_p should already be a small blurred variance around p.
    float sigma_l_variance = svgf_push.sigma_l * sqrt( max( variance_p, 0.0 ) ) + 1e-5;
    float w_l = abs( l_p - l_q ) / sigma_l_variance;

    return exp( -max( w_l, 0.0 ) - max( w_z, 0.0 ) ) * w_n;
}

float compute_w( vec3 n_p, vec2 linear_z_dd, float l_p, float l_q, ivec2 p, ivec2 q, float phi_depth ) {
    // q is already in SVGF/denoiser-resolution space.
    vec2 encoded_normal_q = texelFetch( global_textures[ nonuniformEXT( svgf.current_normals_texture_index ) ], q, 0 ).rg;
    vec3 n_q = octahedral_decode( encoded_normal_q );

    float w_n = pow( clamp( dot( n_p, n_q ), 0.0, 1.0 ), svgf_push.sigma_n );

    float z_q = texelFetch( global_textures[ nonuniformEXT( svgf.current_linear_z_dd_texture_index ) ], q, 0 ).r;

    float w_z = 0.0;
    if ( phi_depth > 0.0 ) {
        w_z = abs( linear_z_dd.x - z_q ) / phi_depth;
    }

    float g = blur_variance_3x3( p );
    float w_l = abs( l_p - l_q ) / ( svgf_push.sigma_l * sqrt( max( 0.0, g ) ) + 1e-5 );

    return exp( -max( w_l, 0.0 ) - max( w_z, 0.0 ) ) * w_n;
}

float compute_w_normal_depth( vec3 n_p, vec2 linear_z_dd, ivec2 p, ivec2 q, float phi_depth ) {
    // q is already in SVGF/denoiser-resolution space.
    vec2 encoded_normal_q = texelFetch( global_textures[ nonuniformEXT( svgf.current_normals_texture_index ) ], q, 0 ).rg;
    vec3 n_q = octahedral_decode( encoded_normal_q );

    float w_n = pow( clamp( dot( n_p, n_q ), 0.0, 1.0 ), svgf_push.sigma_n );

    float z_q = texelFetch( global_textures[ nonuniformEXT( svgf.current_linear_z_dd_texture_index ) ], q, 0 ).r;

    float w_z = 0.0;
    if ( phi_depth > 0.0 ) {
        w_z = abs( linear_z_dd.x - z_q ) / phi_depth;
    }

    return exp( -max( w_z, 0.0 ) ) * w_n;
}

#endif

#if defined( COMPUTE_SVGF_ACCUMULATION )

bool check_temporal_consistency( ivec2 frag_coord, vec2 prev_frag_coord, out vec3 history_color, out vec2 history_moments, out uint history_count ) {

    // NOTE(marco): previous sample is outside texture
    ivec2 base = ivec2( floor( prev_frag_coord ) );
    if ( outside_svgf_resolution( base ) ) {
#if DEBUG_ACCUMULATION
        history_color = vec3( 1, 1, 1 );
        return true;
#else
        return false;
#endif
    }

    uint mesh_id = texelFetch( global_utextures[ nonuniformEXT( svgf.current_mesh_id_texture_index ) ], frag_coord, 0 ).r;
    vec2 depth_normal_fwidth = texelFetch( global_textures[ nonuniformEXT( svgf.current_depth_normal_fwidth_texture_index ) ], frag_coord, 0 ).rg;
    float z = texelFetch( global_textures[ nonuniformEXT( svgf.current_linear_z_dd_texture_index ) ], frag_coord, 0 ).r;
    vec2 encoded_normal = texelFetch( global_textures[ nonuniformEXT( svgf.current_normals_texture_index ) ], frag_coord, 0 ).rg;
    vec3 normal = octahedral_decode( encoded_normal );

    uint count = 0;
    vec3 color_sum = vec3( 0 );
    vec2 moments_sum = vec2( 0 );
    float weight_sum = 0;

    vec2 f = fract( prev_frag_coord );

    // Data needed to find the best count sample
    float best_count_weight = 0.0;
    uint best_history_count = 1u;
    bool used_fallback = false;

    // 2x2 bilinear filter
    for ( int y = 0; y <=1; ++y ) {
        for ( int x = 0; x <= 1; ++x ) {
            ivec2 prev_frag_coord_offset = base + ivec2( x, y );

            if ( outside_svgf_resolution( prev_frag_coord_offset ) ) {
#if DEBUG_ACCUMULATION
                history_color = vec3( 1, 1, 1 );
                count = 1;
                break;
#else
                continue;
#endif
            }

            uint prev_mesh_id = texelFetch( global_utextures[ nonuniformEXT( svgf.history_mesh_id_texture_index ) ], prev_frag_coord_offset, 0 ).r;

            if ( mesh_id != prev_mesh_id ) {
#if DEBUG_ACCUMULATION
                history_color = vec3( 1, 0, 0 );
                count = 1;
                break;
#else
                continue;
#endif
            }

            float prev_z = texelFetch( global_textures[ nonuniformEXT( svgf.history_linear_depth_texture ) ], prev_frag_coord_offset, 0 ).r;

            float depth_diff = abs( prev_z - z ) / ( depth_normal_fwidth.x + 1e-2 );

            if ( depth_diff > svgf.temporal_depth_difference ) {
#if DEBUG_ACCUMULATION
                history_color = vec3( 0, 1, 0 );
                count = 1;
                break;
#else
                continue;
#endif
            }

            vec2 prev_encoded_normal = texelFetch( global_textures[ nonuniformEXT( svgf.history_normals_texture_index ) ], prev_frag_coord_offset, 0 ).rg;
            vec3 prev_normal = octahedral_decode( prev_encoded_normal );

            float normal_diff = distance( normal, prev_normal ) / ( depth_normal_fwidth.y + 1e-2 );
            if ( normal_diff > svgf.temporal_normal_difference ) {
#if DEBUG_ACCUMULATION
                history_color = vec3( 0, 0, 1 );
                count = 1;
                break;
#else
                continue;
#endif
            }

            vec4 history_output_color = texelFetch( global_textures[ nonuniformEXT( svgf.history_output_texture_index ) ], prev_frag_coord_offset, 0 );
            if ( any( isnan( history_output_color.rgb ) ) || any( isinf( history_output_color.rgb ) ) ) {
#if DEBUG_ACCUMULATION
                history_color = vec3( 1, 1, 0 );
                count = 1;
                break;
#else
                continue;
#endif
            }

            vec2 moment = texelFetch( global_textures[ nonuniformEXT( svgf.history_moments_texture_index ) ], ivec2( prev_frag_coord_offset ), 0 ).rg;
            float wx = ( x == 0 ) ? 1.0 - f.x : f.x;
            float wy = ( y == 0 ) ? 1.0 - f.y : f.y;
            float w = wx * wy;

            color_sum += history_output_color.rgb * w;
            moments_sum += moment * w;
            weight_sum += w;
            count += 1;

            uint previous_count = uint( clamp( history_output_color.a, 1.0, 32.0 ) );

            if ( w > best_count_weight ) {
                best_count_weight = w;
                best_history_count = previous_count;
            }
        }
    }

    if ( count == 0 || weight_sum < 0.25 ) {

        used_fallback = true;

        // Reset sums and weights
        count = 0;
        color_sum = vec3( 0.0 );
        moments_sum = vec2( 0.0 );
        weight_sum = 0.0;
        best_count_weight = 0.0;
        best_history_count = 1u;

        // 3x3 filter
        for ( int y = -1; y <= 1; ++y ) {
            for ( int x = -1; x <= 1; ++x ) {
                ivec2 prev_frag_coord_offset = base + ivec2( x, y );

                if ( outside_svgf_resolution( prev_frag_coord_offset ) ) {
#if DEBUG_ACCUMULATION
                    history_color = vec3( 1, 1, 1 );
                    count = 2;
                    break;
#else
                    continue;
#endif
                }

                uint prev_mesh_id = texelFetch( global_utextures[ nonuniformEXT( svgf.history_mesh_id_texture_index ) ], prev_frag_coord_offset, 0 ).r;

                if ( mesh_id != prev_mesh_id ) {
#if DEBUG_ACCUMULATION
                    history_color = vec3( 1, 0, 0 );
                    count = 2;
                    break;
#else
                    continue;
#endif
                }

                float prev_z = texelFetch( global_textures[ nonuniformEXT( svgf.history_linear_depth_texture ) ], prev_frag_coord_offset, 0 ).r;

                float depth_diff = abs( prev_z - z ) / ( depth_normal_fwidth.x + 1e-2 );

                if ( depth_diff > svgf.temporal_depth_difference ) {
#if DEBUG_ACCUMULATION
                    history_color = vec3( 0, 1, 0 );
                    count = 2;
                    break;
#else
                    continue;
#endif
                }

                vec2 prev_encoded_normal = texelFetch( global_textures[ nonuniformEXT( svgf.history_normals_texture_index ) ], prev_frag_coord_offset, 0 ).rg;
                vec3 prev_normal = octahedral_decode( prev_encoded_normal );

                float normal_diff = distance( normal, prev_normal ) / ( depth_normal_fwidth.y + 1e-2 );
                if ( normal_diff > svgf.temporal_normal_difference ) {
#if DEBUG_ACCUMULATION
                    history_color = vec3( 0, 0, 1 );
                    count = 2;
                    break;
#else
                    continue;
#endif
                }

                vec4 history_output_color = texelFetch( global_textures[ nonuniformEXT( svgf.history_output_texture_index ) ], prev_frag_coord_offset, 0 );
                if ( any( isnan( history_output_color.rgb ) ) || any( isinf( history_output_color.rgb ) ) ) {
#if DEBUG_ACCUMULATION
                    history_color = vec3( 1, 1, 0 );
                    count = 2;
                    break;
#else
                    continue;
#endif
                }

                vec2 moment = texelFetch( global_textures[ nonuniformEXT( svgf.history_moments_texture_index ) ], ivec2( prev_frag_coord_offset ), 0 ).rg;

                color_sum += history_output_color.rgb;
                moments_sum += moment;
                weight_sum += 1.0;
                count += 1;

                uint previous_count = uint( clamp( history_output_color.a, 1.0, 32.0 ) );

                // Weight is 1.0
                if ( 1.0 > best_count_weight ) {
                    best_count_weight = 1.0;
                    best_history_count = previous_count;
                }
            }
        }
    }

#if DEBUG_ACCUMULATION
    history_count = count;
    return true;
#else
    bool valid = ( count != 0 ) && ( weight_sum > 1e-2 );
    if ( valid ) {
        history_color = color_sum / weight_sum;
        history_moments = moments_sum / weight_sum;
        history_count = used_fallback ? min( best_history_count, 4u ) : best_history_count;
    }

    return valid;
#endif
}

void validate_checkerboard( ivec2 input_xy, ivec2 offset, inout vec3 color, inout float sum ) {
    vec2 encoded_normal = texelFetch( global_textures[ svgf.normals_texture_index ], input_xy, 0 ).rg;
    vec3 n = octahedral_decode( encoded_normal );

    encoded_normal = texelFetch( global_textures[ svgf.normals_texture_index ], input_xy + offset, 0 ).rg;
    vec3 other_n = octahedral_decode( encoded_normal );

    if ( dot( n, other_n ) < 0.95 ) return;

    float z = texelFetch( global_textures[ svgf.linear_z_dd_texture_index ], input_xy, 0 ).r;
    float other_z = texelFetch( global_textures[ svgf.linear_z_dd_texture_index ], input_xy + offset, 0 ).r;

    if ( abs( z - other_z ) >= 0.05 ) return;

    color += texelFetch( global_textures[ svgf.output_texture_index ], input_xy + offset, 0 ).rgb;
    sum += 1.0;
}

vec3 checkerboard_color( ivec2 input_xy ) {
    if ( svgf.output_resolution_scale == svgf.input_resolution_scale ) {
        return texelFetch( global_textures[ svgf.output_texture_index ], input_xy, 0 ).rgb;
    } else {
        ivec2 scaled_xy = input_xy * 2;

        vec3 color = texelFetch( global_textures[ svgf.output_texture_index ], scaled_xy, 0 ).rgb;
        float sum = 1;

        validate_checkerboard( scaled_xy, ivec2( 0, 1 ), color, sum );
        validate_checkerboard( scaled_xy, ivec2( 1, 0 ), color, sum );
        validate_checkerboard( scaled_xy, ivec2( 1, 1 ), color, sum );

        return color / sum;
    }
}

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
void main() {
    uvec2 frag_coord = gl_GlobalInvocationID.xy;

    if ( outside_svgf_resolution( ivec2( frag_coord ) ) ) {
        return;
    }

    vec3 output_color = checkerboard_color( ivec2( frag_coord ) );
    bool debug_color = false;
    if ( any( isnan( output_color.rgb ) ) || any( isinf( output_color.rgb ) ) ) {
        output_color = vec3( 0 );
    }

    float u_1 = luminance( output_color );
    float u_2 = u_1 * u_1;
    vec2 moments = vec2( u_1, u_2 );

    // Current motion vectors have already been downsampled to SVGF resolution using the same representative pixel as the guide buffers.
    vec2 motion_vector = texelFetch( global_textures[ nonuniformEXT( svgf.current_motion_vectors_texture_index ) ], ivec2( frag_coord ), 0 ).rg;

    vec2 history_resolution = vec2( svgf_resolution() );
    vec2 current_uv = ( vec2( frag_coord ) + 0.5 ) / history_resolution;

    // Same as TAA, input current_uv = half-res center.
    vec2 prev_uv = reproject_uv_from_ndc_motion(current_uv, motion_vector);
    vec2 prev_frag_coord = prev_uv * history_resolution - 0.5;

    vec3 history_output_color = vec3( 0 );
    vec2 history_moments = vec2( 0 );
    uint moment_history_count = 1;
    bool is_consistent = check_temporal_consistency( ivec2( frag_coord ), prev_frag_coord, history_output_color, history_moments, moment_history_count );

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

        float alpha = 0.05;
        alpha = max( 1.0 / float( moment_history_count ), 0.05 );
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


    float c = float(moment_history_count) / 32.0;
    //integrated_color_out = vec3(c);
    //integrated_color_out = is_consistent ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);

    imageStore( global_images_2d[ svgf.integrated_color_texture_index ], ivec2( frag_coord ), vec4( integrated_color_out, float( moment_history_count ) ) );
    imageStore( global_images_2d[ svgf.integrated_moments_texture_index ], ivec2( frag_coord ), vec4( integrated_moments_out, 0, 0 ) );
}

#endif // COMPUTE_SVGF_ACCUMULATION

#if defined( COMPUTE_SVGF_VARIANCE )

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
void main() {
    ivec2 frag_coord = ivec2( gl_GlobalInvocationID.xy );

    if ( outside_svgf_resolution( frag_coord ) ) return;

    uint moment_history_count = uint( texelFetch( global_textures[ nonuniformEXT( svgf.integrated_color_texture_index ) ], frag_coord, 0 ).a );

    vec2 encoded_normal_p = texelFetch( global_textures[ nonuniformEXT( svgf.current_normals_texture_index ) ], frag_coord, 0 ).rg;
    vec3 normal_p = octahedral_decode( encoded_normal_p );

    vec2 linear_z_dd = texelFetch( global_textures[ nonuniformEXT( svgf.current_linear_z_dd_texture_index ) ], frag_coord, 0 ).rg;
    uint mesh_id_p = texelFetch( global_utextures[ nonuniformEXT( svgf.current_mesh_id_texture_index ) ], frag_coord, 0 ).r;

    int filter_size = 3;
    float phi_depth = max( linear_z_dd.y, 1e-8 ) * filter_size;

    float variance = 0.0;
    if ( moment_history_count < 4 ) {
        vec2 moments = vec2( 0 );
        float sum_weights = 0.0;
        for ( int y = -filter_size; y <= filter_size; ++y ) {
            for ( int x = -filter_size; x <= filter_size; ++x ) {
                ivec2 offset = ivec2( x, y );
                ivec2 q = frag_coord + offset;

                if ( outside_svgf_resolution( q ) ) {
                    continue;
                }

                uint mesh_id_q = texelFetch( global_utextures[ nonuniformEXT( svgf.current_mesh_id_texture_index ) ], q, 0 ).r;
                if ( mesh_id_p != mesh_id_q ) {
                    continue;
                }

                float w_pq = compute_w_normal_depth( normal_p, linear_z_dd, frag_coord, q, phi_depth );

                moments += w_pq * texelFetch( global_textures[ nonuniformEXT( svgf.integrated_moments_texture_index ) ], q, 0 ).rg;
                sum_weights += w_pq;
            }
        }

        if ( sum_weights > 0.0 ) {
            moments /= sum_weights;
            variance = max( moments.y - pow( moments.x, 2.0 ), 0.0 );
        }
    } else {
        vec2 moments = texelFetch( global_textures[ nonuniformEXT( svgf.integrated_moments_texture_index ) ], frag_coord, 0 ).rg;
        variance = max( moments.y - pow( moments.x, 2.0 ), 0.0 );
    }

    imageStore( global_images_2d[ nonuniformEXT( svgf.variance_texture_index ) ], frag_coord, vec4( variance, 0, 0, 0 ) );
}

#endif // COMPUTE_SVGF_VARIANCE

#if defined( COMPUTE_SVGF_WAVELET )

// Weights are different from the paper and reflect the falcor implementation
float h[ 3 ] = {
    1.0,
    2.0 / 3.0,
    1.0 / 6.0
};

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
void main() {
    ivec2 frag_coord = ivec2( gl_GlobalInvocationID.xy );

    if ( outside_svgf_resolution( frag_coord ) ) return;

    float new_variance = 0.0;

    vec2 encoded_normal_p = texelFetch( global_textures[ nonuniformEXT( svgf.current_normals_texture_index ) ], frag_coord, 0 ).rg;
    vec3 normal_p = octahedral_decode( encoded_normal_p );

    vec2 linear_z_dd = texelFetch( global_textures[ nonuniformEXT( svgf.current_linear_z_dd_texture_index ) ], frag_coord, 0 ).rg;
    uint mesh_id_p = texelFetch( global_utextures[ nonuniformEXT( svgf.current_mesh_id_texture_index ) ], frag_coord, 0 ).r;

    vec3 color_p = texelFetch( global_textures[ nonuniformEXT( svgf.integrated_color_texture_index ) ], frag_coord, 0 ).rgb;
    float luminance_p = luminance( color_p );

    const int radius = 2;

    const float phi_depth = max( linear_z_dd.y, 1e-8 ) * svgf_push.step_size;

    vec3 new_filtered_color = color_p;
    float color_weight = 1.0;

    float variance_p = blur_variance_3x3( frag_coord );

    for ( int y = -radius; y <= radius; ++y ) {
        for ( int x = -radius; x <= radius; ++x ) {
            ivec2 offset = ivec2( x, y );
            ivec2 q = frag_coord + offset * int( svgf_push.step_size );

            if ( outside_svgf_resolution( q ) ) {
                continue;
            }

            if ( x == 0 && y == 0 ) {
                continue;
            }

            uint mesh_id_q = texelFetch( global_utextures[ nonuniformEXT( svgf.current_mesh_id_texture_index ) ], q, 0 ).r;
            if ( mesh_id_p != mesh_id_q ) {
                continue;
            }

            vec3 c_q = texelFetch( global_textures[ nonuniformEXT( svgf.integrated_color_texture_index ) ], q, 0 ).rgb;
            float l_q = luminance( c_q );

            float h_q = h[ abs( x ) ] * h[ abs( y ) ];

            //float w_pq = compute_w( normal_p, linear_z_dd, luminance_p, l_q, frag_coord, q, phi_depth );
            float w_pq = compute_w_fast( normal_p, linear_z_dd, luminance_p, l_q, q, phi_depth, variance_p );

            float prev_variance = texelFetch( global_textures[ nonuniformEXT( svgf.variance_texture_index ) ], q, 0 ).r;

            float sample_weight = h_q * w_pq;

            new_filtered_color += sample_weight * c_q;
            color_weight += sample_weight;

            new_variance += pow( h_q, 2.0 ) * pow( w_pq, 2.0 ) * prev_variance;
        }
    }

    new_filtered_color /= color_weight;
    new_variance /= pow( color_weight, 2.0 );

    uint moment_history_count = uint( texelFetch( global_textures[ nonuniformEXT( svgf.integrated_color_texture_index ) ], frag_coord, 0 ).a );

    imageStore( global_images_2d[ nonuniformEXT( svgf.filtered_color_texture_index ) ], frag_coord, vec4( new_filtered_color, moment_history_count ) );
    imageStore( global_images_2d[ nonuniformEXT( svgf.updated_variance_texture_index ) ], frag_coord, vec4( new_variance, 0, 0, 0 ) );
}

#endif // COMPUTE_SVGF_WAVELET

#if defined(COMPUTE_BRDF_LUT_GENERATION)

float radical_inverse_vdc(uint bits) {

    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);

    return float(bits) * 2.3283064365386963e-10; // / 0x100000000
}

vec2 hammersley(uint i, uint N) {
    return vec2(float(i) / float(N), radical_inverse_vdc(i));
}

vec3 importance_sample_ggx(vec2 Xi, vec3 N, float roughness) {
    float a = roughness * roughness;

    float phi = 2.0 * PI * Xi.x;
    float cos_theta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
    float sin_theta = sqrt(1.0 - cos_theta * cos_theta);

    vec3 H;
    H.x = cos(phi) * sin_theta;
    H.y = sin(phi) * sin_theta;
    H.z = cos_theta;

    vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, N));
    vec3 bitangent = cross(N, tangent);

    vec3 sample_vec = H.x * tangent + H.y * bitangent + H.z * N;
    return normalize(sample_vec);
}

float geometry_schlick_ggx(float NdotV, float roughness) {
    float a = roughness;
    float k = (a * a) / 2.0f;
    float nom = NdotV;
    float denom = NdotV * (1.0 - k) + k;
    return nom / denom;
}

float geometry_smith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = geometry_schlick_ggx(NdotV, roughness);
    float ggx1 = geometry_schlick_ggx(NdotL, roughness);
    return ggx1 * ggx2;
}

vec2 integrate_brdf(float NdotV, float roughness) {
    vec3 V;
    V.x = sqrt(1.0 - NdotV * NdotV);
    V.y = 0.0f;
    V.z = NdotV;

    float A = 0.0;
    float B = 0.0;

    vec3 N = vec3(0.0, 0.0, 1.0);

    const uint sample_count = 1024u;
    for(uint i = 0u; i < sample_count; ++i) {
        vec2 Xi = hammersley(i, sample_count);
        vec3 H = importance_sample_ggx(Xi, N, roughness);
        vec3 L = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = max(L.z, 0.0);
        float NdotH = max(H.z, 0.0);
        float VdotH = max(dot(V, H), 0.0);

        if(NdotL > 0.0)
        {
            float G = geometry_smith(N, V, L, roughness);
            float G_Vis = (G * VdotH) / (NdotH * NdotV);
            float Fc = pow(1.0 - VdotH, 5.0);
            A += (1.0 - Fc) * G_Vis;
            B += Fc * G_Vis;
        }
    }
    A /= float(sample_count);
    B /= float(sample_count);
    return vec2(A, B);
}

struct BrdfLutPushConstants {
    uint output_texture_index;
    uint output_texture_size;
};

layout( push_constant ) uniform BrdfLutPushConstantsBlock {
    BrdfLutPushConstants brdf_lut_push;
};

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
void main() {
    ivec2 frag_coord = ivec2( gl_GlobalInvocationID.xy );
    vec2 uv = uv_nearest( frag_coord, vec2(brdf_lut_push.output_texture_size) );
    vec2 integrated_brdf = integrate_brdf( uv.x, 1 - uv.y );
    imageStore( global_images_2d[ brdf_lut_push.output_texture_index ], frag_coord, vec4( integrated_brdf, 0, 0 ) );
}

#endif // COMPUTE_BRDF_LUT_GENERATION

#if defined(COMPUTE_SVGF_DOWNSAMPLE)

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
void main() {
    ivec2 frag_coord = ivec2( gl_GlobalInvocationID.xy );

    if ( outside_svgf_resolution( frag_coord ) ) {
        return;
    }

    float representative_depth = -1.f;
    ivec2 fullres_xy = choose_svgf_representative_fullres_pixel( frag_coord, representative_depth );

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
