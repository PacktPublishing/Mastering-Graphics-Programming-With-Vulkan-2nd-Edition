
#define FROXEL_DISPATCH_X 8
#define FROXEL_DISPATCH_Y 8
#define FROXEL_DISPATCH_Z 1


#include "platform.slangh"
#include "frame.slangh"


// Noise helper functions ////////////////////////////////////////////////
float generate_noise(float2 pixel, int frame_index, float scale) {
    // Animated blue noise using golden ratio.
    if (fog.noise_type == 0) {
        float2 blue_noise_value = animated_blue_noise_golden_ratio(uint2(pixel), uint(frame_index), frame.blue_noise_128_rg_texture_index);

        return triangular_noise(blue_noise_value.x, blue_noise_value.y) * scale;
    }
    // Interleaved gradient noise
    if (fog.noise_type == 1) {
        float noise0 = interleaved_gradient_noise(pixel, frame_index);
        float noise1 = interleaved_gradient_noise(pixel, frame_index + 1);

        return triangular_noise(noise0, noise1) * scale;
    }

    // Initial noise attempt, left for reference.
    return (interleaved_gradient_noise(pixel, frame_index) * scale) - (scale * 0.5f);
}



// Coordinate transformations ////////////////////////////////////////////
float2 uv_from_froxels( float2 froxel_position, uint width, uint height ) {
    float2 uv = floor(froxel_position) + .5;
    return uv / float2(width * 1.f, height * 1.f);
}

float3 world_from_froxel(int3 froxel_coord) {

    float2 uv = uv_from_froxels( froxel_coord.xy + float2(0.5) + frame.halton_xy * fog.temporal_reprojection_jitter_scale, fog.froxel_dimensions.x, fog.froxel_dimensions.y );

    // First calculate linear depth
    float depth = froxel_coord.z * 1.0f / float(fog.froxel_dimensions.z);
    //float linear_depth = fog.froxel_near + depth * (fog.froxel_far - fog.froxel_near);
    // Exponential depth
    float jittering = 0;//generate_noise(froxel_coord.xy * 1.0f, frame.current_frame, fog.noise_scale);
    float linear_depth = slice_to_exponential_depth_jittered( fog.froxel_near, fog.froxel_far, jittering, froxel_coord.z, int(fog.froxel_dimensions.z) );

    // Then calculate raw depth, necessary to properly calculate world position.
    float raw_depth = linear_depth_to_raw_depth(linear_depth, fog.froxel_near, fog.froxel_far);
    return world_position_from_depth( uv, raw_depth, fog.froxel_inverse_view_projection );
}

