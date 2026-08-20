#pragma once

#include "graphics/frame_graph.hpp"
#include "graphics/renderer.hpp"

namespace raptor {

    struct Renderer;

    //
    //
    struct MotionVectorPass : public FrameGraphRenderPass {

        void                    declare_frame_graph_node( FrameGraphResourceContext& context ) override;
        void                    update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) override;

        void                    render( FrameGraphRenderContext& context ) override;

        void                    on_resize( FrameGraphResourceContext& context, u32 new_width, u32 new_height ) override;

        void                    create_gpu_resources( FrameGraphResourceContext& context ) override;
        void                    upload_gpu_data( FrameGraphResourceContext& context ) override;
        void                    destroy_gpu_resources( FrameGraphResourceContext& context ) override;

        void                    update_dependent_resources( FrameGraphResourceContext& context ) override;
        void                    create_descriptors( GpuDevice* gpu, RenderScene* scene, ImageViewHandle gbuffer_normals );

        ComputePipelineState    camera_composite_pipeline;
        DescriptorSetHandle     camera_composite_descriptor_set;

    }; // struct MotionVectorPass

} // namespace raptor