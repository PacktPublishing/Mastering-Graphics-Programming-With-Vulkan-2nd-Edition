#version 460

#extension GL_GOOGLE_include_directive : enable

#include "platform.glslh"
#include "frame.h"
#include "../shared_structs.h"

layout ( std140, set = MATERIAL_SET, binding = 40 ) uniform VolumetricFogConstants {
    GpuVolumetricFogConstants vfog;
};

#define FROXEL_DISPATCH_X 8
#define FROXEL_DISPATCH_Y 8
#define FROXEL_DISPATCH_Z 1

// Noise helper functions ////////////////////////////////////////////////
float generate_noise(vec2 pixel, int frame_index, float scale) {
    // Animated blue noise using golden ratio.
    if (vfog.noise_type == 0) {
        vec2 blue_noise_value = animated_blue_noise(uvec2(pixel), uint(frame_index), frame.blue_noise_128_rg_texture_index);

        return triangular_noise(blue_noise_value.x, blue_noise_value.y) * scale;
    }
    // Interleaved gradient noise
    if (vfog.noise_type == 1) {
        float noise0 = interleaved_gradient_noise(pixel, frame_index);
        float noise1 = interleaved_gradient_noise(pixel, frame_index + 1);

        return triangular_noise(noise0, noise1) * scale;
    }

    // Initial noise attempt, left for reference.
    return (interleaved_gradient_noise(pixel, frame_index) * scale) - (scale * 0.5f);
}

float z_jitter_half_range( ivec3 froxel_coord, uint frame_index, uint blue_noise_texture_index ) {

    // Use XY + Z to decorrelate per-slice
    uvec2 p = uvec2(froxel_coord.xy);
    uint  z = uint(froxel_coord.z);

    // Simple per-slice scramble
    p = (p + uvec2(z * 13u, z * 7u)) & 127u;

    // Optional per-frame scramble (only if temporal is enabled)
    p = (p + uvec2(frame_index * 17u, frame_index * 29u)) & 127u;

    vec2 bn = texelFetch(global_textures[nonuniformEXT(blue_noise_texture_index)], ivec2(p), 0).rg;

    // [-0.5, +0.5]
    return bn.r - 0.5;
}

float z_jitter_half_range_static( ivec3 froxel_coord, uint blue_noise_texture_index ) {
    uvec2 p = uvec2(froxel_coord.xy);
    uint  z = uint(froxel_coord.z);

    p = (p + uvec2(z * 13u, z * 7u)) & 127u;

    vec2 bn = texelFetch(global_textures[nonuniformEXT(blue_noise_texture_index)], ivec2(p), 0).rg;

    return bn.r - 0.5;
}

// Coordinate transformations ////////////////////////////////////////////
vec2 uv_from_froxels( vec2 froxel_position, uint width, uint height ) {
    vec2 uv = floor(froxel_position) + .5;
    return uv / vec2(width * 1.f, height * 1.f);
}

vec3 world_from_froxel(ivec3 froxel_coord) {

    vec2 uv = uv_from_froxels( froxel_coord.xy + vec2(0.5) + frame.halton_xy * vfog.temporal_reprojection_jitter_scale, vfog.froxel_dimensions.x, vfog.froxel_dimensions.y );

    // First calculate linear depth
    float depth = froxel_coord.z * 1.0f / float(vfog.froxel_dimensions.z);
    //float linear_depth = vfog.froxel_near + depth * (vfog.froxel_far - vfog.froxel_near);
    // Exponential depth
    // Do not use jittering as it is too much unstable
    //float jittering = generate_noise(froxel_coord.xy * 1.0f, current_frame, vfog.noise_scale);
    //float linear_depth = slice_to_exponential_depth_jittered( vfog.froxel_near, vfog.froxel_far, jittering, froxel_coord.z, int(vfog.froxel_dimensions.z) );
    float linear_depth = slice_to_exponential_depth( vfog.froxel_near, vfog.froxel_far, froxel_coord.z, int(vfog.froxel_dimensions.z) );
    // Then calculate raw depth, necessary to properly calculate world position.
    float raw_depth = linear_depth_to_raw_depth(linear_depth, vfog.froxel_near, vfog.froxel_far);
    return world_position_from_depth( uv, raw_depth, vfog.froxel_inverse_view_projection );
}