float3 world_from_froxel_no_jitter(int3 froxel_coord) {

    float2 uv = uv_from_froxels( froxel_coord.xy + float2(0.5), fog.froxel_dimensions.x, fog.froxel_dimensions.y );

    // First calculate linear depth
    float depth = froxel_coord.z * 1.0f / float(fog.froxel_dimensions.z);
    //float linear_depth = fog.froxel_near + depth * (fog.froxel_far - fog.froxel_near);
    // Exponential depth
    float linear_depth = slice_to_exponential_depth( fog.froxel_near, fog.froxel_far, froxel_coord.z, int(fog.froxel_dimensions.z) );
    // Then calculate raw depth, necessary to properly calculate world position.
    float raw_depth = linear_depth_to_raw_depth(linear_depth, fog.froxel_near, fog.froxel_far);
    return world_position_from_depth( uv, raw_depth, fog.froxel_inverse_view_projection );
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
float phase_function(float3 V, float3 L, float g) {
    float cos_theta = dot(V, L);

    if (fog.phase_function_type == 0) {
        return henyey_greenstein(g, cos_theta);
    }
    if (fog.phase_function_type == 1) {
        return cornette_shanks(g, cos_theta);
    }
    if (fog.phase_function_type == 2) {
        return shlick(g, cos_theta);
    }

    return cornette_shanks_approximated(g, cos_theta);
}


// General utility functions /////////////////////////////////////////////
float remap( float value, float oldMin, float oldMax, float newMin,float newMax) {
    return newMin + (value - oldMin) / (oldMax - oldMin) * (newMax - newMin);
}

float4 scattering_extinction_from_color_density( float3 color, float density ) {

    const float extinction = fog.scattering_factor * density;
    return float4( color * extinction, extinction );
}


ConstantBuffer<GpuVolumetricFogConstants> fog : register(b40, space1);


#if defined(COMPUTE_INJECT_DATA_SLANG)

[shader("compute")]
[numthreads(FROXEL_DISPATCH_X, FROXEL_DISPATCH_Y, FROXEL_DISPATCH_Z)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
    //int3 froxel_coord = int3(gl_GlobalInvocationID.xyz);
    int3 froxel_coord = int3(dispatchThreadID);

    float3 world_position = world_from_froxel(froxel_coord);

    float4 scattering_extinction = float4(0);

    float3 sampling_coord = world_position * fog.volumetric_noise_position_multiplier + float3(1,0.1,2) * frame.current_frame * fog.volumetric_noise_speed_multiplier;

    //float fog_noise = texture(global_textures_3d[nonuniformEXT(volumetric_noise_texture_index)], sampling_coord).r;
    float fog_noise = texture_3d(fog.volumetric_noise_texture_index).SampleLevel(g_linear_repeat(), sampling_coord, 0).r;
    fog_noise = saturate(fog_noise * fog_noise);

    // Add constant fog
    float fog_density = fog.density_modifier * fog_noise;
    scattering_extinction += scattering_extinction_from_color_density( float3(0.5), fog_density );
    // Add height fog
    float height_fog = fog.height_fog_density * exp(-fog.height_fog_falloff * max(world_position.y, 0)) * fog_noise;
    scattering_extinction += scattering_extinction_from_color_density( float3(0.5), height_fog );

    // Add density from box
    float3 box = abs(world_position - fog.box_position);
    if (all(box <= fog.box_half_size)) {

        float4 box_fog_color = unpack_color_rgba( fog.box_color );

        scattering_extinction += scattering_extinction_from_color_density( box_fog_color.rgb, fog.box_fog_density * fog_noise);
    }

    //imageStore( global_images_3d[froxel_data_texture_index], froxel_coord.xyz, scattering_extinction );
    global_images_3d[fog.froxel_data_texture_index][froxel_coord.xyz] = scattering_extinction;
}

#endif // COMPUTE_INJECT_DATA_SLANG


#if defined(COMPUTE_LIGHT_SCATTERING_SLANG)

#include "lighting.slangh"

[shader("compute")]
[numthreads(FROXEL_DISPATCH_X, FROXEL_DISPATCH_Y, FROXEL_DISPATCH_Z)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
    int3 froxel_coord = int3(dispatchThreadID);

    // Check coordinates boundaries
    float3 world_position = world_from_froxel(froxel_coord);

    float3 rcp_froxel_dim = 1.0f / fog.froxel_dimensions.xyz;
    float3 fog_data_uvw = froxel_coord * rcp_froxel_dim;

    //float4 scattering_extinction = texture(global_textures_3d[nonuniformEXT(froxel_data_texture_index)], fog_data_uvw);
    float4 scattering_extinction = texture_3d(fog.froxel_data_texture_index).SampleLevel(g_linear_clamp(), fog_data_uvw, 0);

    float extinction = scattering_extinction.a;
    float3 lighting = float3(0);

    if ( extinction >= 0.01f ) {
        float3 V = normalize(frame.camera_position.xyz - world_position);

        // Read clustered lighting data
        // Calculate linear depth.
        float linear_d = froxel_coord.z * rcp_froxel_dim.z;
        linear_d = raw_depth_to_linear_depth(linear_d, fog.froxel_near, fog.froxel_far) / fog.froxel_far;
        // Select bin
        int bin_index = int( linear_d / BIN_WIDTH );
        uint bin_value = z_bins[ bin_index ];

        uint min_light_id = bin_value & 0xFFFF;
        uint max_light_id = ( bin_value >> 16 ) & 0xFFFF;

        uint2 position = uint2(uint(froxel_coord.x * 1.0f / fog.froxel_dimensions.x * frame.resolution.x),
                               uint(froxel_coord.y * 1.0f / fog.froxel_dimensions.y * frame.resolution.y));
        // Select tile lighting base address
        uint address = get_tile_address( position );

        if ( min_light_id != NUM_LIGHTS + 1 ) {
            for ( uint light_id = min_light_id; light_id <= max_light_id; ++light_id ) {
                uint word_id = light_id / 32;
                uint bit_id = light_id % 32;

                if ( ( tiles[ address + word_id ] & ( 1 << bit_id ) ) != 0 ) {
                    uint global_light_index = light_indices[ light_id ];
                    GpuLight point_light = lights[ global_light_index ];

                    //final_color.rgb += calculate_point_light_contribution( albedo, orm, normal, emissive, world_position, V, F0, NoV, point_light, global_light_index );

                    // TODO: properly use light clustering.
                    float3 light_position = point_light.world_position;
                    float light_radius = point_light.radius;
                    if (length(world_position - light_position) < light_radius) {

                        // Calculate point light contribution
                        
                        // TODO: add shadows
                        float3 shadow_position_to_light = world_position - light_position;
                        const float current_depth = vector_to_depth_value(shadow_position_to_light, light_radius, point_light.rcp_n_minus_f);
                        const float bias = 0.0001f;
                        const uint shadow_light_index = global_light_index;

                        const uint samples = 4;
                        float shadow = 0;

                        for(uint i = 0; i < samples; ++i) {

                            float2 disk_offset = vogel_disk_offset(i, 4, 0.1f);
                            float3 sampling_position = shadow_position_to_light + disk_offset.xyx * 0.0005f;
                            //const float closest_depth = texture(global_textures_cubemaps_array[nonuniformEXT(cubemap_shadows_index)], float4(sampling_position, shadow_light_index)).r;
                            const float closest_depth = global_textures_cubemaps_array[NonUniformResourceIndex(light_consts.cubemap_shadows_index)].SampleLevel(g_linear_clamp(), float4(sampling_position, shadow_light_index), 0).r;
                            shadow += current_depth - bias < closest_depth ? 1 : 0;
                        }

                        shadow /= samples;
                        //const float closest_depth = texture(global_textures_cubemaps_array[nonuniformEXT(cubemap_shadows_index)], float4(shadow_position_to_light, shadow_light_index)).r;
                        //float shadow = current_depth - bias < closest_depth ? 1 : 0;

                        const float3 L = normalize(light_position - world_position);
                        float attenuation = attenuation_square_falloff(L, 1.0f / light_radius) * shadow;

                        lighting += point_light.color * point_light.intensity * phase_function(V, -L, fog.phase_anisotropy_01) * attenuation;
                    }
                }
            }
        }
    }

    // Add lighting dithering
    float lighting_noise = generate_noise(froxel_coord.xy * 1.0f, frame.current_frame, fog.lighting_noise_scale);
    lighting += float3(lighting_noise);

    float3 scattering = scattering_extinction.rgb * lighting;

    //imageStore( global_images_3d[light_scattering_texture_index], ivec3(froxel_coord.xyz), float4(scattering, extinction) );
    global_images_3d[fog.raw_light_scattering_texture_index][froxel_coord] = float4(scattering, extinction);
}

#endif // COMPUTE_LIGHT_SCATTERING_SLANG


#if defined(COMPUTE_LIGHT_INTEGRATION_SLANG)


[shader("compute")]
// Dispatch with Z = 1 as we perform the integration.
[numthreads(FROXEL_DISPATCH_X, FROXEL_DISPATCH_Y, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
    int3 froxel_coord = int3(dispatchThreadID);

    float3 integrated_scattering = float3(0,0,0);
    float integrated_transmittance = 1.0f;

    float current_z = 0;

    float3 rcp_froxel_dim = 1.0f / fog.froxel_dimensions.xyz;

    for ( int z = 0; z < fog.froxel_dimensions.z; ++z ) {

        froxel_coord.z = z;

        float next_z = slice_to_exponential_depth( fog.froxel_near, fog.froxel_far, z + 1, int(fog.froxel_dimensions.z) );

        const float z_step = abs(next_z - current_z);
        current_z = next_z;

        // Following equations from Physically Based Sky, Atmosphere and Cloud Rendering by Hillaire
        //const float4 sampled_scattering_extinction = texture(global_textures_3d[nonuniformEXT(light_scattering_texture_index)], froxel_coord * rcp_froxel_dim);
        const float4 sampled_scattering_extinction = texture_3d(fog.froxel_data_texture_index).SampleLevel(g_linear_clamp(), froxel_coord * rcp_froxel_dim, 0);
        const float3 sampled_scattering = sampled_scattering_extinction.xyz;
        const float sampled_extinction = sampled_scattering_extinction.w;
        const float clamped_extinction = max(sampled_extinction, 0.00001f);

        const float transmittance = exp(-sampled_extinction * z_step);

        const float3 scattering = (sampled_scattering - (sampled_scattering * transmittance)) / clamped_extinction;

        integrated_scattering += scattering * integrated_transmittance;
        integrated_transmittance *= transmittance;

        float3 stored_scattering = integrated_scattering;

        if ( enable_volumetric_fog_opacity_anti_aliasing() ) {
            const float opacity = max( 1 - integrated_transmittance, 0.00000001f );
            stored_scattering = integrated_scattering / opacity;
        }

        //imageStore( global_images_3d[integrated_light_scattering_texture_index], froxel_coord.xyz, float4(stored_scattering, integrated_transmittance) );
        global_images_3d[fog.raw_light_scattering_texture_index][froxel_coord] = float4(stored_scattering, integrated_transmittance);
    }
}

#endif // COMPUTE_LIGHT_INTEGRATION_SLANG



#if defined(COMPUTE_SPATIAL_FILTERING_SLANG)

#define SIGMA_FILTER 4.0
#define RADIUS 2

float gaussian(float radius, float sigma) {
    const float v = radius / sigma;
    return exp(-(v*v));
}

[shader("compute")]
[numthreads(FROXEL_DISPATCH_X, FROXEL_DISPATCH_Y, FROXEL_DISPATCH_Z)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
    int3 froxel_coord = int3(dispatchThreadID);

    float3 rcp_froxel_dim = 1.0f / fog.froxel_dimensions.xyz;

    //float4 scattering_extinction = texture(global_textures_3d[nonuniformEXT(light_scattering_texture_index)], froxel_coord * rcp_froxel_dim);
    float4 scattering_extinction = texture_3d(fog.raw_light_scattering_texture_index).SampleLevel(g_linear_clamp(), froxel_coord * rcp_froxel_dim, 0);
    
    if ( fog.use_spatial_filtering == 1 ) {
        
        float accumulated_weight = 0;
        float4 accumulated_scattering_extinction = float4(0);

        float dithering = (frame.current_frame % 64) * 0.000001f;

        for (int i = -RADIUS; i <= RADIUS; ++i ) {
            for (int j = -RADIUS; j <= RADIUS; ++j ) {
                int3 coord = froxel_coord + int3(i, j, 0);
                // if inside
                if (all(coord >= int3(0)) && all(coord <= int3(fog.froxel_dimensions.xyz))) {
                    const float weight = gaussian(length(float2(i, j)), SIGMA_FILTER);
                    
                    //const float4 sampled_value = texture(global_textures_3d[nonuniformEXT(light_scattering_texture_index)], coord * rcp_froxel_dim);
                    const float4 sampled_value = texture_3d(fog.raw_light_scattering_texture_index).SampleLevel(g_linear_clamp(), coord * rcp_froxel_dim + float3(0,dithering,0), 0);
                    
                    accumulated_scattering_extinction.rgba += sampled_value.rgba * weight;
                    accumulated_weight += weight;
                }
            }
        }

        scattering_extinction = accumulated_scattering_extinction / accumulated_weight;
    }

    //imageStore( global_images_3d[froxel_data_texture_index], froxel_coord.xyz, scattering_extinction );
    global_images_3d[fog.froxel_data_texture_index][froxel_coord] = scattering_extinction;
}

#endif // COMPUTE_SPATIAL_FILTERING_SLANG



#if defined(COMPUTE_TEMPORAL_FILTERING_SLANG)


[shader("compute")]
[numthreads(FROXEL_DISPATCH_X, FROXEL_DISPATCH_Y, FROXEL_DISPATCH_Z)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
    int3 froxel_coord = int3(dispatchThreadID);

    float3 rcp_froxel_dim = 1.0f / fog.froxel_dimensions.xyz;

    //float4 scattering_extinction = texture(global_textures_3d[nonuniformEXT(froxel_data_texture_index)], froxel_coord * rcp_froxel_dim);
    float4 scattering_extinction = texture_3d(fog.raw_light_scattering_texture_index).SampleLevel(g_linear_clamp(), froxel_coord * rcp_froxel_dim, 0);

    // Temporal reprojection
    if (fog.use_temporal_reprojection == 1) {

        float3 world_position_no_jitter = world_from_froxel(froxel_coord);
        float4 sceen_space_center_last = mul(frame.previous_view_projection, float4(world_position_no_jitter, 1.0));
        float3 ndc = sceen_space_center_last.xyz / sceen_space_center_last.w;

        float linear_depth = raw_depth_to_linear_depth( ndc.z, fog.froxel_near, fog.froxel_far );
        // Exponential
        float depth_uv = linear_depth_to_uv( fog.froxel_near, fog.froxel_far, linear_depth, int(fog.froxel_dimensions.z) );
        float3 history_uv = float3( ndc.x * .5 + .5, ndc.y * -.5 + .5, depth_uv );

        // If history UV is outside the frustum, skip
        if (all(history_uv >= float3(0.0f)) && all(history_uv <= float3(1.0f))) {
            // Fetch history sample
            //float4 history = textureLod(global_textures_3d[nonuniformEXT(previous_light_scattering_texture_index)], history_uv, 0.0f);
            float4 history = texture_3d(fog.temporal_previous_texture_index).SampleLevel(g_linear_clamp(), history_uv, 0);

            history = max(history, scattering_extinction);

            scattering_extinction.rgb = lerp(history.rgb, scattering_extinction.rgb, fog.temporal_reprojection_percentage);
            scattering_extinction.a = lerp(history.a, scattering_extinction.a, fog.temporal_reprojection_percentage);

            // DEBUG: test where pixels are being sampled.
            //scattering = float3(1,0,0);
        }
    }

    //imageStore( global_images_3d[light_scattering_texture_index], froxel_coord.xyz, scattering_extinction );
    global_images_3d[fog.temporal_current_texture_index][froxel_coord] = scattering_extinction;
}

#endif // COMPUTE_TEMPORAL_FILTERING_SLANG


#if defined(COMPUTE_VOLUMETRIC_NOISE_BAKING_SLANG)

struct PushConstants {
    uint            output_texture_index;
};

[[vk::push_constant]]PushConstants pc;

float3 interpolation_c2( float3 x ) { return x * x * x * (x * (x * 6.0 - 15.0) + 10.0); }

// from: https://github.com/BrianSharpe/GPU-Noise-Lib/blob/master/gpu_noise_lib.glsl
void perlin_hash(float3 gridcell, float s, bool tile, 
                    out float4 lowz_hash_0,
                    out float4 lowz_hash_1,
                    out float4 lowz_hash_2,
                    out float4 highz_hash_0,
                    out float4 highz_hash_1,
                    out float4 highz_hash_2)
{
    const float2 OFFSET = float2( 50.0, 161.0 );
    const float DOMAIN = 69.0;
    const float3 SOMELARGEFLOATS = float3( 635.298681, 682.357502, 668.926525 );
    const float3 ZINC = float3( 48.500388, 65.294118, 63.934599 );

    gridcell.xyz = gridcell.xyz - floor(gridcell.xyz * ( 1.0 / DOMAIN )) * DOMAIN;
    float d = DOMAIN - 1.5;
    float3 gridcell_inc1 = step( gridcell, float3( d,d,d ) ) * ( gridcell + 1.0 );

    gridcell_inc1 = tile ? fmod(gridcell_inc1, s) : gridcell_inc1;

    float4 P = float4( gridcell.xy, gridcell_inc1.xy ) + OFFSET.xyxy;
    P *= P;
    P = P.xzxz * P.yyww;
    float3 lowz_mod = float3( 1.0 / ( SOMELARGEFLOATS.xyz + gridcell.zzz * ZINC.xyz ) );
    float3 highz_mod = float3( 1.0 / ( SOMELARGEFLOATS.xyz + gridcell_inc1.zzz * ZINC.xyz ) );
    lowz_hash_0 = fract( P * lowz_mod.xxxx );
    highz_hash_0 = fract( P * highz_mod.xxxx );
    lowz_hash_1 = fract( P * lowz_mod.yyyy );
    highz_hash_1 = fract( P * highz_mod.yyyy );
    lowz_hash_2 = fract( P * lowz_mod.zzzz );
    highz_hash_2 = fract( P * highz_mod.zzzz );
}

// from: https://github.com/BrianSharpe/GPU-Noise-Lib/blob/master/gpu_noise_lib.glsl
float perlin(float3 P, float s, bool tile) {
    P *= s;

    float3 Pi = floor(P);
    float3 Pi2 = floor(P);
    float3 Pf = P - Pi;
    float3 Pf_min1 = Pf - 1.0;

    float4 hashx0, hashy0, hashz0, hashx1, hashy1, hashz1;
    perlin_hash( Pi2, s, tile, hashx0, hashy0, hashz0, hashx1, hashy1, hashz1 );

    float4 grad_x0 = hashx0 - 0.49999;
    float4 grad_y0 = hashy0 - 0.49999;
    float4 grad_z0 = hashz0 - 0.49999;
    float4 grad_x1 = hashx1 - 0.49999;
    float4 grad_y1 = hashy1 - 0.49999;
    float4 grad_z1 = hashz1 - 0.49999;
    float4 grad_results_0 = 1.0 / sqrt( grad_x0 * grad_x0 + grad_y0 * grad_y0 + grad_z0 * grad_z0 ) * ( float2( Pf.x, Pf_min1.x ).xyxy * grad_x0 + float2( Pf.y, Pf_min1.y ).xxyy * grad_y0 + Pf.zzzz * grad_z0 );
    float4 grad_results_1 = 1.0 / sqrt( grad_x1 * grad_x1 + grad_y1 * grad_y1 + grad_z1 * grad_z1 ) * ( float2( Pf.x, Pf_min1.x ).xyxy * grad_x1 + float2( Pf.y, Pf_min1.y ).xxyy * grad_y1 + Pf_min1.zzzz * grad_z1 );

    float3 blend = interpolation_c2( Pf );
    float4 res0 = lerp( grad_results_0, grad_results_1, blend.z );
    float4 blend2 = float4( blend.xy, float2( 1.0 - blend.xy ) );
    float final = dot( res0, blend2.zxzx * blend2.wwyy );
    final *= 1.0/sqrt(0.75);
    return ((final * 1.5) + 1.0) * 0.5;
}

float perlin(float3 P) {
    return perlin(P, 1, false);
}

float get_perlin_7_octaves(float3 p, float s) {
    float3 xyz = p;
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

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
    int3 pos = int3(dispatchThreadID);

    float3 xyz = pos / 64.0;

    float perlin_data = get_perlin_7_octaves(xyz, 8.0);

    //imageStore( global_images_3d[output_texture_index], pos, float4(perlin_data, 0, 0, 0) );
    global_images_3d[pc.output_texture_index][pos] = float4(perlin_data, 0, 0, 0);
}

#endif // COMPUTE_VOLUMETRIC_NOISE_BAKING_SLANG
