#pragma once

#include "foundation/color.hpp"

#include "graphics/gpu_resources.hpp"

#include "external/glm/mat4x4.hpp"
#include "external/glm/vec2.hpp"
#include "external/glm/vec3.hpp"
#include "external/glm/vec4.hpp"

namespace raptor {

// Gpu Data //////////////////////////////////////////////////////////////
struct alignas( 16 ) GpuFrameData {
    glm::mat4               view_projection;
    glm::mat4               view_projection_debug;
    glm::mat4               inverse_view_projection;
    glm::mat4               world_to_camera;    // view matrix
    glm::mat4               world_to_camera_debug;
    glm::mat4               previous_view_projection;
    glm::mat4               inverse_projection;
    glm::mat4               inverse_view;

    glm::vec4               camera_position;
    glm::vec4               camera_position_debug;
    glm::vec3               camera_direction;
    i32                     current_frame;

    u32                     active_lights;
    u32                     use_tetrahedron_shadows;
    u32                     dither_image_view_index;
    f32                     z_near;

    f32                     z_far;
    f32                     projection_00;
    f32                     projection_11;
    u32                     culling_options;

    f32                     resolution_x;
    f32                     resolution_y;
    f32                     aspect_ratio;
    u32                     num_mesh_instances;

    f32                     halton_x;
    f32                     halton_y;
    u32                     depth_texture_index;
    u32                     blue_noise_128_rg_image_view_index;

    glm::vec2               jitter_xy;
    glm::vec2               previous_jitter_xy;

    f32                     forced_metalness;
    f32                     forced_roughness;
    f32                     volumetric_fog_application_dithering_scale;
    u32                     volumetric_fog_application_options;

    glm::vec4               frustum_planes[ 6 ];

    // Helpers for bit packing. Would be perfect for code generation
    // NOTE: must be in sync with scene.h!
    bool                    frustum_cull_meshes() const { return ( culling_options & 1 ) == 1; }
    bool                    frustum_cull_meshlets() const { return ( culling_options & 2 ) == 2; }
    bool                    occlusion_cull_meshes() const { return ( culling_options & 4 ) == 4; }
    bool                    occlusion_cull_meshlets() const { return ( culling_options & 8 ) == 8; }
    bool                    freeze_occlusion_camera() const { return ( culling_options & 16 ) == 16; }
    bool                    shadow_meshlets_cone_cull() const { return ( culling_options & 32 ) == 32; }
    bool                    shadow_meshlets_sphere_cull() const { return ( culling_options & 64 ) == 64; }
    bool                    shadow_meshlets_cubemap_face_cull() const { return ( culling_options & 128 ) == 128; }
    bool                    shadow_mesh_sphere_cull() const { return ( culling_options & 256 ) == 256; }

    void                    set_frustum_cull_meshes( bool value ) { value ? ( culling_options |= 1 ) : ( culling_options &= ~( 1 ) ); }
    void                    set_frustum_cull_meshlets( bool value ) { value ? ( culling_options |= 2 ) : ( culling_options &= ~( 2 ) ); }
    void                    set_occlusion_cull_meshes( bool value ) { value ? ( culling_options |= 4 ) : ( culling_options &= ~( 4 ) ); }
    void                    set_occlusion_cull_meshlets( bool value ) { value ? ( culling_options |= 8 ) : ( culling_options &= ~( 8 ) ); }
    void                    set_freeze_occlusion_camera( bool value ) { value ? ( culling_options |= 16 ) : ( culling_options &= ~( 16 ) ); }
    void                    set_shadow_meshlets_cone_cull( bool value ) { value ? ( culling_options |= 32 ) : ( culling_options &= ~( 32 ) ); }
    void                    set_shadow_meshlets_sphere_cull( bool value ) { value ? ( culling_options |= 64 ) : ( culling_options &= ~( 64 ) ); }
    void                    set_shadow_meshlets_cubemap_face_cull( bool value ) { value ? ( culling_options |= 128 ) : ( culling_options &= ~( 128 ) ); }
    void                    set_shadow_mesh_sphere_cull( bool value ) { value ? ( culling_options |= 256 ) : ( culling_options &= ~( 256 ) ); }

}; // struct GpuFrameData

struct alignas( 16 ) GpuLightingData {