vec3 world_from_froxel_no_jitter(ivec3 froxel_coord) {

    vec2 uv = uv_from_froxels( froxel_coord.xy + vec2(0.5), vfog.froxel_dimensions.x, vfog.froxel_dimensions.y );

    // First calculate linear depth
    float depth = froxel_coord.z * 1.0f / float(vfog.froxel_dimensions.z);
    //float linear_depth = vfog.froxel_near + depth * (vfog.froxel_far - vfog.froxel_near);
    // Exponential depth
    float linear_depth = slice_to_exponential_depth( vfog.froxel_near, vfog.froxel_far, froxel_coord.z, int(vfog.froxel_dimensions.z) );
    // Then calculate raw depth, necessary to properly calculate world position.
    float raw_depth = linear_depth_to_raw_depth(linear_depth, vfog.froxel_near, vfog.froxel_far);
    return world_position_from_depth( uv, raw_depth, vfog.froxel_inverse_view_projection );
}

// Phase functions ///////////////////////////////////////////////////////
// Equations from http://patapom.com/topics/Revision2013/Revision%202013%20-%20Real-time%20Volumetric%20Rendering%20Course%20Notes.pdf
float henyey_greenstein(float g, float costh) {
    const float numerator = 1.0 - g * g;
    const float denominator = 4.0 * PI * pow(1.0 + g * g - 2.0 * g * costh, 3.0/2.0);
    return numerator / denominator;
}

float shlick(float g, float costh) {
    const float numerator = 1.0 - g * g;
    const float g_costh = g * costh;
    const float denominator = 4.0 * PI * ((1 + g_costh) * (1 + g_costh));
    return numerator / denominator;
}

float cornette_shanks(float g, float costh) {
    const float numerator = 3.0 * (1.0 - g * g) * (1.0 + costh * costh);
    const float denominator = 4.0 * PI * 2.0 * (2.0 + g * g) * pow(1.0 + g * g - 2.0 * g * costh, 3.0/2.0);
    return numerator / denominator;
}

// As found in "Real-time Rendering of Dynamic Clouds" by Xiao-Lei Fan, Li-Min Zhang, Bing-Qiang Zhang, Yuan Zhang [2014]
float cornette_shanks_approximated(float g, float costh) {
    const float numerator = 3.0 * (1.0 - g * g) * (1.0 + costh * costh);
    const float denominator = 4.0 * PI * 2.0 * (2.0 + g * g) * (1.0 + g * g - 2.0 * g * costh);
    return (numerator / denominator) + (g * costh);
}

// Choose different phase functions
// NOTE: normally in realtime production you will choose one, guarded by ifdef FINAL or similar,
// keeping the others for visual exploration purpose during production.
float phase_function(vec3 V, vec3 L, float g) {
    float cos_theta = dot(V, L);

    if (vfog.phase_function_type == 0) {
        return henyey_greenstein(g, cos_theta);
    }
    if (vfog.phase_function_type == 1) {
        return cornette_shanks(g, cos_theta);
    }
    if (vfog.phase_function_type == 2) {
        return shlick(g, cos_theta);
    }

    return cornette_shanks_approximated(g, cos_theta);
}


// General utility functions /////////////////////////////////////////////
float remap( float value, float oldMin, float oldMax, float newMin,float newMax) {
    return newMin + (value - oldMin) / (oldMax - oldMin) * (newMax - newMin);
}

vec4 scattering_extinction_from_color_density( vec3 color, float density ) {

    const float extinction = vfog.scattering_factor * density;
    return vec4( color * extinction, extinction );
}


