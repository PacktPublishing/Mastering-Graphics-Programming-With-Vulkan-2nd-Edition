#include "graphics/render_passes/gbuffer_pass.hpp"
#include "graphics/render_scene.hpp"
#include "graphics/render_blackboard.hpp"

#include "graphics/render_passes/meshlet_pipelines.hpp"
#include "graphics/render_passes/mesh_vertex_inputs.hpp"
#include "graphics/render_passes/mesh_pipelines.hpp"

namespace raptor {

//
// GBufferPass ////////////////////////////////////////////////////////
void GBufferPass::declare_frame_graph_node( FrameGraphResourceContext& context ) {
    FrameGraphBuilder& builder = *context.frame_graph->builder;

    context.frame_graph->add_node_v2( {
        .inputs = {
            {
                .type = FrameGraphResourceType_Buffer,
                .handle = builder.get_output_handle( "mesh_occlusion_early_pass", "early_mesh_indirect_draw_list" )
            },
            {
                .type = FrameGraphResourceType_Buffer,
                .handle = builder.get_output_handle( "mesh_occlusion_early_pass", "early_task_indirect_draw_list" )
            }
        },
        .outputs = {
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Attachment,
                .resource_info{
                    .texture = {
                        .scale_width = 1.0f,
                        .scale_height = 1.0f,
                        .format = VK_FORMAT_B8G8R8A8_UNORM,
                        .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR
                    }
                },
                .name = "gbuffer_colour",
            } ),
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Attachment,
                .resource_info{
                    .texture = {
                        .scale_width = 1.0f,
                        .scale_height = 1.0f,
                        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                        .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR
                    }
                },
                .name = "gbuffer_normals",
            } ),
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Attachment,
                .resource_info{
                    .texture = {
                        .scale_width = 1.0f,
                        .scale_height = 1.0f,
                        .format = VK_FORMAT_B8G8R8A8_UNORM,
                        .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR
                    }
                },
                .name = "gbuffer_metallic_roughness_occlusion",
            } ),
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Attachment,
                .resource_info{
                    .texture = {
                        .scale_width = 1.0f,
                        .scale_height = 1.0f,
                        .format = VK_FORMAT_B10G11R11_UFLOAT_PACK32,
                        .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR
                    }
                },
                .name = "gbuffer_emissive",
            } ),
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Attachment,
                .resource_info{
                    .texture = {
                        .scale_width = 1.0f,
                        .scale_height = 1.0f,
                        .format = VK_FORMAT_R32_UINT,
                        .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR
                    }
                },
                .name = "mesh_id",
            } ),
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Attachment,
                .resource_info{
                    .texture = {
                        .scale_width = 1.0f,
                        .scale_height = 1.0f,
                        .format = VK_FORMAT_R16G16_SFLOAT,
                        .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR
                    }
                },
                .name = "z_normal_fwidth",
            } ),
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Attachment,
                .resource_info{
                    .texture = {
                        .scale_width = 1.0f,
                        .scale_height = 1.0f,
                        .format = VK_FORMAT_R16G16_SFLOAT,
                        .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR
                    }
                },
                .name = "linear_z_dd",
            } ),
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Attachment,
                .resource_info{
                    .texture = {
                        .scale_width = 1.0f,
                        .scale_height = 1.0f,
                        .format = VK_FORMAT_D32_SFLOAT,
                        .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
                        .clear_values = { 1.0f, 0.0f, 0.0f, 0.0f },
                        .persistent = true
                    }
                },
                .name = "depth",
            } ),
        },
        .scheduling = { CommandQueueType::Graphics, 0 },
        .enabled = true,
        .name = "gbuffer_pass_early", } );
}

