#if !defined(SHARED_STRUCTS_H)
#define SHARED_STRUCTS_H

#if defined (__cplusplus)

#define uint uint32_t
#define int int32_t

#define float4x4 glm::mat4

#define int3 glm::ivec3

#define float2 glm::vec2
#define float3 glm::vec3
#define float4 glm::vec4

#elif defined(__SLANG__)

//#define uint uint32_t
//#define int int32_t

//#define mat4 float4x4

//#define float2 float2
//#define float3 float3
//#define float4 float4

#else // GLSL

//#define uint uint
//#define int int

#define float4x4 mat4

#define int3 ivec3

#define float2 vec2
#define float3 vec3
#define float4 vec4

#endif // __cplusplus

struct GpuFrameConstants {
    float4x4    view_projection;
    float4x4    view_projection_debug;
    float4x4    inverse_view_projection;
    float4x4    world_to_camera;
    float4x4    world_to_camera_debug;
    float4x4    previous_view_projection;
    float4x4    inverse_projection;
    float4x4    inverse_view;

    float4      camera_position;
    float4      camera_position_debug;

    float3      camera_direction;
    int         current_frame;

    uint        active_lights;
    uint        use_tetrahedron_shadows;
    uint        dither_texture_index;
    float       z_near;

    float       z_far;
    float       projection_00;
    float       projection_11;
    uint        culling_options;

    float2      resolution;
    float       aspect_ratio;
    uint        num_mesh_instances;

    float2      halton_xy;
    uint        depth_texture_index;
    uint        blue_noise_128_rg_texture_index;

    float2      jitter_xy;
    float2      previous_jitter_xy;

    float       forced_metalness;
    float       forced_roughness;
    float       volumetric_fog_application_dithering_scale;
    uint        volumetric_fog_application_options;

    float4      frustum_planes[6];
}; // struct GpuFrameConstants

struct GpuDDGIConstants {
    uint        radiance_output_index;
    uint        grid_irradiance_output_index;
    uint        indirect_output_index;
    uint        normal_texture_index;

    uint        depth_pyramid_texture_index;
    uint        depth_fullscreen_texture_index;
    uint        grid_visibility_texture_index;
    uint        probe_offset_texture_index;

    float       hysteresis;
    float       infinite_bounces_multiplier;
    int         probe_update_offset;
    int         probe_update_count;

    float3      probe_grid_position;
    float       probe_sphere_scale;

    float3      probe_spacing;
    float       max_probe_offset;   // [0,0.5] max offset for probes

    float3      reciprocal_probe_spacing;
    float       self_shadow_bias;

    int3        probe_counts;
    uint        debug_options;

    int         irradiance_texture_width;
    int         irradiance_texture_height;
    int         irradiance_side_length;
    int         probe_rays;

    int         visibility_texture_width;
    int         visibility_texture_height;
    int         visibility_side_length;
    int         pad003_ddgic;

    float4x4    random_rotation;
};

struct GpuPostProcessConstants {

    uint        tonemap_type;
    float       exposure;
    float       sharpening_amount;
    uint        input_texture_index;

    float2      mouse_uv;
    float       zoom_scale;
    uint        enable_zoom;

    uint        resolution_x;
    uint        resolution_y;

    uint        bloom_texture_index;
    float       bloom_amount;
}; // struct GpuPostProcessConstants

struct GpuVolumetricFogConstants {

    float4x4    froxel_inverse_view_projection;

    float       froxel_near;
    float       froxel_far;
    float       scattering_factor;
    float       density_modifier;

    uint        temporal_current_texture_index;
    uint        raw_light_scattering_texture_index;
    uint        froxel_data_texture_index;
    uint        temporal_previous_texture_index;

    uint        use_temporal_reprojection;
    float       time_random_01;
    float       temporal_reprojection_percentage;
    float       phase_anisotropy_01;

    int3        froxel_dimensions;
    uint        phase_function_type;

    float       height_fog_density;
    float       height_fog_falloff;
    float       light_scattering_jitter_scale;
    float       noise_scale;

    float       lighting_noise_scale;
    uint        noise_type;
    uint        light_scattering_jitter_animated;
    uint        use_spatial_filtering;

    uint        volumetric_noise_texture_index;
    float       volumetric_noise_position_multiplier;
    float       volumetric_noise_speed_multiplier;
    float       temporal_reprojection_jitter_scale;

    float3      box_position;
    float       box_fog_density;

    float3      box_half_size;
    uint        box_color;
}; // struct GpuVolumetricFogConstants


struct GpuTaaConstants {

    uint        history_color_texture_index;
    uint        taa_output_texture_index;
    uint        velocity_texture_index;
    uint        current_color_texture_index;

    uint        taa_modes;
    uint        options;
    uint        current_depth_texture_index;
    uint        previous_depth_texture_index;

    uint        velocity_sampling_mode;
    uint        history_sampling_filter;
    uint        history_constraint_mode;
    uint        current_color_filter;

    float2      render_resolution;
    float2      swapchain_resolution;

    float2      jitter;
    float       scale_factor; // swapchain res / render_res
    float       current_sample_sharpness;

}; // struct GpuTaaConstants


struct GpuShadowVisibilityConstants {

    uint previous_visibility_cache_texture_index;
    uint current_visibility_cache_texture_index;
    uint previous_variation_cache_texture_index;
    uint current_variation_cache_texture_index;

    uint previous_samples_count_cache_texture_index;
    uint current_samples_count_cache_texture_index;
    uint variation_texture_index;
    uint motion_vectors_texture_index;

    uint normals_texture_index;
    uint filtered_visibility_texture;
    uint filtered_variation_texture;
    uint frame_index; // NOTE(marco): [0-3]

    float resolution_scale;
    float resolution_scale_rcp;
    uint previous_depth_texture_index;
    uint current_depth_texture_index;

    uint upscaled_visibility_texture;
    uint disable_history;
    uint max_samples;
    uint disable_spatial;

}; // struct GpuShadowVisibilityConstants


#endif // SHARED_STRUCTS_H