#if defined(COMPUTE_INJECT_DATA)

layout (local_size_x = FROXEL_DISPATCH_X, local_size_y = FROXEL_DISPATCH_Y, local_size_z = FROXEL_DISPATCH_Z) in;
void main() {
    ivec3 froxel_coord = ivec3(gl_GlobalInvocationID.xyz);

    vec3 world_position = world_from_froxel_no_jitter(froxel_coord);

    vec4 scattering_extinction = vec4(0);

    vec3 sampling_coord = world_position * ( vfog.volumetric_noise_position_multiplier * 0.1f ) +
                          vec3(1,0.1,1) * frame.current_frame * vfog.volumetric_noise_speed_multiplier * 0.1;
    
    float fog_noise = texture(global_textures_3d[nonuniformEXT(vfog.volumetric_noise_texture_index)], sampling_coord).r;
    fog_noise = saturate(fog_noise);
    fog_noise = mix(fog_noise, fog_noise * fog_noise, 0.5);

    // Add constant fog
    float fog_density = vfog.density_modifier * fog_noise;
    scattering_extinction += scattering_extinction_from_color_density( vec3(0.5), fog_density );
    // Add height fog
    float height_fog = vfog.height_fog_density * exp(-vfog.height_fog_falloff * max(world_position.y, 0)) * fog_noise;
    scattering_extinction += scattering_extinction_from_color_density( vec3(0.5), height_fog );

    // Add density from box
    vec3 box = abs(world_position - vfog.box_position);
    if (all(lessThanEqual(box, vfog.box_half_size))) {

        vec4 box_fog_color = unpack_color_rgba( vfog.box_color );

        scattering_extinction += scattering_extinction_from_color_density( box_fog_color.rgb, vfog.box_fog_density * fog_noise);
    }

    imageStore( global_images_3d[vfog.froxel_data_texture_index], froxel_coord.xyz, scattering_extinction );
}

#endif

#if defined(COMPUTE_LIGHT_SCATTERING)

#include "lighting.h"

float decode_local_z(float depth, float radius, float rcp_n_minus_f) {
    float f = radius;
    float n = 0.01;

    float numerator = n * f * rcp_n_minus_f;
    float denominator = depth + f * rcp_n_minus_f;

    return numerator / denominator;
}