void GBufferPass::update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) {

    Renderer* renderer = context.renderer;

    if ( phase == PipelineUpdatePhase::Destroy ) {
        renderer->destroy_graphics_pipeline_state( meshlet_draw_pipeline );
        renderer->destroy_graphics_pipeline_state( meshlet_draw_pipeline_slang );
        renderer->destroy_graphics_pipeline_state( skinning_pipeline );

        return;
    }

    GraphicsPipelineTransaction transaction( renderer );

    GraphicsPipelineState& new_pipeline = transaction.add( meshlet_draw_pipeline );
    GraphicsPipelineState& new_pipeline_slang = transaction.add( meshlet_draw_pipeline_slang );
    GraphicsPipelineState& new_pipeline_skinning = transaction.add( skinning_pipeline );

    renderer->create_graphics_pipeline_state( scc_meshlet_gbuffer_culling, pc_meshlet_gbuffer_culling,
                                              "meshlet", context.frame_graph, new_pipeline );

    renderer->create_graphics_pipeline_state( scc_meshlet_gbuffer_culling_slang, pc_meshlet_gbuffer_culling_slang,
                                              "meshlet_slang", context.frame_graph, new_pipeline_slang );

    renderer->create_graphics_pipeline_state( scc_mesh_gbuffer_skinning, pc_mesh_gbuffer_skinning,
                                              "gbuffer_skinning", context.frame_graph, new_pipeline_skinning );

    transaction.commit_or_rollback();
}

void GBufferPass::pre_render( FrameGraphRenderContext& context ) {

    Renderer* renderer = context.renderer;
    GpuDevice* gpu = renderer->gpu;

    RenderScene* render_scene = context.render_view->scene;
    u32 current_frame_index = context.current_frame_index;
    CommandBuffer* gpu_commands = context.gpu_commands;
    RenderBlackboard& render_blackboard = *context.render_blackboard;

    if ( context.render_config->meshlets.use_meshlets_emulation ) {

        // TODO: remove
        gpu_commands->global_debug_barrier();

        // Generate meshlet list
        gpu_commands->bind_pipeline( generate_meshlets_instances_pipeline );
        gpu_commands->bind_descriptor_set(
            { renderer->gpu->bindless_descriptor_set, generate_meshlets_instances_descriptor_set[ current_frame_index ] },
            { render_blackboard.scene_cb_offset } );
        gpu_commands->dispatch( ( render_scene->mesh_instances.size + 31 ) / 32, 1, 1 );

        // TODO: remove
        gpu_commands->global_debug_barrier();

        // Cull visible meshlets
        gpu_commands->bind_pipeline( meshlet_instance_culling_pipeline );
        gpu_commands->bind_descriptor_set(
            { renderer->gpu->bindless_descriptor_set, meshlet_instance_culling_descriptor_set[ current_frame_index ] },
            { render_blackboard.scene_cb_offset } );
        gpu_commands->dispatch_indirect( render_blackboard.meshlet_emulation_instances_indirect_count_sb[ current_frame_index ], 0 );

        // TODO: remove
        gpu_commands->global_debug_barrier();

        // Write counts
        gpu_commands->bind_pipeline( meshlet_write_counts_pipeline );
        gpu_commands->bind_descriptor_set(
            { renderer->gpu->bindless_descriptor_set, meshlet_instance_culling_descriptor_set[ current_frame_index ] },
            { render_blackboard.scene_cb_offset } );
        gpu_commands->dispatch( 1, 1, 1 );

        // TODO: remove
        gpu_commands->global_debug_barrier();

        // Generate index buffer
        BufferHandle meshlet_index_buffer = render_blackboard.meshlets.meshlets_emulation_index_buffer_sb[ current_frame_index ];

        //gpu_commands->issue_buffer_barrier( meshlet_index_buffer, RESOURCE_STATE_INDEX_BUFFER, RESOURCE_STATE_UNORDERED_ACCESS, QueueType::Graphics, QueueType::Compute );

        gpu_commands->bind_pipeline( generate_meshlet_index_buffer_pipeline );
        gpu_commands->bind_descriptor_set(
            { renderer->gpu->bindless_descriptor_set, generate_meshlet_index_buffer_descriptor_set[ current_frame_index ] },
            { render_blackboard.scene_cb_offset } );
        gpu_commands->dispatch_indirect( generate_meshlet_dispatch_indirect_buffer[ current_frame_index ], offsetof( GpuMeshDrawCounts, dispatch_task_x ) );

        //gpu_commands->issue_buffer_barrier( meshlet_index_buffer, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_INDEX_BUFFER, QueueType::Compute, QueueType::Graphics );

        gpu_commands->global_debug_barrier();
    }
}

