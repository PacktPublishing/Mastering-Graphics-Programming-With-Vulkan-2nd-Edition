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
    // Safe clamp to have valid values
    int representative_index = clamp( int( round( guide.a ) ), 0, 3 );

    int scale_rcp = max( 1, int( round( reflections_push.resolution_scale_rcp ) ) );
    ivec2 fullres_xy = clamp( xy * scale_rcp + representative_offsets[ representative_index ],
                              ivec2( 0 ), ivec2( frame.resolution ) - ivec2( 1 ) );

    // Debug: use full res texture
    //raw_depth = texelFetch( global_textures[ nonuniformEXT( frame.depth_texture_index ) ], fullres_xy, 0 ).r;
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
    vec2 U = animated_blue_noise_golden_ratio( xy, frame.current_frame, frame.blue_noise_128_rg_texture_index );

    vec3 reflection_colour = vec3( 0 );

    // Debug
   // U = vec2(.5, .5);

    if ( roughness <= 0.333 ) {

        vec3 normal = octahedral_decode( encoded_normal );

        vec2 screen_uv = uv_nearest( fullres_xy, frame.resolution );
        vec3 world_pos = world_position_from_depth( screen_uv, raw_depth, frame.inverse_view_projection );

        vec3 incoming = normalize( frame.camera_position.xyz - world_pos );
        if ( dot(normal, incoming) < 0.0 ) {
            normal = -normal;
        }

        float alpha = roughness * roughness;
        mat3 local_frame = make_tangent_frame( normal );
        vec3 wo_local = world_to_local( local_frame, incoming );

        vec3 vndf_normal_local = sampleGGXVNDF( wo_local, alpha, alpha, U.x, U.y );
        vec3 vndf_normal_world = normalize( local_frame * vndf_normal_local );

        vec3 ray_origin = world_pos + normal * 0.02;

        vec3 reflected_ray = normalize( reflect( -incoming, vndf_normal_world ) );
        //reflected_ray = normalize( reflect( -incoming, normal ) );

        if ( dot( reflected_ray, normal ) <= 0.001f ) {
            // Ray is inside or under the ggx surface, no contribution.
            // Write a value otherwise ghosting and flickering can happen.
            imageStore( global_images_2d[ reflections.out_image_index ], ivec2( gl_LaunchIDEXT.xy ), vec4( 0, 0, 0, 1 ) );
            return;
        }

        traceRayEXT( as, // topLevel
                 gl_RayFlagsOpaqueEXT, // rayFlags
                 0xff, // cullMask
                 reflections.sbt_offset, // sbtRecordOffset
                 reflections.sbt_stride, // sbtRecordStride
                 reflections.miss_index, // missIndex
                 ray_origin, // origin
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

            // Debug: hit, no light selected
            //reflection_colour = vec3( 1, 0, 0 );

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

            vec4 p0_world = vec4( instance.model * p0 );
            vec4 p1_world = vec4( instance.model * p1 );
            vec4 p2_world = vec4( instance.model * p2 );

            float flip_normal = payload.triangle_facing == gl_HitKindFrontFacingTriangleEXT ? 1 : -1;
            vec3 triangle_normal = normalize( cross( p1_world.xyz - p0_world.xyz, p2_world.xyz - p0_world.xyz ) ) * flip_normal;

            float b = payload.barycentric_weights.x;
            float c = payload.barycentric_weights.y;
            float a = 1 - b - c;

            float_array_type normals_buffer = float_array_type( mesh.normals_buffer );
            vec3 n0 = vec3(normals_buffer[ i0 * 3 + 0 ].v,
                           normals_buffer[ i0 * 3 + 1 ].v,
                           normals_buffer[ i0 * 3 + 2 ].v );

            vec3 n1 = vec3(normals_buffer[ i1 * 3 + 0 ].v,
                           normals_buffer[ i1 * 3 + 1 ].v,
                           normals_buffer[ i1 * 3 + 2 ].v );

            vec3 n2 = vec3(normals_buffer[ i2 * 3 + 0 ].v,
                           normals_buffer[ i2 * 3 + 1 ].v,
                           normals_buffer[ i2 * 3 + 2 ].v );

            vec3 shading_normal = normalize( ( a * n0 + b * n1 + c * n2 ) * mat3( instance.model_inverse ) );

            //shading_normal = triangle_normal;
            shading_normal *= flip_normal;

            vec4 p0_screen = frame.view_projection * p0_world;
            vec4 p1_screen = frame.view_projection * p1_world;
            vec4 p2_screen = frame.view_projection * p2_world;

            ivec2 texture_size = textureSize( global_textures[ nonuniformEXT( mesh.textures.x ) ], 0 );

            // Raygen works at lower resolution, take in account for mip calculation
            vec2 screen_size = vec2( frame.resolution ) / max( 1.0, reflections_push.resolution_scale_rcp );

            vec2_array_type uv_buffer = vec2_array_type( mesh.uv_buffer );
            vec2 uv0 = uv_buffer[ i0 ].v;
            vec2 uv1 = uv_buffer[ i1 ].v;
            vec2 uv2 = uv_buffer[ i2 ].v;

            // TODO(marco): use ray differentials
            float lod = compute_projected_triangle_lod( p0_world, p1_world, p2_world,
                                                    uv0, uv1, uv2, texture_size, screen_size, frame.view_projection );

            vec2 uv = ( a * uv0 + b * uv1 + c * uv2 );

            // Use precise barycentric based world coorindates
            vec3 p_world = a * p0_world.xyz + b * p1_world.xyz + c * p2_world.xyz;
            //vec3 p_world = world_pos + reflected_ray * payload.t;

            if ( render_debug_line ) {
                debug_draw_line( world_pos, p_world, white, yellow );
                debug_draw_line( p_world, p_world + ( triangle_normal * 2 ), white, red );
            }

            float lights_importance[ NUM_LIGHTS ];
            float total_importance = 0.0;

            uint num_lights = min( frame.active_lights, uint( NUM_LIGHTS ) );

            for ( uint l = 0; l < num_lights; ++l ) {
                // Compute light importance by using something similar to "Importance Sampling of Many Lights on the GPU"
                Light light = lights[ l ];
                vec3 p_to_light = light.world_position - p_world.xyz;

                float point_light_angle = dot( normalize( p_to_light ), shading_normal );

                float distance_sq = max( dot( p_to_light, p_to_light ), 1e-6 );
                float r_sq = light.radius * light.radius;

                bool light_active = ( point_light_angle > 1e-4 ) && ( distance_sq <= r_sq );

                float orientation = point_light_angle;

                // Follow the light attenuation formula
                float factor = distance_sq * ( 1.0 / r_sq );
                float smooth_factor = max( 1.0 - factor * factor, 0.0 );
                float importance = ( light.intensity * orientation * smooth_factor * smooth_factor ) / distance_sq;
                //float importance = ( light.intensity * orientation ) / distance_sq;

                float final_value = light_active ? importance : 0.0;
                lights_importance[ l ] = final_value;

                total_importance += final_value;
            }

            if ( total_importance > 0.0001f ) {
                for ( uint l = 0; l < num_lights; ++l ) {
                    lights_importance[ l ] /= total_importance;
                }    
            }
            
            float rnd_value = rand_pcg( rng_state ) * rnd_normalizer;

            uint light_index = 0;
            float accum_probability = 0.0;
            for ( ; light_index < num_lights; ++light_index ) {
                accum_probability += lights_importance[ light_index ];

                if ( accum_probability > rnd_value ) {
                    break;
                }
            }

            if ( total_importance > 0.0001f && light_index < num_lights ) {

                // Debug: light selected
                //reflection_colour = vec3( 0, 0, 1 );

                Light light = lights[ light_index ];
                vec3 p_to_light = light.world_position - p_world.xyz;
                vec3 l = normalize( p_to_light );
                float light_distance = sqrt( dot( p_to_light, p_to_light ) );

                float offset = max( 0.02, length( p_world - world_pos ) * 0.001f );
                const float shadow_tmin = 0.01;
                float tmax = light_distance - offset;

                vec3 secondary_ray_start = p_world.xyz + triangle_normal * offset;

                // With closer lights tmax could become smaller than tmin, and this is undefined.
                float shadow_term = 1.0;
                if ( tmax > shadow_tmin ) {
                    // Reuse instance id - setting a value that is not -1 as the closest hit is not called.
                    // Default to hit, and if missing, only the miss shader will be set and instance id will be set to -1.
                    payload.instance_id = 0;

                    traceRayEXT( as, gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT,
                                 0xff, reflections.sbt_offset, reflections.sbt_stride, reflections.miss_index,
                                 secondary_ray_start, shadow_tmin, l, tmax, 0 );

                    shadow_term = payload.instance_id == -1 ? 1.0 : 0.0;
                }

                if ( render_debug_line ) {
                    debug_draw_line( p_world, p_world + l * light_distance, yellow, yellow );
                }

                // TODO(marco): refactor this to use calculate_point_light_contribution
                float geo_NoL = dot( triangle_normal, l );
                float terminator_fade = saturate( geo_NoL / 0.1 );

                float attenuation = attenuation_square_falloff( p_to_light, 1.0f / light.radius ) * shadow_term;// * terminator_fade;
                float NoL = clamp(dot( shading_normal, l ), 0.0, 1.0);

                if ( attenuation > 0.0001f  && NoL > 0.0001f ) {
                    vec3 orm = calculate_pbr_parameters( mesh.metallic_roughness_occlusion_factor.x, mesh.metallic_roughness_occlusion_factor.y,
                                                        mesh.textures.y, mesh.metallic_roughness_occlusion_factor.z, mesh.textures.w, uv );

                    vec3 view = normalize( world_pos - p_world.xyz );
                    float NoV = saturate( dot( shading_normal, view ));

                    float roughness = frame.forced_roughness > 0.0 ? frame.forced_roughness : orm.g * orm.g;
                    float metallic = frame.forced_metalness > 0.0 ? frame.forced_metalness : orm.b;

                    vec4 albedo = textureLod( global_textures[ nonuniformEXT( mesh.textures.x ) ], uv, lod );

                    vec3 light_intensity = NoL * light.intensity * attenuation * light.color;

                    float light_pdf = max( lights_importance[ light_index ], 0.000001f );
                    reflection_colour = albedo.rgb * light_intensity / ( PI * light_pdf );

                    //reflection_colour = vec3( attenuation );
                }

                // reflection_colour = vec3( shadow_term, 0, 0 );
            }

            // Add raytraced light contribution
            if ( !is_raytrace_shadow_point_light() ) {
                vec3 l = normalize( light_cb.raytraced_shadow_light_position );
                float NoL = dot( shading_normal, l );

                if ( NoL > 0.0 ) {

                    // Reuse instance id - setting a value that is not -1 as the closest hit is not called.
                    // Default to hit, and if missing, only the miss shader will be set and instance id will be set to -1.
                    payload.instance_id = 0;
                    vec3 secondary_ray_start = p_world.xyz + triangle_normal * 0.02;

                    traceRayEXT( as, gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT, 0xff,
                        reflections.sbt_offset, reflections.sbt_stride, reflections.miss_index, secondary_ray_start, 0.01, l, 100.0, 0 );

                    float shadow_term = payload.instance_id == -1 ? 1.0 : 0.0;

                    if ( render_debug_line ) {
                        debug_draw_line( p_world, p_world + l * 20.0, yellow, yellow );
                    }

                    if ( shadow_term > 0.0 ) {
                        uint packed_color = light_cb.raytraced_shadow_light_color_type & 0x00ffffffu;
                        vec4 albedo = textureLod( global_textures[ nonuniformEXT( mesh.textures.x ) ], uv, lod );
                        vec3 light_intensity = NoL * light_cb.raytraced_shadow_light_intensity * shadow_term * unpack_color_rgba(light_cb.raytraced_shadow_light_color_type).rgb;

                        reflection_colour += albedo.rgb * light_intensity / PI;
                    }
                }
            }
            // Indirect light sampling
            // vec3 indirect_color = sample_irradiance( p_world.xyz, shading_normal, camera_position.xyz );
            // reflection_colour += indirect_color;
        } // 
        else {
            // Debug: green, primary ray in the void
            //reflection_colour = vec3( 0, 1, 0 );
            vec3 l = normalize( light_cb.raytraced_shadow_light_position );

            reflection_colour = sample_procedural_sky( reflected_ray, l );            
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
