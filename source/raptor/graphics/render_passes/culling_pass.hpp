#pragma once

#include "graphics/frame_graph.hpp"
#include "graphics/renderer.hpp"

namespace raptor {

    struct Renderer;

    //
    //
    struct CullingEarlyPass : public FrameGraphRenderPass {

        void                    declare_frame_graph_node( FrameGraphResourceContext& context ) override;

        void                    update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) override;
        
        void                    render( FrameGraphRenderContext& context ) override;

        void                    create_gpu_resources( FrameGraphResourceContext& context ) override;
        void                    destroy_gpu_resources( FrameGraphResourceContext& context ) override;

        ComputePipelineState    cull_pipeline;

        DescriptorSetHandle     cull_descriptor_set[ k_max_frames ];
        SamplerHandle           depth_pyramid_sampler;

    }; // struct CullingEarlyPass

    //
    //
    struct CullingLatePass : public FrameGraphRenderPass {

        void                    declare_frame_graph_node( FrameGraphResourceContext& context ) override;

        void                    update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) override;

        void                    render( FrameGraphRenderContext& context ) override;

        void                    create_gpu_resources( FrameGraphResourceContext& context ) override;
        void                    destroy_gpu_resources( FrameGraphResourceContext& context ) override;

        ComputePipelineState    cull_pipeline;

        DescriptorSetHandle     cull_descriptor_set[ k_max_frames ];
        SamplerHandle           depth_pyramid_sampler;

    }; // struct CullingLatePass


} // namespace raptor