void GBufferPass::render( FrameGraphRenderContext& context ) {
    if ( !enabled )
        return;

    RenderScene* render_scene = context.render_view->scene;
    u32 current_frame_index = context.current_frame_index;
    CommandBuffer* gpu_commands = context.gpu_commands;
    Renderer* renderer = context.renderer;

    RenderBlackboard& render_blackboard = *context.render_blackboard;
    MeshletsRuntimeData& meshlets = render_blackboard.meshlets;
    GpuCullingRuntimeData& gpu_culling = render_blackboard.gpu_culling;

    gpu_commands->set_depth_bias_enabled( false );

    if ( context.render_config->meshlets.use_meshlets_emulation ) {

        gpu_commands->bind_pipeline( meshlet_emulation_draw_pipeline );

        gpu_commands->bind_descriptor_set(
            { renderer->gpu->bindless_descriptor_set, meshlets.meshlets_emulation_draw_descriptor_set[ current_frame_index ] },
            { render_blackboard.scene_cb_offset } );

        gpu_commands->bind_index_buffer( meshlets.meshlets_emulation_index_buffer_sb[ current_frame_index ], 0, VK_INDEX_TYPE_UINT32 );
        gpu_commands->draw_indexed_indirect( gpu_culling.meshlet_indirect_early_commands_sb[ current_frame_index ], 1, offsetof( GpuMeshDrawCommand, indirect ), sizeof( GpuMeshDrawCommand ) );
    } else if ( context.render_config->meshlets.use_meshlets ) {

        if ( context.render_config->use_slang_shaders ) {
            gpu_commands->bind_pipeline( meshlet_draw_pipeline_slang.pipeline );

            gpu_commands->bind_descriptor_set(
                { renderer->gpu->bindless_descriptor_set, meshlets.meshlets_early_draw_descriptor_set_slang[ current_frame_index ] },
                { render_blackboard.scene_cb_offset } );
        }
        else
        {
            gpu_commands->bind_pipeline( meshlet_draw_pipeline.pipeline );

            gpu_commands->bind_descriptor_set(
                { renderer->gpu->bindless_descriptor_set, meshlets.meshlets_early_draw_descriptor_set[ current_frame_index ] },
                { render_blackboard.scene_cb_offset } );
        }

        gpu_commands->draw_mesh_task_indirect_count(
            gpu_culling.meshlet_indirect_early_commands_sb[ current_frame_index ], offsetof( GpuMeshDrawCommand, indirectMS ),
            gpu_culling.meshlet_indirect_early_count_sb[ current_frame_index ], 0,
            render_scene->mesh_instances.size, sizeof( GpuMeshDrawCommand ) );
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

            render_scene->draw_mesh_instance( gpu_commands, *mesh_instance_draw.mesh_instance, false );
        }*/
    }