    u32                     cubemap_shadows_index;
    u32                     debug_show_light_tiles;
    u32                     debug_show_tiles;
    u32                     debug_show_bins;

    u32                     disable_shadows;
    u32                     debug_modes;
    u32                     debug_texture_index;
    u32                     shadow_visibility_texture_index;

    u32                     volumetric_fog_texture_index;
    u32                     volumetric_fog_num_slices;
    f32                     volumetric_fog_near;
    f32                     volumetric_fog_far;

    f32                     volumetric_fog_distribution_scale;
    f32                     volumetric_fog_distribution_bias;
    f32                     gi_intensity;
    u32                     indirect_lighting_texture_index;

    u32                     bilateral_weights_texture_index;
    u32                     reflections_texture_index;

    u32                     raytraced_shadow_light_color_type;
    f32                     raytraced_shadow_light_radius;

    glm::vec3               raytraced_shadow_light_position;
    f32                     raytraced_shadow_light_intensity;

    u32                     brdf_lut_texture_index;
    u32                     pad[ 3 ];
}; // GpuLightingData


// Render Configs ////////////////////////////////////////////////////////
//
struct LightingRenderConfig {

    bool                    debug_show_light_tiles  = false;
    bool                    debug_show_tiles        = false;
    bool                    debug_show_bins         = false;
    u32                     lighting_debug_modes    = 0;
    bool                    skip_invisible_lights   = true;
    bool                    use_mcguire_method      = false;
    bool                    enable_camera_inside    = true;
    bool                    force_fullscreen_light_aabb = false;

    void                    draw_imgui();

}; // struct LightingRenderConfig

//
struct GpuCullingRenderConfig {

    bool                    enable_frustum_cull_meshes      = true;
    bool                    enable_frustum_cull_meshlets    = true;
    bool                    enable_occlusion_cull_meshes    = true;
    bool                    enable_occlusion_cull_meshlets  = true;
    bool                    freeze_occlusion_camera         = false;
    bool                    shadow_meshlets_cone_cull       = true;
    bool                    shadow_meshlets_sphere_cull     = true;
    bool                    shadow_meshlets_cubemap_face_cull = true;

    void                    draw_imgui();
}; // struct GpuCullingRenderConfig

//
struct ShadowRenderConfig {

    bool                    disable_shadows = false;
    f32                     depth_bias_constant = 1.25f;
    f32                     depth_bias_clamp = 0.0f;
    f32                     depth_bias_slope = 1.75f;


    void                    draw_imgui();

}; // struct ShadowRenderConfig

//
struct MeshletsRenderConfig {

    bool                    use_meshlets = true;
    bool                    use_meshlets_emulation = false;
    bool                    gpu_mesh_shaders_extension_present = false;

    void                    draw_imgui();

}; // struct MeshletsRenderConfig

//
struct PostProcessRenderConfig {

    i32                     tonemap_mode   = 0;
    f32                     exposure       = 1.0f;
    f32                     sharpening_amount = 0.2f;
    u32                     zoom_scale     = 2;
    f32                     bloom_amount   = 0.1f;
    bool                    enable_zoom    = false;
    bool                    block_zoom_input = false;

    void                    draw_imgui();

}; // struct PostProcessConfig

//
struct DebugDrawRenderConfig {

    bool                    show_cpu_draws = true;
    bool                    show_gpu_draws = false;

    bool                    inspect_mesh_instance = false;
    u32                     mesh_instance_index = 0; // index of mesh instance to inspect
    u32                     mesh_instances_count = u32_max;

    void                    draw_imgui();

}; // struct DebugDrawRenderConfig

//
struct VolumetricFogRenderConfig {

