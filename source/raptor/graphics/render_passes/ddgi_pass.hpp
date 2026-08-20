#pragma once

#include "graphics/frame_graph.hpp"
#include "external/glm/vec3.hpp"
#include "external/glm/mat4x4.hpp"

#if 0

namespace raptor {

    struct Renderer;

    struct alignas( 16 ) GpuDDGIConstants {
        u32         radiance_output_index;
        u32         grid_irradiance_output_index;
        u32         indirect_output_index;
        u32         normal_texture_index;

        u32         depth_pyramid_texture_index;
        u32         depth_fullscreen_texture_index;
        u32         grid_visibility_texture_index;
        u32         probe_offset_texture_index;

        f32         hysteresis;
        f32         infinte_bounces_multiplier;
        i32         probe_update_offset;
        i32         probe_update_count;

        glm::vec3   probe_grid_position;
        f32         probe_sphere_scale;

        glm::vec3   probe_spacing;
        f32         max_probe_offset;   // [0,0.5] max offset for probes

        glm::vec3   reciprocal_probe_spacing;
        f32         self_shadow_bias;

        i32         probe_counts[ 3 ];
        u32         debug_options;

        i32         irradiance_texture_width;
        i32         irradiance_texture_height;
        i32         irradiance_side_length;
        i32         probe_rays;

        i32         visibility_texture_width;
        i32         visibility_texture_height;
        i32         visibility_side_length;
        u32         pad1;

        glm::mat4   random_rotation;
    }; // struct GpuDDGIConstants

    enum class DDGISteps {
        ProbeRaytrace,
        ProbeRaytraceSlang,
        UpdateIrradiance,
        //UpdateIrradianceSlang,
        UpdateVisibility,
        //UpdateVisibilitySlang,
        CalculateProbeOffsets,
        //CalculateProbeOffsetsSlang,
        CalculateProbeStatuses,
        //CalculateProbeStatusesSlang,
        SampleIrradiance,
        //SampleIrradianceSlang,
        DebugMesh,
        //DebugMeshSlang,
        Count
    };

    //
    //
    struct DDGIPass : public FrameGraphRenderPass {

        void                    update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) override;

        void                    pre_render( FrameGraphRenderContext& context ) override;
        void                    render( FrameGraphRenderContext& context ) override;

        void                    on_resize( FrameGraphResourceContext& context, u32 new_width, u32 new_height ) override;

        void                    create_gpu_resources( FrameGraphResourceContext& context ) override;
        void                    upload_gpu_data( FrameGraphResourceContext& context ) override;
        void                    destroy_gpu_resources( FrameGraphResourceContext& context ) override;

        u32                     get_total_probes()                  { return probe_count_x * probe_count_y * probe_count_z; }
        u32                     get_total_rays()                    { return probe_rays * get_total_probes(); }

        Renderer*               renderer;

        PipelineHandle          pipelines[ (size_t)DDGISteps::Count ];

        BufferHandle            ddgi_probe_status_buffer;

        DescriptorSetHandle     probe_raytrace_descriptor_set;
        DescriptorSetHandle     probe_raytrace_descriptor_set_slang;
        ImageHandle             probe_raytrace_radiance_image;
        ImageViewHandle         probe_raytrace_radiance_image_view;

        DescriptorSetHandle     probe_grid_update_descriptor_set;
        ImageHandle             probe_grid_irradiance_image;
        ImageViewHandle         probe_grid_irradiance_image_view;
        ImageHandle             probe_grid_visibility_image;
        ImageViewHandle         probe_grid_visibility_image_view;

        ImageHandle             probe_offsets_image;
        ImageViewHandle         probe_offsets_image_view;

        DescriptorSetHandle     sample_irradiance_descriptor_set;

        ImageHandle             indirect_image;
        ImageViewHandle         indirect_image_view;

        ImageViewHandle         normals_image_view;
        u32                     depth_pyramid_texture_index;
        ImageViewHandle         depth_fullscreen_image_view;

        u32                     probe_count_x = 20;
        u32                     probe_count_y = 12;
        u32                     probe_count_z = 20;

        i32                     per_frame_probe_updates = 0;
        i32                     probe_update_offset = 0;

        i32                     probe_rays = 128;
        i32                     irradiance_atlas_width;
        i32                     irradiance_atlas_height;
        i32                     irradiance_probe_size = 6;  // Irradiance is a 6x6 quad with 1 pixel borders for bilinear filtering, total 8x8

        i32                     visibility_atlas_width;
        i32                     visibility_atlas_height;
        i32                     visibility_probe_size = 6;

        bool                    half_resolution_output = false;

    }; // struct IndirectPass

} // namespace raptor

#endif // 0