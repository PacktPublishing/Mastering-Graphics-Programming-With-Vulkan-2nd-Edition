#include "graphics/render_passes/motion_vector_pass.hpp"
#include "graphics/render_scene.hpp"
#include "graphics/render_blackboard.hpp"

#include "foundation/numerics.hpp"

namespace raptor {

static ShaderCompilationCreation scc_motion_vector = {
    .stages = {
        {
            .source_file_path = "glsl/motion_vectors.glsl",
            .type = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    },
    .name = "composite_camera_motion",
    .slang_input = 0,
};

// MotionVectorPass ////////////////////////////////////////////////////////
struct MotionVectorPushConstants {
    u32     motion_vectors_index;
    u32     visibility_motion_vectors_index;
    u32     normals_index;
    u32     pad000_mvpc;
};

void MotionVectorPass::render( FrameGraphRenderContext& context ) {

    if ( !enabled ) {
        return;
    }

    CommandBuffer* cb = context.gpu_commands;
    Renderer* renderer = context.renderer;
    RenderBlackboard& render_blackboard = *context.render_blackboard;

    cb->bind_pipeline( camera_composite_pipeline.pipeline );

    VkImageSubresourceRange range = range_aspect( VK_IMAGE_ASPECT_COLOR_BIT,
                                                  0, VK_REMAINING_MIP_LEVELS,
                                                  0, VK_REMAINING_ARRAY_LAYERS );

    cb->bind_descriptor_set( { renderer->gpu->bindless_descriptor_set, camera_composite_descriptor_set },
                              { render_blackboard.scene_cb_offset } );

    FrameGraph* frame_graph = context.frame_graph;
    FrameGraphResource* gbuffer_normals_resource = frame_graph->get_resource( "gbuffer_normals" );
    RASSERT( gbuffer_normals_resource );

    MotionVectorPushConstants push_constants{
        .motion_vectors_index = render_blackboard.motion_vector_image_view.index(),
        .visibility_motion_vectors_index = render_blackboard.visibility_motion_vector_image_view.index(),
        .normals_index = gbuffer_normals_resource->resource_info.texture.image_view.index()
    };

    cb->push_constants( camera_composite_pipeline.pipeline, 0, sizeof( MotionVectorPushConstants ), &push_constants );

    cb->dispatch( raptor::ceilu32( render_blackboard.render_width / 8.0f ), 
                  raptor::ceilu32( render_blackboard.render_height / 8.0f ), 1 );
}

void MotionVectorPass::declare_frame_graph_node( FrameGraphResourceContext& context ) {
    FrameGraphBuilder& builder = *context.frame_graph->builder;

    context.frame_graph->add_node_v2( {
        .inputs = {
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "gbuffer_pass_late", "depth" )
            },
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "gbuffer_pass_late", "gbuffer_normals" )
            }
        },
        .outputs = {
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Attachment,
                .resource_info{
                    .texture = {
                        .scale_width = 1.0f,
                        .scale_height = 1.0f,
                        .format = VK_FORMAT_R16G16_SFLOAT,
                        .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
                        .clear_values = { 0.0f, 0.0f, 0.0f, 0.0f },
                        .compute = true
                    }
                },
                .name = "motion_vectors",
            } ),
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Attachment,
                .resource_info{
                    .texture = {
                        .scale_width = 1.0f,
                        .scale_height = 1.0f,
                        .format = VK_FORMAT_R16G16_SFLOAT,
                        .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
                        .clear_values = { 0.0f, 0.0f, 0.0f, 0.0f },
                        .compute = true
                    }
                },
                .name = "visibility_motion_vectors",
            } ),
        },
        .scheduling = { CommandQueueType::Graphics, 0 },
        .enabled = true,
        .compute = true,
        .name = "motion_vector_pass" } );
}

void MotionVectorPass::update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) {

    Renderer* renderer = context.renderer;

    if ( phase == PipelineUpdatePhase::Destroy ) {
        renderer->destroy_compute_pipeline_state( camera_composite_pipeline );
        return;
    }

    ComputePipelineTransaction transaction( renderer );

    PipelineCreation pipeline_creation = {
        .name = "motion_vector_pass_pipeline",
        .render_pass_name = "motion_vector_pass",
    };

    ComputePipelineState& pipeline = transaction.add( camera_composite_pipeline );

    renderer->create_compute_pipeline_state( scc_motion_vector, pipeline_creation,
        "motion_vector_pass_pipeline", context.frame_graph, pipeline );

    transaction.commit_or_rollback();
}

void MotionVectorPass::create_gpu_resources( FrameGraphResourceContext& context ) {

    FrameGraph* frame_graph = context.frame_graph;
    FrameGraphNode* node = frame_graph->get_node( "motion_vector_pass" );
    if ( node == nullptr ) {
        enabled = false;
        return;
    }

    enabled = node->enabled;

    Renderer* renderer = context.renderer;
    GpuDevice& gpu = *renderer->gpu;
    RenderBlackboard& render_blackboard = *context.render_blackboard;

    FrameGraphResource* gbuffer_normals_resource = frame_graph->get_resource( "gbuffer_normals" );
    RASSERT( gbuffer_normals_resource != nullptr );

    ShaderReflectionInfo* shader_reflection = renderer->get_shader_reflection( camera_composite_pipeline.pipeline );
    RASSERT( shader_reflection != nullptr );

    DescriptorSetBinder descriptors;
    descriptors.reset();
    descriptors.name = "motion_vector_ds";

    camera_composite_descriptor_set = renderer->create_descriptor_set( descriptors, shader_reflection,
                                                                       camera_composite_pipeline.pipeline, 0, 
                                                                       render_blackboard );
}

void MotionVectorPass::destroy_gpu_resources( FrameGraphResourceContext& context ) {

    if ( !enabled ) {
        return;
    }

    Renderer* renderer = context.renderer;
    renderer->gpu->destroy_descriptor_set( camera_composite_descriptor_set );
}

void MotionVectorPass::update_dependent_resources( FrameGraphResourceContext& context ) {

}

void MotionVectorPass::on_resize( FrameGraphResourceContext& context, u32 new_width, u32 new_height ) {}

void MotionVectorPass::upload_gpu_data( FrameGraphResourceContext& context ) {

    FrameGraph* frame_graph = context.frame_graph;
    RenderBlackboard& render_blackboard = *context.render_blackboard;
    // Upload blackboard image views

    render_blackboard.motion_vector_image_view = frame_graph->get_resource( "motion_vectors" )->resource_info.texture.image_view;
    render_blackboard.visibility_motion_vector_image_view = frame_graph->get_resource( "visibility_motion_vectors" )->resource_info.texture.image_view;
}

} // namespace raptor