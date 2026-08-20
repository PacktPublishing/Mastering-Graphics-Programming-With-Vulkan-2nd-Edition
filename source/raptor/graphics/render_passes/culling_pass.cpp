
#include "graphics/render_passes/culling_pass.hpp"
#include "graphics/render_scene.hpp"
#include "graphics/render_blackboard.hpp"

#include "foundation/numerics.hpp"

namespace raptor {

// CullingEarlyPass /////////////////////////////////////////////////////////
void CullingEarlyPass::render( FrameGraphRenderContext& context ) {

    if ( !enabled )
        return;

    RenderScene* render_scene = context.render_view->scene;
    u32 current_frame_index = context.current_frame_index;
    CommandBuffer* cb = context.gpu_commands;
    Renderer* renderer = context.renderer;
    RenderBlackboard& render_blackboard = *context.render_blackboard;
    GpuCullingRuntimeData& gpu_culling = render_blackboard.gpu_culling;

    // Frustum cull meshes
    GpuMeshDrawCounts& mesh_draw_counts = render_scene->mesh_draw_counts;
    mesh_draw_counts.opaque_mesh_visible_count = 0;
    mesh_draw_counts.opaque_mesh_culled_count = 0;
    mesh_draw_counts.transparent_mesh_visible_count = 0;
    mesh_draw_counts.transparent_mesh_culled_count = 0;
    // Initialize total mesh instances count
    mesh_draw_counts.total_count = render_scene->mesh_instances.size;

    FrameGraphResource* depth_pyramid_resource = context.frame_graph->get_resource( "depth_pyramid" );
    mesh_draw_counts.depth_pyramid_texture_index = depth_pyramid_resource->resource_info.texture.image_view.index();
    mesh_draw_counts.late_flag = 0;
    mesh_draw_counts.meshlet_index_count = 0;
    mesh_draw_counts.dispatch_task_x = 0;
    mesh_draw_counts.dispatch_task_y = 1;
    mesh_draw_counts.dispatch_task_z = 1;

    // Reset mesh draw counts
    
    BufferHandle visible_commands_sb = gpu_culling.meshlet_indirect_early_commands_sb[ current_frame_index ];
    BufferHandle count_sb = gpu_culling.meshlet_indirect_early_count_sb[ current_frame_index ];
    BufferHandle culling_handoff_sb = gpu_culling.meshlet_culling_handoff_sb[ current_frame_index ];

    // Prepare CPU/GPU initialized buffers for transfer writes.
    cb->add_buffer_barrier( count_sb, 0, sizeof( GpuMeshDrawCounts ),
                            { VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                              VK_ACCESS_2_TRANSFER_WRITE_BIT } );

    cb->add_buffer_barrier( culling_handoff_sb, 0, sizeof( u32 ) * 4,
                            { VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                              VK_ACCESS_2_TRANSFER_WRITE_BIT } );

    cb->flush_barriers();

    cb->update_buffer( count_sb, 0, sizeof( GpuMeshDrawCounts ), &mesh_draw_counts );
    cb->fill_buffer( culling_handoff_sb, 0, sizeof( u32 ) * 4, 0 );

    // Make all culling outputs available to compute.
    cb->add_buffer_barrier( visible_commands_sb, 0, VK_WHOLE_SIZE,
                            { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_WRITE_BIT } );

    cb->add_buffer_barrier( count_sb, 0, sizeof( GpuMeshDrawCounts ),
                            { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_READ_BIT |
                              VK_ACCESS_2_SHADER_WRITE_BIT } );

    cb->add_buffer_barrier( culling_handoff_sb, 0, sizeof( u32 ) * 4,
                            { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_READ_BIT |
                              VK_ACCESS_2_SHADER_WRITE_BIT } );

    cb->flush_barriers();

    cb->bind_pipeline( cull_pipeline.pipeline );

    cb->bind_descriptor_set(
        { renderer->gpu->bindless_descriptor_set, cull_descriptor_set[ current_frame_index ] },
        { render_blackboard.scene_cb_offset } );

    u32 group_x = raptor::ceilu32( render_scene->mesh_instances.size / 64.0f );
    cb->dispatch( group_x, 1, 1 );

    cb->add_buffer_barrier( visible_commands_sb, 0, VK_WHOLE_SIZE,
                            { VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                              VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT } );

    cb->add_buffer_barrier( count_sb, 0, VK_WHOLE_SIZE,
                            { VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                              VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT } );

    cb->add_buffer_barrier( culling_handoff_sb, 0, VK_WHOLE_SIZE,
                            { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_READ_BIT } );

    cb->flush_barriers();
}

void CullingEarlyPass::declare_frame_graph_node( FrameGraphResourceContext& context ) {
    FrameGraphBuilder& builder = *context.frame_graph->builder;

    ArenaAllocator* temp_allocator = MemoryService::instance()->get_thread_allocator();
    ArenaScope temp_scope( temp_allocator );

    Array<FrameGraphResourceCreation_v2> inputs;
    inputs.init( temp_allocator, 4 );

    // Optional input for animated meshlets.
    if ( context.render_config->enable_meshlet_animations ) {

        inputs.push( { 
            .type = FrameGraphResourceType_Buffer, 
            .handle = builder.get_output_handle( "animated_meshlet_pass", "meshlet_vertex_buffer" ) } );
    }

    context.frame_graph->add_node_v2( {
        .inputs = {
            inputs.as_cspan()
        },
        .outputs = {
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Buffer,
                .resource_info{
                    .buffer = {}
                },
                .name = "early_mesh_indirect_draw_list",
            } ),
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Buffer,
                .resource_info{
                    .buffer = {}
                },
                .name = "early_task_indirect_draw_list",
            } ),
        },
        .scheduling = { CommandQueueType::Graphics, 0 },
        .enabled = true,
        .compute = true,
        .name = "mesh_occlusion_early_pass" } );
}

