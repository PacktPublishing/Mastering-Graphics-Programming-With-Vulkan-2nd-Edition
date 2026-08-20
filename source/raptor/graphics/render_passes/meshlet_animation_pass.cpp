
#include "graphics/render_passes/meshlet_animation_pass.hpp"
#include "graphics/render_scene.hpp"
#include "graphics/render_blackboard.hpp"

#include "foundation/numerics.hpp"

namespace raptor {

// MeshletAnimationPass /////////////////////////////////////////////////////////
void MeshletAnimationPass::render( FrameGraphRenderContext& context ) {

    if ( !enabled )
        return;

    RenderScene* render_scene = context.render_view->scene;
    u32 current_frame_index = context.current_frame_index;
    CommandBuffer* cb = context.gpu_commands;
    Renderer* renderer = context.renderer;
    RenderBlackboard& render_blackboard = *context.render_blackboard;
    MeshletsRuntimeData& meshlets = render_blackboard.meshlets;

    cb->add_buffer_barrier( meshlets.meshlets_vertex_pos_sb_gpu[ current_frame_index ], 0, VK_WHOLE_SIZE,
                           { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             VK_ACCESS_2_SHADER_WRITE_BIT } );

    cb->add_buffer_barrier( meshlets.meshlets_vertex_data_sb_gpu[ current_frame_index ], 0, VK_WHOLE_SIZE,
                           { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             VK_ACCESS_2_SHADER_WRITE_BIT } );

    cb->flush_barriers();

    cb->bind_pipeline( vertex_update_pipeline.pipeline );

    for ( u32 i = 0; i < animated_mesh_indices.size; ++i ) {
        u32 mesh_index = animated_mesh_indices[ i ];
        Mesh& mesh = render_scene->meshes[ mesh_index ];

        cb->push_constants( vertex_update_pipeline.pipeline, 0, 4, &mesh_index );
        cb->bind_descriptor_set( { renderer->gpu->bindless_descriptor_set, vertex_update_descriptor_set[ current_frame_index ][ mesh.skin_index ] },
                                  { render_blackboard.scene_cb_offset } );

        cb->dispatch( (mesh.position_count + 63) / 64, 1, 1 );
    }

    cb->add_buffer_barrier( meshlets.meshlets_vertex_pos_sb_gpu[ current_frame_index ], 0, VK_WHOLE_SIZE,
                    { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_READ_BIT } );

    cb->add_buffer_barrier( meshlets.meshlets_vertex_data_sb_gpu[ current_frame_index ], 0, VK_WHOLE_SIZE,
                           { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             VK_ACCESS_2_SHADER_READ_BIT } );

    cb->add_buffer_barrier( meshlets.meshlets_sb_gpu[ current_frame_index ], 0, VK_WHOLE_SIZE,
                           { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT } );

    cb->flush_barriers();

    cb->bind_pipeline( meshlet_update_pipeline.pipeline );

    for ( u32 i = 0; i < animated_mesh_indices.size; ++i ) {
        u32 mesh_index = animated_mesh_indices[ i ];
        Mesh& mesh = render_scene->meshes[ mesh_index ];

        cb->push_constants( meshlet_update_pipeline.pipeline, 0, 4, &mesh_index );
        cb->bind_descriptor_set( { renderer->gpu->bindless_descriptor_set, meshlet_update_descriptor_set[ current_frame_index ] },
                                  { render_blackboard.scene_cb_offset } );

        cb->dispatch( mesh.meshlet_count, 1, 1 );
    }

    cb->add_buffer_barrier( meshlets.meshlets_sb_gpu[ current_frame_index ], 0, VK_WHOLE_SIZE,
                           { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             VK_ACCESS_2_SHADER_READ_BIT } );

    cb->flush_barriers();
}

void MeshletAnimationPass::declare_frame_graph_node( FrameGraphResourceContext& context ) {
    FrameGraphBuilder& builder = *context.frame_graph->builder;

    context.frame_graph->add_node_v2( {
        .outputs = {
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Buffer,
                .resource_info{
                    .buffer = {}
                },
                .name = "meshlet_vertex_buffer",
            } ),
        },
        .scheduling = { CommandQueueType::Graphics, 0 },
        .enabled = true,
        .compute = true,
        .name = "animated_meshlet_pass" } );
}

