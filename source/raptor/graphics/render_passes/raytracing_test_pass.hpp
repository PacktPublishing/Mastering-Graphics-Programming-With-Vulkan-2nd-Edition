#pragma once

#include "graphics/frame_graph.hpp"
#include "graphics/renderer.hpp"

namespace raptor {

    struct Renderer;


    //
    //
    struct RayTracingTestPass : public FrameGraphRenderPass {

        void                    declare_frame_graph_node( FrameGraphResourceContext& context ) override;

        void                    update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) override;
        
        void                    render( FrameGraphRenderContext& context ) override;

        void                    upload_gpu_data( FrameGraphResourceContext& context ) override;
        void                    create_gpu_resources( FrameGraphResourceContext& context ) override;
        void                    destroy_gpu_resources( FrameGraphResourceContext& context ) override;

        RayTracingPipelineState pipeline;

        DescriptorSetHandle     descriptor_set;
        bool                    needs_resources_creation = true;
        u32                     constants_offset = u32_max;

    }; // struct RayTracingTestPass

    //
    //
    // struct RayTracingTestPass : public FrameGraphRenderPass {
    //     struct GpuData {
    //         u32 sbt_offset; // shader binding table offset
    //         u32 sbt_stride; // shader binding table stride
    //         u32 miss_index;
    //         u32 out_image_index;
    //     };

    //     void                    render( FrameGraphRenderContext& context ) override;
    //     void                    on_resize( FrameGraphResourceContext& context, u32 new_width, u32 new_height ) override;

    //     void                    create_gpu_resources( FrameGraphResourceContext& context ) override;
    //     void                    upload_gpu_data( FrameGraphResourceContext& context ) override;
    //     void                    destroy_gpu_resources( FrameGraphResourceContext& context ) override;

    //     Renderer* renderer;

    //     PipelineHandle          pipeline;
    //     DescriptorSetHandle     descriptor_set;
    //     ImageHandle             render_target;
    //     ImageViewHandle         render_target_view;
    //     u32                     constants_offset;
    //     bool                    owns_render_target;

    // }; // struct RayTracingTestPass

} // namespace raptor