ShaderCompilationCreation scc_culling_early = {
    .stages = {
        {
            .source_file_path = "glsl/culling.glsl",
            .type = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    },
    .name = "mesh_culling_early",
    .slang_input = 0,
};


ShaderCompilationCreation scc_culling_late = {
    .stages = {
        {
            .source_file_path = "glsl/culling.glsl",
            .type = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    },
    .name = "mesh_culling_late",
    .slang_input = 0,
};

void CullingEarlyPass::update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) {

    Renderer* renderer = context.renderer;

    if ( phase == PipelineUpdatePhase::Destroy ) {
        renderer->destroy_compute_pipeline_state( cull_pipeline );

        return;
    }

    ComputePipelineTransaction transaction( renderer );

    PipelineCreation pipeline_creation = {
        .name = "gpu_mesh_culling",
        .render_pass_name = "mesh_occlusion_early_pass",
    };

    ComputePipelineState& pipeline = transaction.add( cull_pipeline );

    renderer->create_compute_pipeline_state( scc_culling_early, pipeline_creation,
                                             "gpu_mesh_culling", context.frame_graph, pipeline );

    transaction.commit_or_rollback();
}

void CullingEarlyPass::create_gpu_resources( FrameGraphResourceContext& context ) {

    FrameGraph* frame_graph = context.frame_graph;

    FrameGraphNode* node = frame_graph->get_node( "mesh_occlusion_early_pass" );
    if ( node == nullptr ) {
        enabled = false;

        return;
    }

    enabled = node->enabled;

    RenderBlackboard& render_blackboard = *context.render_blackboard;
    Renderer* renderer = context.renderer;
    GpuDevice& gpu = *renderer->gpu;
    GpuCullingRuntimeData& gpu_culling = render_blackboard.gpu_culling;

    // Cache frustum cull shader
    DescriptorSetLayoutHandle layout = gpu.get_descriptor_set_layout( cull_pipeline.pipeline, k_material_descriptor_set_index );
    DescriptorSetBinder descriptors;

    ShaderReflectionInfo* shader_reflection = renderer->get_shader_reflection( cull_pipeline.pipeline );

    for ( u32 i = 0; i < k_max_frames; ++i ) {
        descriptors.reset();
        descriptors.name = "culling_early_ds";
        descriptors.ssbos.push( { gpu_culling.meshlet_indirect_early_count_sb[ i ], 15 } );
        descriptors.ssbos.push( { gpu_culling.meshlet_culling_handoff_sb[ i ], 16 } );
        descriptors.ssbos.push( { gpu_culling.meshlet_indirect_early_commands_sb[ i ], 1 } );
        descriptors.ssbos.push( { gpu_culling.meshlet_culled_mesh_instance_ids_sb[ i ], 3 } );

        cull_descriptor_set[ i ] = renderer->create_descriptor_set( descriptors, shader_reflection, cull_pipeline.pipeline, i, render_blackboard );
    }
}

void CullingEarlyPass::destroy_gpu_resources( FrameGraphResourceContext& context ) {

    Renderer* renderer = context.renderer;

    for ( u32 i = 0; i < k_max_frames; ++i ) {
        renderer->gpu->destroy_descriptor_set( cull_descriptor_set[ i ] );
    }
}

// CullingLatePass /////////////////////////////////////////////////////////
void CullingLatePass::render( FrameGraphRenderContext& context ) {

    if ( !enabled )
        return;

    RenderScene* render_scene = context.render_view->scene;
    u32 current_frame_index = context.current_frame_index;
    CommandBuffer* cb = context.gpu_commands;
    Renderer* renderer = context.renderer;
    RenderBlackboard& render_blackboard = *context.render_blackboard;
    GpuCullingRuntimeData& gpu_culling = render_blackboard.gpu_culling;

    // Frustum cull meshes
    GpuMeshDrawCounts& mesh_draw_counts = render_scene->mesh_draw_counts;
    mesh_draw_counts.opaque_mesh_visible_count = 0;
    mesh_draw_counts.opaque_mesh_culled_count = 0;
    mesh_draw_counts.transparent_mesh_visible_count = 0;
    mesh_draw_counts.transparent_mesh_culled_count = 0;
    mesh_draw_counts.late_flag = 1;

    mesh_draw_counts.total_count = render_scene->mesh_instances.size;

    FrameGraphResource* depth_pyramid_resource = context.frame_graph->get_resource( "depth_pyramid" );
    mesh_draw_counts.depth_pyramid_texture_index = depth_pyramid_resource->resource_info.texture.image_view.index();

    cb->update_buffer( gpu_culling.meshlet_indirect_late_count_sb[ current_frame_index ], 0, sizeof( GpuMeshDrawCounts ), &mesh_draw_counts );

    cb->bind_pipeline( cull_pipeline.pipeline );

    const Buffer* visible_commands_sb = renderer->gpu->get_buffer( gpu_culling.meshlet_indirect_late_commands_sb[ current_frame_index ] );
    const Buffer* count_sb = renderer->gpu->get_buffer( gpu_culling.meshlet_indirect_late_count_sb[ current_frame_index ] );
    cb->add_buffer_barrier( visible_commands_sb->handle, 0, VK_WHOLE_SIZE,
                            { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_WRITE_BIT } );

    cb->add_buffer_barrier( count_sb->handle, 0, VK_WHOLE_SIZE,
                            { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_WRITE_BIT } );

    cb->flush_barriers();

    cb->bind_descriptor_set(
        { renderer->gpu->bindless_descriptor_set, cull_descriptor_set[ current_frame_index ] },
        { render_blackboard.scene_cb_offset } );

    u32 group_x = raptor::ceilu32( render_scene->mesh_instances.size / 64.0f );
    cb->dispatch( group_x, 1, 1 );

    cb->add_buffer_barrier( visible_commands_sb->handle, 0, VK_WHOLE_SIZE,
                            { VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                              VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT } );

    cb->add_buffer_barrier( count_sb->handle, 0, VK_WHOLE_SIZE,
                            { VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                              VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT } );

    cb->flush_barriers();
}

void CullingLatePass::declare_frame_graph_node( FrameGraphResourceContext& context ) {
    FrameGraphBuilder& builder = *context.frame_graph->builder;

    context.frame_graph->add_node_v2( {
        .inputs = {
            {
                .type = FrameGraphResourceType_Attachment,
                .handle = builder.get_output_handle( "depth_pyramid_pass", "depth_pyramid" )
            }
        },
        .outputs = {
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Buffer,
                .resource_info{
                    .buffer = {}
                },
                .name = "late_mesh_indirect_draw_list",
            } ),
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Buffer,
                .resource_info{
                    .buffer = {}
                },
                .name = "late_task_indirect_draw_list",
            } ),
        },
        .scheduling = { CommandQueueType::Graphics, 0 },
        .enabled = true,
        .compute = true,
        .name = "mesh_occlusion_late_pass" } );
}