    u16                     texture_index               = u16_max;
    u32                     tile_size                   = 16;
    u32                     tile_count_x                = 128;
    u32                     tile_count_y                = 128;
    u32                     slices                      = 128;
    f32                     density                     = 0.0f;
    f32                     scattering_factor           = 0.1f;
    f32                     temporal_reprojection_percentage = 0.9f;
    f32                     phase_anisotropy_01         = 0.2f;
    bool                    use_temporal_reprojection   = true;
    bool                    use_spatial_filtering       = true;
    bool                    light_scattering_jitter_animated = false;
    f32                     light_scattering_jitter_scale = 1.0f;
    u32                     phase_function_type         = 0;
    f32                     height_fog_density          = 0.0f;
    f32                     height_fog_falloff          = 1.0f;
    f32                     noise_scale                 = 0.5f;
    f32                     lighting_noise_scale        = 0.11f;
    u32                     noise_type                  = 0;
    f32                     noise_position_scale        = 1.0f;
    f32                     noise_speed_scale           = 0.2f;
    glm::vec3               box_position                = glm::vec3{ 0, 0, 0 };
    glm::vec3               box_size                    = glm::vec3{ 1.f, 2.f, 0.5f };
    f32                     box_density                 = 3.0f;
    u32                     box_color                   = raptor::Color::green().abgr;
    f32                     temporal_reprojection_jittering_scale = 0.2f;
    f32                     application_dithering_scale = 0.023f;
    bool                    application_apply_opacity_anti_aliasing = true;
    bool                    application_apply_tricubic_filtering = false;

    void                    draw_imgui();

}; // struct VolumetricFogRenderConfig

// Enums
namespace JitterType {
    enum Enum {
        Halton = 0,
        R2,
        Hammersley,
        InterleavedGradients
    }; // enum Enum

} // namespace JitterType

//
struct TAARenderConfig {

    bool                    enabled                 = true;
    bool                    jittering_enabled       = true;
    JitterType::Enum        jitter_type             = JitterType::Halton;
    u32                     jitter_period           = 4;
    f32                     jitter_scale            = 1.f;
    i32                     mode                    = 1;
    bool                    use_inverse_luminance_filtering = true;
    bool                    use_temporal_filtering  = true;
    bool                    use_luminance_difference_filtering = true;
    bool                    use_ycocg               = true;
    i32                     velocity_sampling_mode  = 2;
    i32                     history_sampling_filter = 1;
    i32                     history_constraint_mode = 4;
    i32                     current_color_filter    = 1;
    f32                     current_sample_sharpness = 0.8f;

    void                    draw_imgui();

}; // struct TAARenderConfig

//
struct RaytracedShadowsConfig {
    glm::vec3               light_position = glm::vec3{ 0, 1, 0 };
    f32                     light_radius = 10.f;

    glm::vec3               light_direction = glm::vec3{ 0, 1, -0.2f };
    f32                     light_intensity = 5.f;
    glm::vec3               light_color = glm::vec3(1, 1, 1);
    i32                     light_type = 0; // 0 = directional, 1 = point

    u32                     max_samples = 4;
    
    bool                    enabled = false;
    bool                    disable_history = false;
    bool                    disable_spatial = false;

    void                    draw_imgui();
}; // struct RaytracedShadowsConfig

//
struct RaytracedReflectionsConfig {
    bool                    enabled = false;

    f32                     reflections_scale = 0.5f;
    f32                     temporal_depth_difference = 10.f;
    f32                     temporal_normal_difference = 16.f;
    f32                     wavelet_sigma_z = 1.f;
    f32                     wavelet_sigma_n = 128.f;
    f32                     wavelet_sigma_l = 4.f;

    void                    draw_imgui();
}; // struct RaytracedReflectionsConfig

//
struct REStirGIConfig {
    bool                    enabled = false;

    void                    draw_imgui();
}; // struct REStirGIConfig

//
struct RenderConfig {