layout (local_size_x = FROXEL_DISPATCH_X, local_size_y = FROXEL_DISPATCH_Y, local_size_z = FROXEL_DISPATCH_Z) in;
void main() {

    ivec3 froxel_coord = ivec3(gl_GlobalInvocationID.xyz);

    // Check coordinates boundaries
    vec3 world_position = world_from_froxel_no_jitter(froxel_coord);

    float z0 = slice_to_exponential_depth(vfog.froxel_near, vfog.froxel_far, froxel_coord.z, int(vfog.froxel_dimensions.z));
    float z1 = slice_to_exponential_depth(vfog.froxel_near, vfog.froxel_far, froxel_coord.z + 1, int(vfog.froxel_dimensions.z));
    float slice_thickness = abs(z1 - z0);

    // Stable noise per froxel
    float j = ( vfog.light_scattering_jitter_animated == 1 ) ? z_jitter_half_range(froxel_coord, int(frame.current_frame), frame.blue_noise_128_rg_texture_index)
                                                        : z_jitter_half_range_static(froxel_coord, frame.blue_noise_128_rg_texture_index);

    // Ray direction from camera
    vec3 ray_dir = normalize(world_position - frame.camera_position.xyz);

    // Offset position within slice
    float jitter_world = j * ((slice_thickness) * vfog.light_scattering_jitter_scale);
    world_position += ray_dir * jitter_world;

    vec3 rcp_froxel_dim = 1.0f / vfog.froxel_dimensions.xyz;
    vec3 fog_data_uvw = (vec3(froxel_coord) + vec3(0.5)) * rcp_froxel_dim;
    vec4 scattering_extinction = textureLod(global_textures_3d[nonuniformEXT(vfog.froxel_data_texture_index)], fog_data_uvw, 0.0);

    float extinction = scattering_extinction.a;
    vec3 lighting = vec3(0);

    if ( extinction >= 0.01f ) {
        vec3 V = normalize(frame.camera_position.xyz - world_position);

        // Read clustered lighting data
        // Calculate linear depth.
        float linear_d = slice_to_exponential_depth(vfog.froxel_near, vfog.froxel_far, froxel_coord.z, int(vfog.froxel_dimensions.z));
        linear_d /= vfog.froxel_far;

        // Select bin
        int bin_index = int( linear_d / BIN_WIDTH );
        uint bin_value = bins[ bin_index ];

        uint min_light_id = bin_value & 0xFFFF;
        uint max_light_id = ( bin_value >> 16 ) & 0xFFFF;

        float fx = (float(froxel_coord.x) + 0.5) / float(vfog.froxel_dimensions.x);
        float fy = (float(froxel_coord.y) + 0.5) / float(vfog.froxel_dimensions.y);

        uint px = uint(clamp(fx * frame.resolution.x, 0.0, float(frame.resolution.x - 1)));
        uint py = uint(clamp(fy * frame.resolution.y, 0.0, float(frame.resolution.y - 1)));
        py = (uint(frame.resolution.y) - 1u) - py;

        uvec2 position = uvec2(px, py);
        uint address = get_tile_address( position );

        float shadow_noise = generate_noise(froxel_coord.xy * 1.0f, int(frame.current_frame), vfog.lighting_noise_scale);

        if ( min_light_id != NUM_LIGHTS + 1 ) {
            for ( uint light_id = min_light_id; light_id <= max_light_id; ++light_id ) {
                uint word_id = light_id / 32;
                uint bit_id = light_id % 32;

                if ( ( tiles[ address + word_id ] & ( 1 << bit_id ) ) != 0 ) {
                    uint global_light_index = light_indices[ light_id ];
                    Light point_light = lights[ global_light_index ];

                    //final_color.rgb += calculate_point_light_contribution( albedo, orm, normal, emissive, world_position, V, F0, NoV, point_light, global_light_index );

                    // TODO: properly use light clustering.
                    vec3 light_position = point_light.world_position;
                    float light_radius = point_light.radius;
                    if (length(world_position - light_position) < light_radius) {

                        uint shadow_light_index = global_light_index;
                        vec3 v = world_position - light_position;

                        float current_local_z = max(abs(v.x), max(abs(v.y), abs(v.z)));
                        float pcf_radius = current_local_z / light_radius;
                        pcf_radius *= 0.1;

                        vec3 dir = normalize(v);

                        // Build a stable basis around dir:
                        vec3 up = (abs(dir.y) < 0.99) ? vec3(0, 1, 0) : vec3(1, 0, 0);
                        vec3 t  = normalize(cross(up, dir));
                        vec3 b  = cross(dir, t);

                        const uint samples = 2;
                        float shadow = 0;
                        for(uint i = 0; i < samples; ++i) {
                            vec2 disk_offset = vogel_disk_offset(i, samples, 0.1f);
                            vec3 sampling_position = v + (t * disk_offset.x + b * disk_offset.y) * pcf_radius;

                            float closest_depth = texture( global_textures_cubemaps_array[nonuniformEXT(cubemap_shadows_index)], vec4(sampling_position, shadow_light_index) ).r;
                            float closest_local_z = decode_local_z( closest_depth, light_radius, point_light.rcp_n_minus_f );

                            // bias in local_z space
                            float bias = 0.001 * (current_local_z / light_radius);
                            //bias += pcf_radius * 0.25;
                            bias = clamp(bias, 0.0005, 0.05);

                            shadow += (current_local_z - bias < closest_local_z) ? 1.0 : 0.0;
                        }
                        shadow /= samples;

                        const vec3 L = normalize(light_position - world_position);
                        float attenuation = attenuation_square_falloff(L, 1.0f / light_radius) * shadow;

                        lighting += point_light.color * point_light.intensity * phase_function(V, -L, vfog.phase_anisotropy_01) * attenuation;
                    }
                }
            }
        }
    }

    // Add lighting dithering
    lighting = max(lighting, vec3(0));

    //lighting = vec3(1,1,1);

    vec3 scattering = scattering_extinction.rgb * lighting;

    imageStore( global_images_3d[vfog.raw_light_scattering_texture_index], ivec3(froxel_coord.xyz), vec4(scattering, extinction) );
}

