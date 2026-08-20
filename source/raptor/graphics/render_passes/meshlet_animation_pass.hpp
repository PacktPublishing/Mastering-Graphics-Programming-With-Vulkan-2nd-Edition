#pragma once

#include "graphics/frame_graph.hpp"
#include "graphics/renderer.hpp"

namespace raptor {

    struct Renderer;

    //
    //
    struct MeshletAnimationPass : public FrameGraphRenderPass {

        void                    declare_frame_graph_node( FrameGraphResourceContext& context ) override;

        void                    update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) override;

        void                    render( FrameGraphRenderContext& context ) override;

        void                    create_gpu_resources( FrameGraphResourceContext& context ) override;
        void                    destroy_gpu_resources( FrameGraphResourceContext& context ) override;

        Array<u32>              animated_mesh_indices;
        //BufferHandle            mesh_list_buffer;
        BufferHandle            vertex_update_indirect_buffer;
        BufferHandle            meshlet_update_indirect_buffer;

        ComputePipelineState    vertex_update_pipeline;
        ComputePipelineState    meshlet_update_pipeline;

        Array<DescriptorSetHandle> vertex_update_descriptor_set[ k_max_frames ];
        DescriptorSetHandle     meshlet_update_descriptor_set[ k_max_frames ];

    }; // struct MeshletAnimationPass


} // namespace raptor