void CullingLatePass::update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) {

    Renderer* renderer = context.renderer;

    if ( phase == PipelineUpdatePhase::Destroy ) {
        renderer->destroy_compute_pipeline_state( cull_pipeline );

        return;
    }

    ComputePipelineTransaction transaction( renderer );

    PipelineCreation pipeline_creation = {
        .name = "gpu_mesh_culling_late",
        .render_pass_name = "mesh_occlusion_late_pass",
    };

    ComputePipelineState& pipeline = transaction.add( cull_pipeline );

    renderer->create_compute_pipeline_state( scc_culling_late, pipeline_creation,
                                             "gpu_mesh_culling", context.frame_graph, pipeline );

    transaction.commit_or_rollback();
}

void CullingLatePass::create_gpu_resources( FrameGraphResourceContext& context ) {

    FrameGraph* frame_graph = context.frame_graph;
    RenderScene* scene = context.render_scene;

    FrameGraphNode* node = frame_graph->get_node( "mesh_occlusion_late_pass" );
    if ( node == nullptr ) {
        enabled = false;

        return;
    }

    enabled = node->enabled;

    Renderer* renderer = context.renderer;
    GpuDevice& gpu = *renderer->gpu;
    RenderBlackboard& render_blackboard = *context.render_blackboard;
    GpuCullingRuntimeData& gpu_culling = render_blackboard.gpu_culling;

    DescriptorSetBinder descriptors;

    ShaderReflectionInfo* shader_reflection = renderer->get_shader_reflection( cull_pipeline.pipeline );

    for ( u32 i = 0; i < k_max_frames; ++i ) {
        descriptors.reset();

        descriptors.ssbos.push( { gpu_culling.meshlet_indirect_late_count_sb[ i ], 15 } );
        descriptors.ssbos.push( { gpu_culling.meshlet_culling_handoff_sb[ i ], 16 } );
        descriptors.ssbos.push( { gpu_culling.meshlet_indirect_late_commands_sb[ i ], 1 } );
        descriptors.ssbos.push( { gpu_culling.meshlet_culled_mesh_instance_ids_sb[ i ], 3 } );

        descriptors.name = "culling_late_ds";
        cull_descriptor_set[ i ] = renderer->create_descriptor_set( descriptors, shader_reflection, cull_pipeline.pipeline, i, render_blackboard );
    }
}

void CullingLatePass::destroy_gpu_resources( FrameGraphResourceContext& context ) {

    Renderer* renderer = context.renderer;

    for ( u32 i = 0; i < k_max_frames; ++i ) {
        renderer->gpu->destroy_descriptor_set( cull_descriptor_set[ i ] );
    }
}

} // namespace raptor