#endif


#if defined(COMPUTE_LIGHT_INTEGRATION)

// Dispatch with Z = 1 as we perform the integration.
layout (local_size_x = FROXEL_DISPATCH_X, local_size_y = FROXEL_DISPATCH_Y, local_size_z = 1) in;
void main() {

    ivec3 froxel_coord = ivec3(gl_GlobalInvocationID.xyz);

    vec3 integrated_scattering = vec3(0,0,0);
    float integrated_transmittance = 1.0f;

    float current_z = slice_to_exponential_depth( vfog.froxel_near, vfog.froxel_far, 0, int(vfog.froxel_dimensions.z) );

    vec3 rcp_froxel_dim = 1.0f / vfog.froxel_dimensions.xyz;
    vec2 uv_xy = (vec2(froxel_coord.xy) + vec2(0.5)) * rcp_froxel_dim.xy;

    for ( int z = 0; z < vfog.froxel_dimensions.z; ++z ) {

        froxel_coord.z = z;

        float next_z = slice_to_exponential_depth( vfog.froxel_near, vfog.froxel_far, z + 1, int(vfog.froxel_dimensions.z) );

        const float z_step = abs(next_z - current_z);
        current_z = next_z;

        // Jittering on z /////////////////////////////////////////////////////////
        const float jitter_amp = 0.25f;
        float j = z_jitter_half_range( froxel_coord, uint(frame.current_frame), frame.blue_noise_128_rg_texture_index ); // [-0.5, +0.5]

        // sample at center (with jitter) in slice index space:
        float zf = (float(z) + 0.5f + j * jitter_amp) * rcp_froxel_dim.z;
        zf = clamp(zf, 0.5f * rcp_froxel_dim.z, 1.0f - 0.5f * rcp_froxel_dim.z);

        const float dz = rcp_froxel_dim.z;

        vec4 a = textureLod(global_textures_3d[nonuniformEXT(vfog.froxel_data_texture_index)], vec3(uv_xy, zf - dz), 0.0);
        vec4 b = textureLod(global_textures_3d[nonuniformEXT(vfog.froxel_data_texture_index)], vec3(uv_xy, zf),      0.0);
        vec4 c = textureLod(global_textures_3d[nonuniformEXT(vfog.froxel_data_texture_index)], vec3(uv_xy, zf + dz), 0.0);

        vec4 sampled_scattering_extinction = (a + 2.0f * b + c) * 0.25f;
        //sampled_scattering_extinction = b;

        const vec3 sampled_scattering = sampled_scattering_extinction.xyz;
        const float sampled_extinction =  max(sampled_scattering_extinction.w,  0.00001f);

        const float transmittance = exp(-sampled_extinction * z_step);

        const vec3 scattering = (sampled_scattering - (sampled_scattering * transmittance)) / sampled_extinction;

        integrated_scattering += scattering * integrated_transmittance;
        integrated_transmittance *= transmittance;

        vec3 stored_scattering = integrated_scattering;

        if ( enable_volumetric_fog_opacity_anti_aliasing() ) {
            const float opacity = max( 1 - integrated_transmittance, 0.00000001f );
            stored_scattering = integrated_scattering / opacity;
        }

        imageStore( global_images_3d[vfog.raw_light_scattering_texture_index], froxel_coord.xyz, vec4(stored_scattering, integrated_transmittance) );
    }
}