#if 0
    RenderView* render_view = context.render_view;

    gpu_commands->bind_pipeline( skinning_pipeline.pipeline );

    for ( RenderItem& render_mesh : render_view->opaque_items ) {
        MeshInstance* mesh_instance = render_mesh.mesh_instance;
        Mesh& mesh = render_scene->meshes[ mesh_instance->mesh_index ];

        if ( !mesh.has_skinning() ) {
            continue;
        }

        gpu_commands->bind_vertex_buffer( mesh.position_buffer, 0, mesh.position_offset );

        if ( mesh.tangent_buffer.is_valid() ) {
            gpu_commands->bind_vertex_buffer( mesh.tangent_buffer, 1, mesh.tangent_offset );
        } else {
            gpu_commands->bind_vertex_buffer( renderer->gpu->dummy_vertex_buffer, 1, 0 );
        }

        gpu_commands->bind_vertex_buffer( mesh.normal_buffer, 2, mesh.normal_offset );

        if ( mesh.texcoord_buffer.is_valid() ) {
            gpu_commands->bind_vertex_buffer( mesh.texcoord_buffer, 3, mesh.texcoord_offset );
        } else {
            gpu_commands->bind_vertex_buffer( renderer->gpu->dummy_vertex_buffer, 3, 0 );
        }

        RASSERT( mesh.joints_buffer.is_valid() && mesh.weights_buffer.is_valid() );
        gpu_commands->bind_vertex_buffer( mesh.joints_buffer, 4, mesh.joints_offset );
        gpu_commands->bind_vertex_buffer( mesh.weights_buffer, 5, mesh.weights_offset );

        gpu_commands->bind_index_buffer( mesh.index_buffer, mesh.index_offset, mesh.index_type );

        gpu_commands->bind_descriptor_set( { renderer->gpu->bindless_descriptor_set,
            meshlets.skinning_descriptor_set[ current_frame_index ][ mesh.skin_index ] }, { render_blackboard.scene_cb_offset } );

        gpu_commands->draw_indexed( raptor::TopologyType::Triangle, mesh.index_count, 1, 0, 0, mesh_instance->gpu_mesh_instance_index );
    }

    // TODO(marco): move this to transparent pass
    for ( RenderItem& render_mesh : render_view->transparent_items ) {
        MeshInstance* mesh_instance = render_mesh.mesh_instance;
        Mesh& mesh = render_scene->meshes[ mesh_instance->mesh_index ];

        if ( !mesh.has_skinning() ) {
            continue;
        }

        gpu_commands->bind_vertex_buffer( mesh.position_buffer, 0, mesh.position_offset );

        if ( mesh.tangent_buffer.is_valid() ) {
            gpu_commands->bind_vertex_buffer( mesh.tangent_buffer, 1, mesh.tangent_offset );
        } else {
            gpu_commands->bind_vertex_buffer( renderer->gpu->dummy_vertex_buffer, 1, 0 );
        }

        gpu_commands->bind_vertex_buffer( mesh.normal_buffer, 2, mesh.normal_offset );

        if ( mesh.texcoord_buffer.is_valid() ) {
            gpu_commands->bind_vertex_buffer( mesh.texcoord_buffer, 3, mesh.texcoord_offset );
        } else {
            gpu_commands->bind_vertex_buffer( renderer->gpu->dummy_vertex_buffer, 3, 0 );
        }

        RASSERT( mesh.joints_buffer.is_valid() && mesh.weights_buffer.is_valid() );
        gpu_commands->bind_vertex_buffer( mesh.joints_buffer, 4, mesh.joints_offset );
        gpu_commands->bind_vertex_buffer( mesh.weights_buffer, 5, mesh.weights_offset );

        gpu_commands->bind_index_buffer( mesh.index_buffer, mesh.index_offset, mesh.index_type );

        gpu_commands->bind_descriptor_set( { renderer->gpu->bindless_descriptor_set,
            meshlets.skinning_descriptor_set[ current_frame_index ][ mesh.skin_index ] }, { render_blackboard.scene_cb_offset } );

        gpu_commands->draw_indexed( raptor::TopologyType::Triangle, mesh.index_count, 1, 0, 0, mesh_instance->gpu_mesh_instance_index );
    }
#endif
}

