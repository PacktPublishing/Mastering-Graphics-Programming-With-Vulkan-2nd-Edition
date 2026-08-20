#pragma once

#include "graphics/frame_graph.hpp"
#include "graphics/renderer.hpp"

namespace raptor {

    struct Renderer;

    //
    //
    struct RaytracedReflectionsPass : public FrameGraphRenderPass {
        struct GpuReflectionsConstants {
            u32 sbt_offset; // shader binding table offset
            u32 sbt_stride; // shader binding table stride
            u32 miss_index;
            u32 out_image_index;

            u32 gbuffer_texures[4]; // x = roughness, y = normals, z = indirect lighting
        };

        void                    declare_frame_graph_node( FrameGraphResourceContext& context ) override;

        void                    pre_render( FrameGraphRenderContext& context ) override;
        void                    render( FrameGraphRenderContext& context ) override;

        void                    on_resize( FrameGraphResourceContext& context, u32 new_width, u32 new_height ) override;

        void                    create_gpu_resources( FrameGraphResourceContext& context ) override;
        void                    upload_gpu_data( FrameGraphResourceContext& context ) override;
        void                    destroy_gpu_resources( FrameGraphResourceContext& context ) override;

        void                    update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) override;

        void                    update_dependent_resources( FrameGraphResourceContext& context ) override;
        void                    create_descriptors( FrameGraphRenderContext& context );

        Renderer*               renderer;

        u32                     constants_offset;

        ImageHandle             reflections_image;
        ImageViewHandle         reflections_image_view;

        // ImageViewHandle         indirect_image_view;
        ImageViewHandle         roughness_image_view;
        ImageViewHandle         normals_image_view;

        bool                    descriptors_created = false;
        DescriptorSetHandle     reflections_descriptor_set;
        RayTracingPipelineState reflections_pipeline;

        DescriptorSetHandle     brdf_lut_generation_descriptor_set;
        ComputePipelineState    brdf_lut_generation_pipeline;
        ImageHandle             brdf_lut_image;
        ImageViewHandle         brdf_lut_image_view;

        f32                     texture_scale       = 1.f;

    }; // RaytracedReflectionsPass

} // namespace raptor
