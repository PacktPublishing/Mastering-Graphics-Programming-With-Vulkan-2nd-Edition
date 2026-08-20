#pragma once

#include "graphics/frame_graph.hpp"
#include "graphics/renderer.hpp"

namespace raptor {

    struct BufferResource;
    struct Renderer;
    struct SceneGraph;

    //
    // NEW
    struct DebugDrawPass : public FrameGraphRenderPass {

        void                    declare_frame_graph_node( FrameGraphResourceContext& context ) override;

        void                    update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) override;

        void                    pre_render( FrameGraphRenderContext& context ) override;
        void                    render( FrameGraphRenderContext& context ) override;

        void                    create_gpu_resources( FrameGraphResourceContext& context ) override;
        void                    destroy_gpu_resources( FrameGraphResourceContext& context ) override;

        GraphicsPipelineState   cpu_line_draw_pipeline;
        GraphicsPipelineState   cpu_line_2d_draw_pipeline;

        GraphicsPipelineState   gpu_line_draw_pipeline;
        GraphicsPipelineState   gpu_line_2d_draw_pipeline;

        ComputePipelineState    gpu_commands_finalize;

        DescriptorSetHandle     cpu_line_draw_descriptor_set;
        DescriptorSetHandle     gpu_line_draw_descriptor_set;
        DescriptorSetHandle     gpu_commands_finalize_descriptor_set;

    }; // struct DebugDrawPass

    //
    //
    struct DebugPass : public FrameGraphRenderPass {
        void                    declare_frame_graph_node( FrameGraphResourceContext& context ) override;

        void                    update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) override;

        void                    render( FrameGraphRenderContext& context ) override;
        void                    pre_render( FrameGraphRenderContext& context ) override;

        void                    create_gpu_resources( FrameGraphResourceContext& context ) override;
        void                    destroy_gpu_resources( FrameGraphResourceContext& context ) override;
        void                    update_dependent_resources( FrameGraphResourceContext& context ) override;

        BufferHandle            sphere_mesh_buffer;
        BufferHandle            sphere_mesh_indices;
        BufferHandle            sphere_matrices_buffer[ k_max_frames ];
        BufferHandle            sphere_draw_indirect_buffer;
        u32                     sphere_index_count;

        BufferHandle            cone_mesh_buffer;
        BufferHandle            cone_mesh_indices;
        BufferHandle            cone_matrices_buffer[ k_max_frames ];
        BufferHandle            cone_draw_indirect_buffer;
        u32                     cone_index_count;

        BufferHandle            line_buffer;

        u32                     bounding_sphere_count;

        DescriptorSetHandle     sphere_mesh_descriptor_set[ 2 ];
        DescriptorSetHandle     cone_mesh_descriptor_set[ 2 ];

        PipelineHandle          debug_lines_finalize_pipeline;
        DescriptorSetHandle     debug_lines_finalize_set;

        PipelineHandle          debug_lines_draw_pipeline;
        PipelineHandle          debug_lines_2d_draw_pipeline;
        DescriptorSetHandle     debug_lines_draw_set;

        BufferHandle            debug_line_commands_sb_cache;

        GraphicsPipelineState   debug_mesh_pipeline;
        DescriptorSetHandle     debug_mesh_descriptor_set;

        ComputePipelineState    debug_update_sphere_matrices_pipeline;
        DescriptorSetHandle     debug_update_sphere_matrices_descriptor_set[ k_max_frames ];

        ComputePipelineState    debug_update_cone_matrices_pipeline;
        DescriptorSetHandle     debug_update_cone_matrices_descriptor_set[ k_max_frames ];
    }; // struct DebugPass

} // namespace raptor