void GBufferPass::create_gpu_resources( FrameGraphResourceContext& context ) {
    FrameGraph* frame_graph = context.frame_graph;
    RenderScene* scene = context.render_scene;

    FrameGraphNode* node = frame_graph->get_node( "gbuffer_pass_early" );
    if ( node == nullptr ) {
        enabled = false;

        return;
    }

    enabled = node->enabled;
    if ( !enabled )
        return;

    // TODO: material removal
    //const u64 hashed_name = hash_calculate( "main" );
    //GpuTechnique* main_technique = renderer->resource_cache.techniques.get( hashed_name );

    //mesh_instance_draws.init( resident_allocator, 16 );

    //// Copy all mesh draws and change only material.
    //for ( u32 i = 0; i < scene.mesh_instances.size; ++i ) {

    //    MeshInstance& mesh_instance = scene.mesh_instances[ i ];
    //    Mesh& mesh = scene.meshes[ mesh_instance.mesh_index ];
    //    if ( mesh.is_transparent() ) {
    //        continue;
    //    }

    //    MeshInstanceDraw mesh_instance_draw{};
    //    mesh_instance_draw.mesh_instance = &mesh_instance;
    //    mesh_instance_draw.material_pass_index = mesh.has_skinning() ? main_technique->get_pass_index( "gbuffer_skinning" ) : main_technique->get_pass_index( "gbuffer_cull" );

    //    mesh_instance_draws.push( mesh_instance_draw );
    //}
    // TODO:
    // Cache meshlet technique index
    //GpuTechnique* meshlet_technique = renderer->resource_cache.techniques.get( k_technique_hash );

    //meshlet_draw_pipeline = meshlet_technique->get_pipeline( k_gbuffer_culling );
    //meshlet_draw_pipeline_slang = meshlet_technique->get_pipeline( k_gbuffer_culling_slang );

    //meshlet_emulation_draw_pipeline = meshlet_technique->get_pipeline( k_gbuffer_culling_emulation );

    //GpuTechniquePass& generate_ib_pass = meshlet_technique->get_pass( k_generate_index_buffer );
    //generate_meshlet_index_buffer_pipeline = generate_ib_pass.pipeline;

    //GpuTechniquePass& generate_inst_pass = meshlet_technique->get_pass( k_generate_meshlet_instances );
    //generate_meshlets_instances_pipeline = generate_inst_pass.pipeline;

    //GpuTechniquePass& inst_cull_pass = meshlet_technique->get_pass( k_meshlet_instance_culling );
    //meshlet_instance_culling_pipeline = inst_cull_pass.pipeline;

    //meshlet_write_counts_pipeline = meshlet_technique->get_pipeline( k_meshlet_write_counts );

    //DescriptorSetBinder descriptors;

    //for ( u32 i = 0; i < k_max_frames; ++i ) {
    //    descriptors.reset();
    //    descriptors.bind_ssbo( render_blackboard.meshlet_indirect_early_commands_sb[i], generate_ib_pass.get_binding_index( "VisibleMeshInstances" ) );
    //    descriptors.bind_ssbo( render_blackboard.meshlet_indirect_early_count_sb[i], generate_ib_pass.get_binding_index( "VisibleMeshCount" ) );
    //    descriptors.bind_ssbo( render_blackboard.meshlets_emulation_index_buffer_sb[i], generate_ib_pass.get_binding_index( "VisibleMeshletIndexBuffer" ) );
    //    descriptors.bind_ssbo( render_blackboard.meshlets_emulation_instances_sb[i], generate_ib_pass.get_binding_index( "MeshletInstances" ) );
    //    descriptors.bind_ssbo( render_blackboard.meshlets_emulation_visible_instances_sb[i], generate_ib_pass.get_binding_index( "VisibleMeshletInstances" ) );
    //    generate_meshlet_index_buffer_descriptor_set[ i ] = generate_ib_pass.create_descriptor_set( renderer, descriptors, i );

    //    descriptors.reset();
    //    descriptors.bind_ssbo( render_blackboard.meshlet_indirect_early_commands_sb[i], generate_inst_pass.get_binding_index( "VisibleMeshInstances" ) );
    //    descriptors.bind_ssbo( render_blackboard.meshlet_indirect_early_count_sb[i], generate_inst_pass.get_binding_index( "VisibleMeshCount" ) );
    //    descriptors.bind_ssbo( render_blackboard.meshlets_emulation_index_buffer_sb[i], generate_inst_pass.get_binding_index( "VisibleMeshletIndexBuffer" ) );
    //    descriptors.bind_ssbo( render_blackboard.meshlets_emulation_instances_sb[i], generate_inst_pass.get_binding_index( "MeshletInstances" ) );
    //    descriptors.bind_ssbo( render_blackboard.meshlet_emulation_instances_indirect_count_sb[i], generate_inst_pass.get_binding_index( "IndirectPerMeshletCounts" ) );
    //    generate_meshlets_instances_descriptor_set[ i ] = generate_inst_pass.create_descriptor_set( renderer, descriptors, i );

    //    BufferCreation buffer_creation;
    //    meshlet_instance_culling_indirect_buffer[ i ] = renderer->gpu->create_buffer( {
    //        .type_flags = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
    //        .usage = ResourceUsageType::Dynamic, .size = sizeof( u32 ) * 4,
    //        .name = "meshlet_instance_culling_indirect_buffer" } );

    //    descriptors.reset();
    //    descriptors.bind_ssbo( render_blackboard.meshlet_indirect_early_commands_sb[i], inst_cull_pass.get_binding_index( "VisibleMeshInstances" ) );
    //    descriptors.bind_ssbo( render_blackboard.meshlet_indirect_early_count_sb[i], inst_cull_pass.get_binding_index( "VisibleMeshCount" ) );
    //    descriptors.bind_ssbo( render_blackboard.meshlets_instances_sb[i], inst_cull_pass.get_binding_index( "MeshletInstances" ) );
    //    descriptors.bind_ssbo( meshlet_instance_culling_indirect_buffer[ i ], inst_cull_pass.get_binding_index( "IndirectPerMeshletCounts" ) );
    //    descriptors.bind_ssbo( render_blackboard.meshlets_visible_instances_sb[i], inst_cull_pass.get_binding_index( "VisibleMeshletInstances" ) );
    //    meshlet_instance_culling_descriptor_set[ i ] = inst_cull_pass.create_descriptor_set( renderer, descriptors, i );

    //    // Cache indirect buffer
    //    generate_meshlet_dispatch_indirect_buffer[ i ] = render_blackboard.meshlet_indirect_early_count_sb[ i ];
    //}
}