    LightingRenderConfig    lighting;
    GpuCullingRenderConfig  gpu_culling;
    MeshletsRenderConfig    meshlets;
    ShadowRenderConfig      shadows;
    PostProcessRenderConfig post;
    DebugDrawRenderConfig   debug_draw;
    VolumetricFogRenderConfig volumetric_fog;
    TAARenderConfig         taa;
    RaytracedShadowsConfig  raytraced_shadows;
    RaytracedReflectionsConfig raytraced_reflections;
    REStirGIConfig          restirgi;

    u32                     cubemap_debug_face_index = 5;
    bool                    cubemap_face_debug_enabled = false;

    bool                    enable_meshlet_animations = false;

    // Global illumination
    bool                    gi_show_probes = false;
    glm::vec3               gi_probe_grid_position{ -10.0,0.5,-10.0 };
    glm::vec3               gi_probe_spacing{ 1.f, 1.f, 1.f };
    f32                     gi_probe_sphere_scale = 0.1f;
    f32                     gi_max_probe_offset = 0.4f;
    f32                     gi_self_shadow_bias = 0.3f;
    f32                     gi_hysteresis = 0.95f;
    bool                    gi_debug_border = false;
    bool                    gi_debug_border_type = false;
    bool                    gi_debug_border_source = false;
    u32                     gi_total_probes = 0;
    f32                     gi_intensity = 1.0f;
    bool                    gi_use_visibility = true;
    bool                    gi_use_backface_smoothing = true;
    bool                    gi_use_perceptual_encoding = true;
    bool                    gi_use_backface_blending = true;
    bool                    gi_use_probe_offsetting = true;
    bool                    gi_recalculate_offsets = false;     // When moving grid or changing spaces, recalculate offsets.
    bool                    gi_use_probe_status = false;
    bool                    gi_use_half_resolution = true;
    bool                    gi_use_infinite_bounces = true;
    f32                     gi_infinite_bounces_multiplier = 0.75f;
    i32                     gi_per_frame_probes_update = 1000;

    bool                    pointlight_rendering = true;
    bool                    pointlight_use_meshlets = true;
    bool                    use_tetrahedron_shadows = false;
    bool                    show_light_edit_debug_draws = false;

    bool                    cubeface_flip[ 6 ];

    f32                     global_scale = 1.f;

    // Global parameters
    bool                    use_slang_shaders = false;

    // PBR
    f32                     forced_metalness = -1.f;
    f32                     forced_roughness = -1.f;
}; // struct RenderConfig

//
struct LightingRuntimeData {

    BufferHandle            lights_list_sb[ k_max_frames ];
    BufferHandle            light_z_bins_sb[ k_max_frames ];
    BufferHandle            lights_tiles_sb[ k_max_frames ];
    BufferHandle            lights_indices_sb[ k_max_frames ];
    u32                     lighting_constants_cb_offset;

}; // struct LightingRuntimeData

struct MeshRuntimeData {

    BufferHandle            position_buffer_gpu;
    BufferHandle            tangent_buffer_gpu;
    BufferHandle            normal_buffer_gpu;
    BufferHandle            texcoord_buffer_gpu;
    BufferHandle            joints_buffer_gpu;
    BufferHandle            weights_buffer_gpu;

    BufferHandle            index_buffer_gpu;

}; // struct MeshRuntimeData

//
struct MeshletsRuntimeData {

    BufferHandle            meshlets_sb_gpu[ k_max_frames ] = {};
    BufferHandle            meshlets_vertex_pos_sb_gpu[ k_max_frames ] = {};
    BufferHandle            meshlets_vertex_data_sb_gpu[ k_max_frames ] = {};
    BufferHandle            meshlets_connectivity_data_sb_gpu = {};
    BufferHandle            meshlets_position_only_data_sb_gpu = {};

    BufferHandle            meshlets_emulation_instances_sb[ k_max_frames ];
    BufferHandle            meshlets_emulation_index_buffer_sb[ k_max_frames ];
    BufferHandle            meshlets_emulation_visible_instances_sb[ k_max_frames ];

