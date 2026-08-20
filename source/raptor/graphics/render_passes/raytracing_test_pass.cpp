#include "graphics/render_passes/raytracing_test_pass.hpp"
#include "graphics/render_scene.hpp"
#include "graphics/render_blackboard.hpp"

namespace raptor {

struct RayTracingTestGpuData {
    u32 sbt_offset; // shader binding table offset
    u32 sbt_stride; // shader binding table stride
    u32 miss_index;
    u32 out_image_index;
}; // RayTracingTestGpuData

void RayTracingTestPass::render( FrameGraphRenderContext& context ) {

    if ( !enabled )
        return;

    RenderScene* render_scene = context.render_view->scene;
    u32 current_frame_index = context.current_frame_index;
    CommandBuffer* cb = context.gpu_commands;
    Renderer* renderer = context.renderer;
    RenderBlackboard& render_blackboard = *context.render_blackboard;

    // Lazy creation of descriptor set to have access to valid Acceleration Structure
    if ( render_blackboard.tlas.is_invalid() ) {
        return;
    }

    if ( needs_resources_creation ) {

        GpuDevice& gpu = *renderer->gpu;
        DescriptorSetLayoutHandle layout = gpu.get_descriptor_set_layout( pipeline.pipeline, k_material_descriptor_set_index );
        DescriptorSetBinder descriptors;

        ShaderReflectionInfo* shader_reflection = renderer->get_shader_reflection( pipeline.pipeline );
        descriptors.reset();
        descriptors.dynamic_buffers.push( { renderer->get_binding_index( shader_reflection, "rayParams" ), sizeof( RayTracingTestGpuData ) } );
        descriptors.name = "ray_tracing_test_ds";

        descriptor_set = renderer->create_descriptor_set( descriptors, shader_reflection, pipeline.pipeline, 0, render_blackboard );

        needs_resources_creation = false;
    }

    cb->bind_pipeline( pipeline.pipeline );

    cb->bind_descriptor_set( { renderer->gpu->bindless_descriptor_set, descriptor_set },
                              { render_blackboard.scene_cb_offset, constants_offset } );

    FrameGraphResource* output_resource = context.frame_graph->get_resource( "ray_tracing_test_output" );

    cb->trace_rays( pipeline.pipeline, output_resource->resource_info.texture.width, output_resource->resource_info.texture.height, 1 );
}

void RayTracingTestPass::declare_frame_graph_node( FrameGraphResourceContext& context ) {
    FrameGraphBuilder& builder = *context.frame_graph->builder;

    context.frame_graph->add_node_v2( {
        //.inputs = {
        //    {
        //        .type = FrameGraphResourceType_Buffer,
        //        .handle = builder.get_output_handle( "animated_meshlet_pass", "meshlet_vertex_buffer" )
        //    }
        //},
        .outputs = {
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Attachment,
                .resource_info{
                    .texture = {
                        .scale_width = 1.0f,
                        .scale_height = 1.0f,
                        .format = VK_FORMAT_R8G8B8A8_UNORM,
                        .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
                        .compute = true
                    }
                },
                .name = "ray_tracing_test_output",
            } ),
        },
        .scheduling = { CommandQueueType::Graphics, 0 },
        .enabled = true,
        .compute = true,
        .name = "ray_tracing_test_pass" } );
}

ShaderCompilationCreation scc_ray_tracing_test = {
    .stages = {
        {
            .source_file_path = "glsl/ray_tracing.glsl",
            .type = VK_SHADER_STAGE_RAYGEN_BIT_KHR,
        },
        {
            .source_file_path = "glsl/ray_tracing.glsl",
            .type = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
        },
        {
            .source_file_path = "glsl/ray_tracing.glsl",
            .type = VK_SHADER_STAGE_MISS_BIT_KHR,
        },
    },
    .name = "ray_tracing_test_shader",
    .slang_input = 0,
};

void RayTracingTestPass::update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) {

    Renderer* renderer = context.renderer;

    if ( phase == PipelineUpdatePhase::Destroy ) {
        renderer->destroy_ray_tracing_pipeline_state( pipeline );

        return;
    }

    RayTracingPipelineTransaction transaction( renderer );

    PipelineCreation pipeline_creation = {
        .name = "ray_tracing_test_pipeline",
        .render_pass_name = "ray_tracing_test_pass",
    };

    RayTracingPipelineState& new_pipeline = transaction.add( pipeline );

    renderer->create_raytracing_pipeline_state( scc_ray_tracing_test, pipeline_creation,
                                                pipeline_creation.name, context.frame_graph, new_pipeline );

    transaction.commit_or_rollback();
}

void RayTracingTestPass::upload_gpu_data( FrameGraphResourceContext& context ) {

    if ( !enabled )
        return;

    Renderer* renderer = context.renderer;
    u32 current_frame_index = renderer->gpu->current_frame;
    RenderBlackboard& render_blackboard = *context.render_blackboard;

    RayTracingTestGpuData* gpu_data = renderer->gpu->dynamic_buffer_allocate<RayTracingTestGpuData>( &constants_offset );
    if ( gpu_data ) {
        gpu_data->sbt_offset = 0; // shader binding table offset
        gpu_data->sbt_stride = renderer->gpu->ray_tracing_pipeline_properties.shaderGroupHandleAlignment; // shader binding table stride
        gpu_data->miss_index = 0;

        FrameGraphResource* output_resource = context.frame_graph->access_output_resource( context.frame_graph->get_node( "ray_tracing_test_pass" )->outputs[ 0 ] );
        gpu_data->out_image_index = output_resource->resource_info.texture.image_view.index();
    }
}

void RayTracingTestPass::create_gpu_resources( FrameGraphResourceContext& context ) {

    FrameGraph* frame_graph = context.frame_graph;

    FrameGraphNode* node = frame_graph->get_node( "mesh_occlusion_early_pass" );
    if ( node == nullptr ) {
        enabled = false;

        return;
    }

    enabled = node->enabled;
}

void RayTracingTestPass::destroy_gpu_resources( FrameGraphResourceContext& context ) {

    Renderer* renderer = context.renderer;
    renderer->gpu->destroy_descriptor_set( descriptor_set );
}

} // namespace raptor
