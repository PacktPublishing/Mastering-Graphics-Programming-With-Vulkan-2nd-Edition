#pragma once

#include "graphics/frame_graph.hpp"
#include "graphics/mesh.hpp"
#include "graphics/renderer.hpp"

namespace raptor {

    struct Renderer;

    //
    //
    struct GBufferPass : public FrameGraphRenderPass {

        void                    declare_frame_graph_node( FrameGraphResourceContext& context ) override;

        void                    update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) override;

        void                    pre_render( FrameGraphRenderContext& context ) override;
        void                    render( FrameGraphRenderContext& context ) override;

        void                    create_gpu_resources( FrameGraphResourceContext& context ) override;
        void                    destroy_gpu_resources( FrameGraphResourceContext& context ) override;

        GraphicsPipelineState   meshlet_draw_pipeline;
        GraphicsPipelineState   meshlet_draw_pipeline_slang;
        GraphicsPipelineState   skinning_pipeline;

        PipelineHandle          meshlet_emulation_draw_pipeline;

        BufferHandle            generate_meshlet_dispatch_indirect_buffer[ k_max_frames ];
        PipelineHandle          generate_meshlet_index_buffer_pipeline;
        DescriptorSetHandle     generate_meshlet_index_buffer_descriptor_set[ k_max_frames ];
        PipelineHandle          generate_meshlets_instances_pipeline;
        DescriptorSetHandle     generate_meshlets_instances_descriptor_set[ k_max_frames ];
        BufferHandle            meshlet_instance_culling_indirect_buffer[ k_max_frames ];
        PipelineHandle          meshlet_instance_culling_pipeline;
        DescriptorSetHandle     meshlet_instance_culling_descriptor_set[ k_max_frames ];
        PipelineHandle          meshlet_write_counts_pipeline;

    }; // struct GBufferPass

    //
    //
    struct LateGBufferPass : public FrameGraphRenderPass {

        void                    declare_frame_graph_node( FrameGraphResourceContext& context ) override;

        void                    render( FrameGraphRenderContext& context ) override;

        void                    create_gpu_resources( FrameGraphResourceContext& context ) override;
        void                    destroy_gpu_resources( FrameGraphResourceContext& context ) override;

        // Array<MeshInstanceDraw> mesh_instance_draws;
        u32                     meshlet_technique_index;
        u32                     meshlet_technique_slang_index;
    }; // struct LateGBufferPass


} // namespace raptor