#endif

#if defined(COMPUTE_SPATIAL_FILTERING)

#define SIGMA_FILTER 4.0
#define RADIUS 2

float gaussian(float radius, float sigma) {
    const float v = radius / sigma;
    return exp(-(v*v));
}

layout (local_size_x = FROXEL_DISPATCH_X, local_size_y = FROXEL_DISPATCH_Y, local_size_z = FROXEL_DISPATCH_Z) in;
void main() {

    ivec3 froxel_coord = ivec3(gl_GlobalInvocationID.xyz);
    vec3 rcp_froxel_dim = 1.0f / vfog.froxel_dimensions.xyz;

    vec3 uvw = (vec3(froxel_coord) + vec3(0.5)) * rcp_froxel_dim;
    vec4 scattering_extinction = textureLod(global_textures_3d[nonuniformEXT(vfog.raw_light_scattering_texture_index)], uvw, 0);
    if ( vfog.use_spatial_filtering == 1 ) {
        
        float accumulated_weight = 0;
        vec4 accumulated_scattering_extinction = vec4(0);

        for (int i = -RADIUS; i <= RADIUS; ++i ) {
            for (int j = -RADIUS; j <= RADIUS; ++j ) {
                ivec3 coord = froxel_coord + ivec3(i, j, 0);
                // if inside
                if (all(greaterThanEqual(coord, ivec3(0))) && all(lessThan(coord, ivec3(vfog.froxel_dimensions.xyz)))) {
                    const float weight = gaussian(length(ivec2(i, j)), SIGMA_FILTER);
                    vec3 uvw_n = (vec3(coord) + vec3(0.5)) * rcp_froxel_dim;
                    const vec4 sampled_value = texture(global_textures_3d[nonuniformEXT(vfog.raw_light_scattering_texture_index)], uvw_n);
                    accumulated_scattering_extinction.rgba += sampled_value.rgba * weight;
                    accumulated_weight += weight;
                }
            }
        }

        scattering_extinction = accumulated_scattering_extinction / accumulated_weight;
    }

    imageStore( global_images_3d[vfog.froxel_data_texture_index], froxel_coord.xyz, scattering_extinction );
}

#endif

#if defined(COMPUTE_TEMPORAL_FILTERING)