ShaderCompilationCreation scc_vertex_update = {
    .stages = {
        {
            .source_file_path = "glsl/animated_meshlet.glsl",
            .type = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    },
    .name = "animated_meshlet_vertex_update",
    .slang_input = 0,
};


ShaderCompilationCreation scc_meshlet_update = {
    .stages = {
        {
            .source_file_path = "glsl/animated_meshlet.glsl",
            .type = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    },
    .name = "animated_meshlet_update",
    .slang_input = 0,
};

void MeshletAnimationPass::update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) {

    Renderer* renderer = context.renderer;

    if ( phase == PipelineUpdatePhase::Destroy ) {
        renderer->destroy_compute_pipeline_state( vertex_update_pipeline );
        renderer->destroy_compute_pipeline_state( meshlet_update_pipeline );

        return;
    }

    ComputePipelineTransaction transaction( renderer );

    PipelineCreation vertex_update_pipeline_creation = {
        .name = "animated_meshlet_vertex_update",
        .render_pass_name = "animated_meshlet_pass",
    };

    PipelineCreation meshlet_update_pipeline_creation = {
        .name = "animated_meshlet_update",
        .render_pass_name = "animated_meshlet_pass",
    };

    ComputePipelineState& vertex_update_pipeline_state = transaction.add( vertex_update_pipeline );

    renderer->create_compute_pipeline_state( scc_vertex_update, vertex_update_pipeline_creation,
                                             "animated_meshlet_vertex_update", context.frame_graph, vertex_update_pipeline_state );

    ComputePipelineState& meshlet_update_pipeline_state = transaction.add( meshlet_update_pipeline );

    renderer->create_compute_pipeline_state( scc_meshlet_update, meshlet_update_pipeline_creation,
                                             "animated_meshlet_update", context.frame_graph, meshlet_update_pipeline_state );

    transaction.commit_or_rollback();
}

void MeshletAnimationPass::create_gpu_resources( FrameGraphResourceContext& context ) {

    FrameGraph* frame_graph = context.frame_graph;

    FrameGraphNode* node = frame_graph->get_node( "animated_meshlet_pass" );
    if ( node == nullptr ) {
        enabled = false;

        return;
    }

    enabled = node->enabled;

    RenderBlackboard& render_blackboard = *context.render_blackboard;
    Renderer* renderer = context.renderer;
    GpuDevice& gpu = *renderer->gpu;
    GpuCullingRuntimeData& gpu_culling = render_blackboard.gpu_culling;
    MeshletsRuntimeData& meshlets_data = render_blackboard.meshlets;
    MeshRuntimeData& geometry_data = render_blackboard.geometry_data;

    RenderScene* render_scene = context.render_scene;
    animated_mesh_indices.init( gpu.allocator, 8 );
    for ( Mesh& mesh : render_scene->meshes ) {
        if ( mesh.has_skinning() ) {
            animated_mesh_indices.push( mesh.gpu_mesh_index );
        }
    }

    /*BufferCreation bc = { .type_flags = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .usage = ResourceUsageType::Dynamic,
                          .size = (u32)( sizeof( u32 ) * animated_mesh_indices.size ), .initial_data = animated_mesh_indices.data,
                          .name = "animated_mesh_indices_sb" };

    mesh_list_buffer = gpu.create_buffer( bc );*/

    // Cache frustum cull shader
    DescriptorSetLayoutHandle layout = gpu.get_descriptor_set_layout( vertex_update_pipeline.pipeline, k_material_descriptor_set_index );
    DescriptorSetBinder descriptors;

    ShaderReflectionInfo* shader_reflection = renderer->get_shader_reflection( vertex_update_pipeline.pipeline );

    for ( u32 i = 0; i < k_max_frames; ++i ) {
        vertex_update_descriptor_set[ i ].init( gpu.allocator, render_scene->skins.size, render_scene->skins.size );
        for ( u32 s = 0; s < render_scene->skins.size; ++s ) {
            Skin& skin = render_scene->skins[ s ];

            descriptors.reset();
            descriptors.name = "meshlet_vertex_update_ds";
            descriptors.ssbos.push( { geometry_data.position_buffer_gpu, 6 } );
            descriptors.ssbos.push( { geometry_data.joints_buffer_gpu, 7 } );
            descriptors.ssbos.push( { geometry_data.weights_buffer_gpu, 8 } );
            descriptors.ssbos.push( { meshlets_data.meshlets_vertex_pos_sb_gpu[ i ], 9 } );
            descriptors.ssbos.push( { skin.joint_transforms[ i ], 10 } );
            descriptors.ssbos.push( { meshlets_data.meshlets_vertex_data_sb_gpu[ i ], 11});
            descriptors.ssbos.push( { gpu_culling.meshlet_indirect_early_count_sb[ i ], 15 } );

            vertex_update_descriptor_set[ i ][ s ] = renderer->create_descriptor_set( descriptors, shader_reflection,
                vertex_update_pipeline.pipeline, i, render_blackboard );
        }

        descriptors.reset();
        descriptors.name = "meshlet_animation_update_ds";
        descriptors.ssbos.push( { meshlets_data.meshlets_vertex_pos_sb_gpu[ i ], 9 } );
        descriptors.ssbos.push( { gpu_culling.meshlet_indirect_early_count_sb[ i ], 15 } );

        meshlet_update_descriptor_set[ i ] = renderer->create_descriptor_set( descriptors, shader_reflection,
            meshlet_update_pipeline.pipeline, i, render_blackboard );
    }
}

void MeshletAnimationPass::destroy_gpu_resources( FrameGraphResourceContext& context ) {

    Renderer* renderer = context.renderer;
    RenderScene* render_scene = context.render_scene;

    for ( u32 i = 0; i < k_max_frames; ++i ) {
        for ( u32 s = 0; s < render_scene->skins.size; ++s ) {
            renderer->gpu->destroy_descriptor_set( vertex_update_descriptor_set[ i ][ s ] );
        }
        vertex_update_descriptor_set[ i ].shutdown();
        renderer->gpu->destroy_descriptor_set( meshlet_update_descriptor_set[ i ] );

    }
    //renderer->gpu->destroy_buffer( mesh_list_buffer );

    animated_mesh_indices.shutdown();
}

} // namespace raptor