void GBufferPass::destroy_gpu_resources( FrameGraphResourceContext& context ) {
    if ( !enabled )
        return;

    Renderer* renderer = context.renderer;
    GpuDevice& gpu = *renderer->gpu;

    for ( u32 i = 0; i < k_max_frames; ++i ) {
        gpu.destroy_buffer( meshlet_instance_culling_indirect_buffer[ i ] );

        gpu.destroy_descriptor_set( generate_meshlet_index_buffer_descriptor_set[ i ] );
        gpu.destroy_descriptor_set( generate_meshlets_instances_descriptor_set[ i ] );

        gpu.destroy_descriptor_set( meshlet_instance_culling_descriptor_set[ i ] );
    }
}

// LateGBufferPass /////////////////////////////////////////////////////////
void LateGBufferPass::create_gpu_resources( FrameGraphResourceContext& context ) {
    FrameGraph* frame_graph = context.frame_graph;
    RenderScene* scene = context.render_scene;

    FrameGraphNode* node = frame_graph->get_node( "gbuffer_pass_late" );
    if ( node == nullptr ) {
        enabled = false;

        return;
    }

    enabled = node->enabled;
    if ( !enabled )
        return;

    //const u64 hashed_name = hash_calculate( "main" );
    //GpuTechnique* main_technique = renderer->resource_cache.techniques.get( hashed_name );

    //mesh_instance_draws.init( resident_allocator, 16 );

    //// Copy all mesh draws and change only material.
    //for ( u32 i = 0; i < scene.mesh_instances.size; ++i ) {

    //    MeshInstance& mesh_instance = scene.mesh_instances[ i ];
    //    Mesh& mesh = scene.meshes[ mesh_instance.mesh_index ];
    //    if ( mesh.is_transparent() ) {
    //        continue;
    //    }

    //    MeshInstanceDraw mesh_instance_draw{};
    //    mesh_instance_draw.mesh_instance = &mesh_instance;
    //    mesh_instance_draw.material_pass_index = mesh.has_skinning() ? main_technique->get_pass_index( "gbuffer_skinning" ) : main_technique->get_pass_index( "gbuffer_cull" );

    //    mesh_instance_draws.push( mesh_instance_draw );
    //}

    //// Cache meshlet technique index
    //if ( renderer->gpu->mesh_shaders_extension_present ) {
    //    GpuTechnique* main_technique = renderer->resource_cache.techniques.get( k_technique_hash );
    //    meshlet_technique_index = main_technique->get_pass_index( k_gbuffer_culling );
    //    meshlet_technique_slang_index = main_technique->get_pass_index( k_gbuffer_culling_slang );
    //}
}

void LateGBufferPass::destroy_gpu_resources( FrameGraphResourceContext& context ) {
    if ( !enabled )
        return;
}

