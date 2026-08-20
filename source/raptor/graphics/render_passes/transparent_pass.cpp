
#include "graphics/render_passes/transparent_pass.hpp"
#include "graphics/render_scene.hpp"
#include "graphics/render_blackboard.hpp"

#include "graphics/render_passes/meshlet_pipelines.hpp"
#include "graphics/render_passes/mesh_vertex_inputs.hpp"

namespace raptor {

//
// TransparentPass ////////////////////////////////////////////////////////
void TransparentPass::declare_frame_graph_node( FrameGraphResourceContext& context ) {

    FrameGraphBuilder& builder = *context.frame_graph->builder;

    context.frame_graph->add_node_v2( {
        .inputs = {
            {
                .type = FrameGraphResourceType_Attachment,
                .handle = builder.get_output_handle( "lighting_pass", "final" )
            },
            {
                .type = FrameGraphResourceType_Attachment,
                .handle = builder.get_output_handle( "gbuffer_pass_early", "depth" )
            },
        },
        .outputs = {
            builder.create_output_reference(
                builder.get_output_handle( "lighting_pass", "final" ),
                FrameGraphResourceType_Reference )
        },
        .scheduling = { CommandQueueType::Graphics, 1 },
        .enabled = true,
        .name = "transparent_pass" } );
}

void TransparentPass::update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) {

    Renderer* renderer = context.renderer;

    if ( phase == PipelineUpdatePhase::Destroy ) {
        renderer->destroy_graphics_pipeline_state( meshlet_draw_pipeline );

        return;
    }

    GraphicsPipelineTransaction transaction( renderer );

    GraphicsPipelineState& new_pipeline = transaction.add( meshlet_draw_pipeline );
    renderer->create_graphics_pipeline_state( scc_meshlet_transparent_no_cull, pc_meshlet_transparent_no_cull,
                                              "meshlet", context.frame_graph, new_pipeline );

    transaction.commit_or_rollback();
}

void TransparentPass::render( FrameGraphRenderContext& context ) {
    if ( !enabled )
        return;

    RenderScene* render_scene = context.render_view->scene;
    u32 current_frame_index = context.current_frame_index;
    CommandBuffer* gpu_commands = context.gpu_commands;
    RenderBlackboard& render_blackboard = *context.render_blackboard;
    Renderer* renderer = render_scene->renderer;

    if ( context.render_config->meshlets.use_meshlets_emulation ) {
        // TODO:
    } else if ( context.render_config->meshlets.use_meshlets ) {
        gpu_commands->set_depth_bias_enabled( false );
        gpu_commands->bind_pipeline( meshlet_draw_pipeline.pipeline );

        gpu_commands->bind_descriptor_set(
            { renderer->gpu->bindless_descriptor_set, render_blackboard.meshlets.meshlets_transparent_draw_descriptor_set[ current_frame_index ] },
            { render_blackboard.scene_cb_offset, render_blackboard.lighting.lighting_constants_cb_offset } );

        // Transparent commands are put after mesh instances count commands.
        const u32 indirect_commands_offset = offsetof( GpuMeshDrawCommand, indirectMS ) + sizeof( GpuMeshDrawCommand ) * render_scene->mesh_instances.size;
        // Transparent count is after opaque and total count offset.
        const u32 indirect_count_offset = sizeof( u32 ) * 2;

        gpu_commands->draw_mesh_task_indirect_count( render_blackboard.gpu_culling.meshlet_indirect_early_commands_sb[ current_frame_index ], indirect_commands_offset,
                                                     render_blackboard.gpu_culling.meshlet_indirect_early_count_sb[ current_frame_index ], indirect_count_offset, render_scene->mesh_instances.size, sizeof( GpuMeshDrawCommand ) );


    } else {
        /*Material* last_material = nullptr;
        for ( u32 mesh_index = 0; mesh_index < mesh_instance_draws.size; ++mesh_index ) {
            MeshInstanceDraw& mesh_instance_draw = mesh_instance_draws[ mesh_index ];
            Mesh& mesh = render_scene->meshes[ mesh_instance_draw.mesh_instance->mesh_index ];

            if ( mesh.pbr_material.material != last_material ) {
                PipelineHandle pipeline = renderer->get_pipeline( mesh.pbr_material.material, mesh_instance_draw.material_pass_index );

                gpu_commands->bind_pipeline( pipeline );

                last_material = mesh.pbr_material.material;
            }

            render_scene->draw_mesh_instance( gpu_commands, *mesh_instance_draw.mesh_instance, true );
        }*/
    }
}

void TransparentPass::create_gpu_resources( FrameGraphResourceContext& context ) {

    FrameGraph* frame_graph = context.frame_graph;
    RenderScene* scene = context.render_scene;

    FrameGraphNode* node = frame_graph->get_node( "transparent_pass" );
    if ( node == nullptr ) {
        enabled = false;

        return;
    }

    enabled = node->enabled;
    if ( !enabled )
        return;

    const u64 hashed_name = hash_calculate( "main" );
    //GpuTechnique* main_technique = renderer->resource_cache.techniques.get( hashed_name );

    // TODO: material removal
    //mesh_instance_draws.init( resident_allocator, 16 );

    //for ( u32 i = 0; i < scene.mesh_instances.size; ++i ) {

    //    MeshInstance& mesh_instance = scene.mesh_instances[ i ];
    //    Mesh& mesh = scene.meshes[ mesh_instance.mesh_index ];
    //    if ( !mesh.is_transparent() ) {
    //        continue;
    //    }

    //    MeshInstanceDraw mesh_instance_draw{};
    //    mesh_instance_draw.mesh_instance = &mesh_instance;
    //    mesh_instance_draw.material_pass_index = mesh.has_skinning() ? main_technique->get_pass_index( "transparent_skinning_no_cull" ) : main_technique->get_pass_index( "transparent_no_cull" );

    //    mesh_instance_draws.push( mesh_instance_draw );
    //}

    //// Cache meshlet technique index
    //if ( renderer->gpu->mesh_shaders_extension_present ) {
    //    GpuTechnique* main_technique = renderer->resource_cache.techniques.get( hash_calculate( "meshlet" ) );
    //    meshlet_technique_index = main_technique->get_pass_index( "transparent_no_cull" );
    //}
}

void TransparentPass::destroy_gpu_resources( FrameGraphResourceContext& context ) {
    if ( !enabled )
        return;

    //mesh_instance_draws.shutdown();
}

} // namespace raptor