layout (local_size_x = FROXEL_DISPATCH_X, local_size_y = FROXEL_DISPATCH_Y, local_size_z = FROXEL_DISPATCH_Z) in;
void main() {

    ivec3 froxel_coord = ivec3(gl_GlobalInvocationID.xyz);
    vec3 rcp_froxel_dim = 1.0f / vfog.froxel_dimensions.xyz;


    vec3 uvw = (vec3(froxel_coord) + vec3(0.5)) * rcp_froxel_dim;
    vec4 scattering_extinction = texture(global_textures_3d[nonuniformEXT(vfog.raw_light_scattering_texture_index)], uvw);
    // DEBUG: red means history is empty
    // if (froxel_coord.z == vfog.froxel_dimensions.z / 2) {
    //     vec4 history = textureLod(global_textures_3d[nonuniformEXT(vfog.temporal_previous_texture_index)], uvw, 0.0);
    //     imageStore(global_images_3d[vfog.temporal_current_texture_index], froxel_coord, vec4(history.a, 0, 0, 1));
    //     return;
    // }

    // Temporal reprojection
    if (vfog.use_temporal_reprojection == 1) {

        vec3 world_position_no_jitter = world_from_froxel_no_jitter(froxel_coord);
        vec4 sceen_space_center_last = frame.previous_view_projection * vec4(world_position_no_jitter, 1.0);
        vec3 ndc = sceen_space_center_last.xyz / sceen_space_center_last.w;

        float linear_depth = raw_depth_to_linear_depth( ndc.z, vfog.froxel_near, vfog.froxel_far );
        // Exponential
        float depth_uv = linear_depth_to_slice_f( vfog.froxel_near, vfog.froxel_far, linear_depth, int(vfog.froxel_dimensions.z) );
        vec3 history_uv = vec3( ndc.x * .5 + .5, ndc.y * -.5 + .5, depth_uv );

        // If history UV is outside the frustum, skip
        if (all(greaterThanEqual(history_uv, vec3(0.0f))) && all(lessThanEqual(history_uv, vec3(1.0f)))) {
            // Fetch history sample
            vec4 history = textureLod(global_textures_3d[nonuniformEXT(vfog.temporal_previous_texture_index)], history_uv, 0.0f);

            // Get mismatch metrics
            float diff_scattering = length(history.rgb - scattering_extinction.rgb);
            float diff_exctinction = abs(history.a - scattering_extinction.a);

            // Clamp history around current values
            vec3 eps = max(vec3(0.02), scattering_extinction.rgb * 0.2);
            history.rgb = clamp(history.rgb, scattering_extinction.rgb - eps,
                                             scattering_extinction.rgb + eps);

            float eps_t = max(0.01, scattering_extinction.a * 0.05);
            history.a = clamp(history.a, scattering_extinction.a - eps_t, scattering_extinction.a + eps_t);

            // Adaptive weight based on mismatch
            // Reject factor (0 = trust history, 1 = reject history)
            float reject = saturate(max(diff_scattering * 2.0, diff_exctinction * 8.0));

            float history_weight = vfog.temporal_reprojection_percentage;
            history_weight *= (1.0 - reject);

            // Final blend
            scattering_extinction = mix(scattering_extinction, history, history_weight);
        }
    }

    imageStore( global_images_3d[vfog.temporal_current_texture_index], froxel_coord.xyz, scattering_extinction );
}

#endif

#if defined(COMPUTE_VOLUMETRIC_NOISE_BAKING)

layout( push_constant ) uniform PushConstants {
    uint            output_texture_index;
};

vec3 interpolation_c2( vec3 x ) { return x * x * x * (x * (x * 6.0 - 15.0) + 10.0); }

// from: https://github.com/BrianSharpe/GPU-Noise-Lib/blob/master/gpu_noise_lib.glsl
void perlin_hash(vec3 gridcell, float s, bool tile, 
                    out vec4 lowz_hash_0,
                    out vec4 lowz_hash_1,
                    out vec4 lowz_hash_2,
                    out vec4 highz_hash_0,
                    out vec4 highz_hash_1,
                    out vec4 highz_hash_2)
{
    const vec2 OFFSET = vec2( 50.0, 161.0 );
    const float DOMAIN = 69.0;
    const vec3 SOMELARGEFLOATS = vec3( 635.298681, 682.357502, 668.926525 );
    const vec3 ZINC = vec3( 48.500388, 65.294118, 63.934599 );

    gridcell.xyz = gridcell.xyz - floor(gridcell.xyz * ( 1.0 / DOMAIN )) * DOMAIN;
    float d = DOMAIN - 1.5;
    vec3 gridcell_inc1 = step( gridcell, vec3( d,d,d ) ) * ( gridcell + 1.0 );

    gridcell_inc1 = tile ? mod(gridcell_inc1, s) : gridcell_inc1;

    vec4 P = vec4( gridcell.xy, gridcell_inc1.xy ) + OFFSET.xyxy;
    P *= P;
    P = P.xzxz * P.yyww;
    vec3 lowz_mod = vec3( 1.0 / ( SOMELARGEFLOATS.xyz + gridcell.zzz * ZINC.xyz ) );
    vec3 highz_mod = vec3( 1.0 / ( SOMELARGEFLOATS.xyz + gridcell_inc1.zzz * ZINC.xyz ) );
    lowz_hash_0 = fract( P * lowz_mod.xxxx );
    highz_hash_0 = fract( P * highz_mod.xxxx );
    lowz_hash_1 = fract( P * lowz_mod.yyyy );
    highz_hash_1 = fract( P * highz_mod.yyyy );
    lowz_hash_2 = fract( P * lowz_mod.zzzz );
    highz_hash_2 = fract( P * highz_mod.zzzz );
}