    DescriptorSetHandle     meshlets_emulation_draw_descriptor_set[ k_max_frames ];
    DescriptorSetHandle     meshlets_early_draw_descriptor_set[ k_max_frames ];
    DescriptorSetHandle     meshlets_early_draw_descriptor_set_slang[ k_max_frames ];
    DescriptorSetHandle     meshlets_late_draw_descriptor_set[ k_max_frames ];
    DescriptorSetHandle     meshlets_late_draw_descriptor_set_slang[ k_max_frames ];
    DescriptorSetHandle     meshlets_transparent_draw_descriptor_set[ k_max_frames ];
    Array<DescriptorSetHandle> skinning_descriptor_set[ k_max_frames ];

}; // struct MeshletsRuntimeData

//
struct GpuCullingRuntimeData {

    BufferHandle            meshlet_indirect_early_count_sb[ k_max_frames ];
    BufferHandle            meshlet_indirect_early_commands_sb[ k_max_frames ];

    // Contains the mesh instance ids that were culled in the early culling pass
    BufferHandle            meshlet_culled_mesh_instance_ids_sb[ k_max_frames ];

    // Contains the total mesh instances to be re-tested in the late culling pass
    BufferHandle            meshlet_culling_handoff_sb[ k_max_frames ];

    BufferHandle            meshlet_indirect_late_count_sb[ k_max_frames ];
    BufferHandle            meshlet_indirect_late_commands_sb[ k_max_frames ];

    glm::mat4               projection_transpose{ };

}; // struct GpuCullingRuntimeData

//
struct DebugDrawRuntimeData {

    // Cpu Drawing
    BufferHandle            cpu_lines_vb;
    BufferHandle            cpu_lines2d_vb;

    // Gpu Drawing
    BufferHandle            gpu_line_sb;
    BufferHandle            gpu_line_count_sb;
    BufferHandle            gpu_line_commands_sb;

    u32                     cpu_lines_count = 0;
    u32                     cpu_lines2d_count = 0;

}; // struct DebugDrawRuntimeData

//
struct PointlightShadowsRuntimeData {

    u32                     cubemap_shadows_index;

}; // struct PointlightShadowsRuntimeData

//
struct ViewRuntimeData {
    glm::mat4               view_projection;
    glm::mat4               inverse_view_projection;
    f32                     near;
    f32                     far;
}; // struct ViewRuntimeData

//
//
struct RenderBlackboard {

    LightingRuntimeData     lighting;
    MeshRuntimeData         geometry_data;
    MeshletsRuntimeData     meshlets;
    GpuCullingRuntimeData   gpu_culling;
    DebugDrawRuntimeData    debug_draw;
    PointlightShadowsRuntimeData point_shadows;
    ViewRuntimeData         main_view;

    // Gpu buffers
    u32                     scene_cb_offset;
    BufferHandle            meshes_sb = {};
    BufferHandle            mesh_bounds_sb = {};
    BufferHandle            mesh_aabbs_sb = {};
    BufferHandle            mesh_instances_sb = {};
    BufferHandle            physics_cb = {};

    // Indirect data
    BufferHandle            meshlet_emulation_instances_indirect_count_sb[ k_max_frames ];

    ImageHandle             fragment_shading_rate_image;
    ImageViewHandle         fragment_shading_rate_image_view;
    ImageViewHandle         motion_vector_image_view;
    ImageViewHandle         visibility_motion_vector_image_view;
    ImageViewHandle         brdf_lut_image_view;
    ImageViewHandle         taa_output_image_view;

    u32                     lighting_debug_texture_index = 0;
    u32                     cubemap_debug_array_index = 0;
    u32                     blue_noise_128_rg_image_view_index = 0;
    u32                     jitter_index = 0;
    glm::vec2               jitter_offsets;

    TLASHandle              tlas;

    u32                     ddgi_constants_offset;
    BufferHandle            ddgi_probe_status_cache{};

    u32                     render_width;
    u32                     render_height;
    u32                     swapchain_width;
    u32                     swapchain_height;
    f32                     render_scale_factor;    // ratio between swapchain and render resolutions

}; // struct RenderBlackboard

} // namespace raptor