void LateGBufferPass::declare_frame_graph_node( FrameGraphResourceContext& context ) {

    FrameGraphBuilder& builder = *context.frame_graph->builder;

    context.frame_graph->add_node_v2( {
        .inputs = {
            {
                .type = FrameGraphResourceType_Buffer,
                .handle = builder.get_output_handle( "mesh_occlusion_late_pass", "late_mesh_indirect_draw_list" )
            },
            {
                .type = FrameGraphResourceType_Buffer,
                .handle = builder.get_output_handle( "mesh_occlusion_late_pass", "late_task_indirect_draw_list" )
            },
            {
                .type = FrameGraphResourceType_Attachment,
                .handle = builder.get_output_handle( "gbuffer_pass_early", "gbuffer_colour" )
            },
            {
                .type = FrameGraphResourceType_Attachment,
                .handle = builder.get_output_handle( "gbuffer_pass_early", "gbuffer_normals" )
            },
            {
                .type = FrameGraphResourceType_Attachment,
                .handle = builder.get_output_handle( "gbuffer_pass_early", "gbuffer_metallic_roughness_occlusion" )
            },
            {
                .type = FrameGraphResourceType_Attachment,
                .handle = builder.get_output_handle( "gbuffer_pass_early", "gbuffer_emissive" )
            },
            {
                .type = FrameGraphResourceType_Attachment,
                .handle = builder.get_output_handle( "gbuffer_pass_early", "mesh_id" )
            },
            {
                .type = FrameGraphResourceType_Attachment,
                .handle = builder.get_output_handle( "gbuffer_pass_early", "z_normal_fwidth" )
            },
            {
                .type = FrameGraphResourceType_Attachment,
                .handle = builder.get_output_handle( "gbuffer_pass_early", "linear_z_dd" )
            },
            {
                .type = FrameGraphResourceType_Attachment,
                .handle = builder.get_output_handle( "gbuffer_pass_early", "depth" )
            },
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "depth_pyramid_pass", "depth_pyramid" )
            },
        },
        .outputs = {
            builder.create_output_reference(
                builder.get_output_handle( "gbuffer_pass_early", "gbuffer_colour" ),
                FrameGraphResourceType_Reference ),
            builder.create_output_reference(
                builder.get_output_handle( "gbuffer_pass_early", "gbuffer_normals" ),
                FrameGraphResourceType_Reference ),
            builder.create_output_reference(
                builder.get_output_handle( "gbuffer_pass_early", "gbuffer_metallic_roughness_occlusion" ),
                FrameGraphResourceType_Reference ),
            builder.create_output_reference(
                builder.get_output_handle( "gbuffer_pass_early", "gbuffer_emissive" ),
                FrameGraphResourceType_Reference ),
            builder.create_output_reference(
                builder.get_output_handle( "gbuffer_pass_early", "mesh_id" ),
                FrameGraphResourceType_Reference ),
            builder.create_output_reference(
                builder.get_output_handle( "gbuffer_pass_early", "z_normal_fwidth" ),
                FrameGraphResourceType_Reference ),
            builder.create_output_reference(
                builder.get_output_handle( "gbuffer_pass_early", "linear_z_dd" ),
                FrameGraphResourceType_Reference ),
            builder.create_output_reference(
                builder.get_output_handle( "gbuffer_pass_early", "depth" ),
                FrameGraphResourceType_Reference ),
        },
        .scheduling = { CommandQueueType::Graphics, 0 },
        .enabled = true,
        .name = "gbuffer_pass_late" } );
}

void LateGBufferPass::render( FrameGraphRenderContext& context ) {

    if ( !enabled )
        return;

    RenderScene* render_scene = context.render_view->scene;
    u32 current_frame_index = context.current_frame_index;
    CommandBuffer* gpu_commands = context.gpu_commands;
    Renderer* renderer = context.renderer;
    RenderBlackboard& render_blackboard = *context.render_blackboard;
    GpuCullingRuntimeData& gpu_culling = render_blackboard.gpu_culling;

    if ( context.render_config->meshlets.use_meshlets ) {

        if ( context.render_config->use_slang_shaders ) {
            PipelineHandle pipeline = renderer->resource_cache.pipelines.get( hash_calculate( pc_meshlet_gbuffer_culling_slang.name ));
            gpu_commands->bind_pipeline( pipeline );

            gpu_commands->bind_descriptor_set(
                { renderer->gpu->bindless_descriptor_set, render_blackboard.meshlets.meshlets_late_draw_descriptor_set_slang[ current_frame_index ] },
                { render_blackboard.scene_cb_offset } );
        }
        else
        {
            PipelineHandle pipeline = renderer->resource_cache.pipelines.get( hash_calculate( pc_meshlet_gbuffer_culling.name ) );
            gpu_commands->bind_pipeline( pipeline );

            gpu_commands->bind_descriptor_set(
                { renderer->gpu->bindless_descriptor_set, render_blackboard.meshlets.meshlets_late_draw_descriptor_set[ current_frame_index ] },
                { render_blackboard.scene_cb_offset } );
        }

        gpu_commands->draw_mesh_task_indirect_count(
            gpu_culling.meshlet_indirect_late_commands_sb[ current_frame_index ], offsetof( GpuMeshDrawCommand, indirectMS ),
            gpu_culling.meshlet_indirect_late_count_sb[ current_frame_index ], 0,
            render_scene->mesh_instances.size, sizeof( GpuMeshDrawCommand ) );
    }
}

} // namespace raptor