// from: https://github.com/BrianSharpe/GPU-Noise-Lib/blob/master/gpu_noise_lib.glsl
float perlin(vec3 P, float s, bool tile) {
    P *= s;

    vec3 Pi = floor(P);
    vec3 Pi2 = floor(P);
    vec3 Pf = P - Pi;
    vec3 Pf_min1 = Pf - 1.0;

    vec4 hashx0, hashy0, hashz0, hashx1, hashy1, hashz1;
    perlin_hash( Pi2, s, tile, hashx0, hashy0, hashz0, hashx1, hashy1, hashz1 );

    vec4 grad_x0 = hashx0 - 0.49999;
    vec4 grad_y0 = hashy0 - 0.49999;
    vec4 grad_z0 = hashz0 - 0.49999;
    vec4 grad_x1 = hashx1 - 0.49999;
    vec4 grad_y1 = hashy1 - 0.49999;
    vec4 grad_z1 = hashz1 - 0.49999;
    vec4 grad_results_0 = 1.0 / sqrt( grad_x0 * grad_x0 + grad_y0 * grad_y0 + grad_z0 * grad_z0 ) * ( vec2( Pf.x, Pf_min1.x ).xyxy * grad_x0 + vec2( Pf.y, Pf_min1.y ).xxyy * grad_y0 + Pf.zzzz * grad_z0 );
    vec4 grad_results_1 = 1.0 / sqrt( grad_x1 * grad_x1 + grad_y1 * grad_y1 + grad_z1 * grad_z1 ) * ( vec2( Pf.x, Pf_min1.x ).xyxy * grad_x1 + vec2( Pf.y, Pf_min1.y ).xxyy * grad_y1 + Pf_min1.zzzz * grad_z1 );

    vec3 blend = interpolation_c2( Pf );
    vec4 res0 = mix( grad_results_0, grad_results_1, blend.z );
    vec4 blend2 = vec4( blend.xy, vec2( 1.0 - blend.xy ) );
    float final = dot( res0, blend2.zxzx * blend2.wwyy );
    final *= 1.0/sqrt(0.75);
    return ((final * 1.5) + 1.0) * 0.5;
}

float perlin(vec3 P) {
    return perlin(P, 1, false);
}

float get_perlin_7_octaves(vec3 p, float s) {
    vec3 xyz = p;
    float f = 1.0;
    float a = 1.0;

    float perlin_value = 0.0;
    perlin_value += a * perlin(xyz, s * f, true).r; a *= 0.5; f *= 2.0;
    perlin_value += a * perlin(xyz, s * f, true).r; a *= 0.5; f *= 2.0;
    perlin_value += a * perlin(xyz, s * f, true).r; a *= 0.5; f *= 2.0;
    perlin_value += a * perlin(xyz, s * f, true).r; a *= 0.5; f *= 2.0;
    perlin_value += a * perlin(xyz, s * f, true).r; a *= 0.5; f *= 2.0;
    perlin_value += a * perlin(xyz, s * f, true).r; a *= 0.5; f *= 2.0;
    perlin_value += a * perlin(xyz, s * f, true).r;

    return perlin_value;
}

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
void main() {
    ivec3 pos = ivec3(gl_GlobalInvocationID.xyz);

    vec3 xyz = pos / 64.0;

    float perlin_data = get_perlin_7_octaves(xyz, 8.0);

    imageStore( global_images_3d[output_texture_index], pos, vec4(perlin_data, 0, 0, 0) );
}

#endif