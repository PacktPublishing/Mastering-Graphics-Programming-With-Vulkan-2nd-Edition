#include "graphics/render_passes/debug_pass.hpp"
#include "graphics/render_passes/ddgi_pass.hpp"
#include "graphics/render_scene.hpp"
#include "graphics/render_blackboard.hpp"
#include "graphics/scene_graph.hpp"

#include "external/glm/mat4x4.hpp"
#include "external/glm/gtc/quaternion.hpp"

#include <assimp/cimport.h>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#define DEBUG_DRAW_MESHLET_SPHERES 0
#define DEBUG_DRAW_MESHLET_CONES 0
#define DEBUG_DRAW_POINT_LIGHT_SPHERES 0
#define DEBUG_DRAW_REFLECTION_PROBES 0


namespace raptor {

//
// DebugPass ////////////////////////////////////////////////////////
static void load_debug_mesh( cstring filename, Allocator* resident_allocator, Renderer* renderer, u32& index_count, BufferHandle* mesh_buffer, BufferHandle* index_buffer ) {
    const aiScene* mesh_scene = aiImportFile( filename,
                                              aiProcess_CalcTangentSpace |
                                              aiProcess_GenNormals |
                                              aiProcess_Triangulate |
                                              aiProcess_JoinIdenticalVertices |
                                              aiProcess_SortByPType );

    Array<glm::vec3> positions;
    positions.init( resident_allocator, rkilo( 64 ) );

    Array<u32> indices;
    indices.init( resident_allocator, rkilo( 64 ) );

    index_count = 0;

    for ( u32 mesh_index = 0; mesh_index < mesh_scene->mNumMeshes; ++mesh_index ) {
        aiMesh* mesh = mesh_scene->mMeshes[ mesh_index ];

        RASSERT( ( mesh->mPrimitiveTypes & aiPrimitiveType_TRIANGLE ) != 0 );

        for ( u32 vertex_index = 0; vertex_index < mesh->mNumVertices; ++vertex_index ) {
            glm::vec3 position{
                mesh->mVertices[ vertex_index ].x,
                mesh->mVertices[ vertex_index ].y,
                mesh->mVertices[ vertex_index ].z
            };

            positions.push( position );
        }

        for ( u32 face_index = 0; face_index < mesh->mNumFaces; ++face_index ) {
            RASSERT( mesh->mFaces[ face_index ].mNumIndices == 3 );

            u32 index_a = mesh->mFaces[ face_index ].mIndices[ 0 ];
            u32 index_b = mesh->mFaces[ face_index ].mIndices[ 1 ];
            u32 index_c = mesh->mFaces[ face_index ].mIndices[ 2 ];

            indices.push( index_a );
            indices.push( index_b );
            indices.push( index_c );
        }

        index_count = indices.size;
    }

    const VkDeviceSize position_buffer_size = VkDeviceSize( positions.size ) * sizeof( glm::vec3 );
    const VkDeviceSize index_buffer_size = VkDeviceSize( indices.size ) * sizeof( u32 );

    RASSERT( position_buffer_size > 0 );
    RASSERT( index_buffer_size > 0 );

    *mesh_buffer = renderer->create_buffer_with_upload( {
        .size = position_buffer_size,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
        .name = "debug_mesh_pos"
    }, {
        .data = positions.data,
        .policy = BufferUploadPolicy::HostWrite
    } );

    *index_buffer = renderer->create_buffer_with_upload( {
        .size = index_buffer_size,
        .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
        .name = "debug_mesh_indices"
    }, {
        .data = indices.data,
        .policy = BufferUploadPolicy::HostWrite
    } );

    positions.shutdown();
    indices.shutdown();
}

static BlendState blend_premultiplied = {
    .source_color = VK_BLEND_FACTOR_ONE,
    .destination_color = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    .color_operation = VK_BLEND_OP_ADD,
    .source_alpha = VK_BLEND_FACTOR_ONE,
    .destination_alpha = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    .alpha_operation = VK_BLEND_OP_ADD,
};

static ShaderCompilationCreation scc_debug_mesh = {
    .stages = {
        ShaderCompilationStage{
            .source_file_path = "glsl/debug_mesh.glsl",
            .type = VK_SHADER_STAGE_VERTEX_BIT,
        },
        ShaderCompilationStage{
            .source_file_path = "glsl/debug_mesh.glsl",
            .type = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    },
    .name = "debug_mesh",
    .slang_input = 0,
};

static ShaderCompilationCreation scc_debug_update_sphere_matrices = {
    .stages = {
        ShaderCompilationStage{
            .source_file_path = "glsl/debug_mesh.glsl",
            .type = VK_SHADER_STAGE_COMPUTE_BIT,
        }
    },
    .name = "debug_update_sphere_matrices",
    .slang_input = 0,
};

static ShaderCompilationCreation scc_debug_update_cone_matrices = {
    .stages = {
        ShaderCompilationStage{
            .source_file_path = "glsl/debug_mesh.glsl",
            .type = VK_SHADER_STAGE_COMPUTE_BIT,
        }
    },
    .name = "debug_update_cone_matrices",
    .slang_input = 0,
};

inline const VertexInputCreation vi_debug_mesh = {
    .bindings = {
        { 0, 12, VK_VERTEX_INPUT_RATE_VERTEX }
    },
    .attributes = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
    }
};

static PipelineCreation pc_debug_mesh = {
    .rasterization = {
        .cull_mode = VK_CULL_MODE_NONE,
        .front = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .fill = VK_POLYGON_MODE_FILL,
    },
    .depth_stencil = {
        .front = {},
        .back = {},
        .depth_comparison = VK_COMPARE_OP_ALWAYS,
        .depth_enable = 1,
        .depth_write_enable = 0,
        .stencil_enable = 0,
    },
    .blend_state = {
        .blend_states = { blend_premultiplied },
    },
    .vertex_input = vi_debug_mesh,
    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    .flags = 0,

    .render_pass_output = {},
    .shader = {},

    .layout = {},
    .viewport = nullptr,

    .num_active_layouts = 0,
    .num_specialization_constants = 0,

    .name = "debug_mesh",
    .render_pass_name = "debug_mesh_pass",
};


void DebugPass::declare_frame_graph_node( FrameGraphResourceContext& context ) {
    FrameGraphBuilder& builder = *context.frame_graph->builder;

    context.frame_graph->add_node_v2( {
        .inputs = {
            {
                .type = FrameGraphResourceType_Attachment,
                .handle = builder.get_output_handle( "debug_draw_pass", "final" )
            },
        },
        .outputs = {
            builder.create_output_reference(
                builder.get_output_handle( "debug_draw_pass", "final" ),
                FrameGraphResourceType_Reference )
        },
        .scheduling = { CommandQueueType::Graphics, 1 },
        .enabled = true,
        .name = "debug_mesh_pass" } );
}

void DebugPass::update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) {

    Renderer* renderer = context.renderer;

    if ( phase == PipelineUpdatePhase::Destroy ) {
        renderer->destroy_graphics_pipeline_state( debug_mesh_pipeline );
        renderer->destroy_compute_pipeline_state( debug_update_sphere_matrices_pipeline );
        renderer->destroy_compute_pipeline_state( debug_update_cone_matrices_pipeline );
        return;
    }

    GraphicsPipelineTransaction transaction( renderer );

    GraphicsPipelineState& new_debug_mesh_pipeline = transaction.add( debug_mesh_pipeline );

    renderer->create_graphics_pipeline_state( scc_debug_mesh, pc_debug_mesh,
                                              "debug_mesh", context.frame_graph, new_debug_mesh_pipeline );

    transaction.commit_or_rollback();

    ComputePipelineTransaction compute_transaction( renderer );

    ComputePipelineState& new_debug_update_sphere_matrices_pipeline = compute_transaction.add( debug_update_sphere_matrices_pipeline );
    ComputePipelineState& new_debug_update_cone_matrices_pipeline = compute_transaction.add( debug_update_cone_matrices_pipeline );

    renderer->create_compute_pipeline_state( scc_debug_update_sphere_matrices, { .name = scc_debug_update_sphere_matrices.name.data },
                                             "debug_update_sphere_matrices", context.frame_graph, new_debug_update_sphere_matrices_pipeline );

    renderer->create_compute_pipeline_state( scc_debug_update_cone_matrices, { .name = scc_debug_update_cone_matrices.name.data },
                                             "debug_update_cone_matrices", context.frame_graph, new_debug_update_cone_matrices_pipeline );

    compute_transaction.commit_or_rollback();
}

void DebugPass::render( FrameGraphRenderContext& context ) {
    if ( !enabled )
        return;

    RenderScene* render_scene = context.render_view->scene;
    Renderer* renderer = context.renderer;
    u32 current_frame_index = context.current_frame_index;
    CommandBuffer* cb = context.gpu_commands;
    RenderBlackboard& render_blackboard = *context.render_blackboard;
    DebugDrawRuntimeData& debug_draw = render_blackboard.debug_draw;

#if ( DEBUG_DRAW_MESHLET_SPHERES || DEBUG_DRAW_POINT_LIGHT_SPHERES )
    cb->bind_pipeline( debug_mesh_pipeline.pipeline );
    cb->bind_vertex_buffer( sphere_mesh_buffer, 0, 0 );
    cb->bind_index_buffer( sphere_mesh_indices, 0, VK_INDEX_TYPE_UINT32 );

    cb->bind_descriptor_set(
        { renderer->gpu->bindless_descriptor_set, sphere_mesh_descriptor_set[ current_frame_index ] },
        { render_blackboard.scene_cb_offset } );

    cb->draw_indexed_indirect( sphere_draw_indirect_buffer, bounding_sphere_count, 0, sizeof( VkDrawIndexedIndirectCommand ) );
#endif

#if DEBUG_DRAW_MESHLET_CONES
    cb->bind_pipeline( debug_mesh_pipeline.pipeline );
    cb->bind_vertex_buffer( cone_mesh_buffer, 0, 0 );
    cb->bind_index_buffer( cone_mesh_indices, 0, VK_INDEX_TYPE_UINT32 );

    cb->bind_descriptor_set(
        { renderer->gpu->bindless_descriptor_set, cone_mesh_descriptor_set[ current_frame_index ] },
        { render_blackboard.scene_cb_offset } );

    cb->draw_indexed_indirect( cone_draw_indirect_buffer, bounding_sphere_count, 0, sizeof( VkDrawIndexedIndirectCommand ) );
#endif

    // Draw GI debug probe spheres
    if ( context.render_config->gi_show_probes ) {
        cb->bind_pipeline( debug_mesh_pipeline.pipeline );
        cb->bind_vertex_buffer( sphere_mesh_buffer, 0, 0 );
        cb->bind_index_buffer( sphere_mesh_indices, 0, VK_INDEX_TYPE_UINT32 );
        cb->bind_descriptor_set(
            { renderer->gpu->bindless_descriptor_set, sphere_mesh_descriptor_set[ current_frame_index ] },
            { render_blackboard.scene_cb_offset, render_blackboard.ddgi_constants_offset } );

        // TODO: draw only one sphere
        cb->draw_indexed( TopologyType::Triangle, sphere_index_count, context.render_config->gi_total_probes, 0, 0, 0 );
    }

    // pipeline = renderer->get_pipeline( debug_material, 1 );

    // cb->bind_pipeline( pipeline );

    // for ( u32 mesh_index = 0; mesh_index < mesh_instances.size; ++mesh_index ) {
    //     MeshInstance& mesh_instance = mesh_instances[ mesh_index ];
    //     Mesh& mesh = *mesh_instance.mesh;

    //     if ( mesh.physics_mesh != nullptr ) {
    //         PhysicsMesh* physics_mesh = mesh.physics_mesh;

    //         cb->bind_descriptor_set( &physics_mesh->debug_mesh_descriptor_set, 1, nullptr, 0 );

    //         cb->draw_indirect( physics_mesh->draw_indirect_buffer, physics_mesh->vertices.size, 0, sizeof( VkDrawIndirectCommand  ) );
    //     }
    // }

    // Draw cpu debug rendering
    //render_scene->debug_renderer.render( current_frame_index, cb, render_scene );

    // Draw gpu written debug lines
    if ( context.render_config->debug_draw.show_gpu_draws ) {

        cb->bind_pipeline( debug_lines_draw_pipeline );
        cb->bind_descriptor_set(
            { renderer->gpu->bindless_descriptor_set, debug_lines_draw_set },
            { render_blackboard.scene_cb_offset } );

        cb->draw_indirect( debug_draw.gpu_line_commands_sb, 1, 0, sizeof( VkDrawIndirectCommand ) );
        // Draw 2d lines
        cb->bind_pipeline( debug_lines_2d_draw_pipeline );
        cb->bind_descriptor_set(
            { renderer->gpu->bindless_descriptor_set, debug_lines_draw_set },
            { render_blackboard.scene_cb_offset } );

        cb->draw_indirect( debug_draw.gpu_line_commands_sb, 1, sizeof( VkDrawIndirectCommand ), sizeof( VkDrawIndirectCommand ) );
    }
}

void DebugPass::pre_render( FrameGraphRenderContext& context ) {

    if ( !enabled ) {
        return;
    }

    RenderScene* render_scene = context.render_view->scene;
    u32 current_frame_index = context.current_frame_index;
    CommandBuffer* cb = context.gpu_commands;
    RenderBlackboard& render_blackboard = *context.render_blackboard;
    Renderer* renderer = context.renderer;

    Buffer* line_commands = renderer->gpu->get_buffer( debug_line_commands_sb_cache );

    //util_add_buffer_barrier( renderer->gpu, cb->vk_command_buffer, line_commands->vk_buffer,
      //                       RESOURCE_STATE_INDIRECT_ARGUMENT, RESOURCE_STATE_UNORDERED_ACCESS, line_commands->size );

    // Write final command
    // cb->bind_pipeline( debug_lines_finalize_pipeline );
    // cb->bind_descriptor_set(
    //     { renderer->gpu->bindless_descriptor_set, debug_lines_finalize_set },
    //     { render_blackboard.scene_cb_offset } );

    // cb->dispatch( 1, 1, 1 );

   // util_add_buffer_barrier( renderer->gpu, cb->vk_command_buffer, line_commands->vk_buffer,
   //                          RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_INDIRECT_ARGUMENT, line_commands->size );

#if DEBUG_DRAW_MESHLET_SPHERES
    BufferHandle sphere_matrices_buffer_handle = sphere_matrices_buffer[ current_frame_index ];
    cb->add_buffer_barrier( sphere_matrices_buffer_handle, 0, VK_WHOLE_SIZE,
                            { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_WRITE_BIT } );

    cb->flush_barriers();

    cb->bind_pipeline( debug_update_sphere_matrices_pipeline.pipeline );
    cb->bind_descriptor_set(
        { renderer->gpu->bindless_descriptor_set, debug_update_sphere_matrices_descriptor_set[ current_frame_index ] },
        { render_blackboard.scene_cb_offset } );

    for ( u32 mesh_index = 0; mesh_index < render_scene->meshes.size; ++mesh_index ) {
        Mesh& mesh = render_scene->meshes[ mesh_index ];
        if ( !mesh.has_skinning() ) continue;

        cb->push_constants( debug_update_sphere_matrices_pipeline.pipeline, 0, 4, &mesh_index );
        cb->dispatch( mesh.meshlet_count, 1, 1 );
    }

    cb->add_buffer_barrier( sphere_matrices_buffer_handle, 0, VK_WHOLE_SIZE,
                            { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_READ_BIT } );

    cb->flush_barriers();
#endif

#if DEBUG_DRAW_MESHLET_CONES
    BufferHandle cones_matrices_buffer_handle = cone_matrices_buffer[ current_frame_index ];
    cb->add_buffer_barrier( cones_matrices_buffer_handle, 0, VK_WHOLE_SIZE,
                            { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_WRITE_BIT } );

    cb->flush_barriers();

    cb->bind_pipeline( debug_update_cone_matrices_pipeline.pipeline );
    cb->bind_descriptor_set(
        { renderer->gpu->bindless_descriptor_set, debug_update_cone_matrices_descriptor_set[ current_frame_index ] },
        { render_blackboard.scene_cb_offset } );

    for ( u32 mesh_index = 0; mesh_index < render_scene->meshes.size; ++mesh_index ) {
        Mesh& mesh = render_scene->meshes[ mesh_index ];
        if ( !mesh.has_skinning() ) continue;

        cb->push_constants( debug_update_cone_matrices_pipeline.pipeline, 0, 4, &mesh_index );
        cb->dispatch( mesh.meshlet_count, 1, 1 );
    }

    cb->add_buffer_barrier( cones_matrices_buffer_handle, 0, VK_WHOLE_SIZE,
                            { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_READ_BIT } );

    cb->flush_barriers();
#endif
}

void DebugPass::create_gpu_resources( FrameGraphResourceContext& context ) {

    FrameGraph* frame_graph = context.frame_graph;
    RenderScene& scene = *context.render_scene;
    Allocator* resident_allocator = context.renderer->gpu->allocator;
    Renderer* renderer = context.renderer;
    RenderBlackboard& render_blackboard = *context.render_blackboard;
    MeshletsRuntimeData& meshlets_data = render_blackboard.meshlets;

    FrameGraphNode* node = frame_graph->get_node( "debug_mesh_pass" );
    if ( node == nullptr ) {
        enabled = false;

        return;
    }

    enabled = node->enabled;
    if ( !enabled )
        return;

//    const u64 hashed_name = hash_calculate( "debug" );
//    GpuTechnique* main_technique = renderer->resource_cache.techniques.get( hashed_name );
//
//    MaterialCreation material_creation;
//
//    material_creation.set_name( "material_debug" ).set_technique( main_technique ).set_render_index( 0 );
//    debug_material = renderer->create_material( material_creation );
//
    StringBuffer mesh_name;
    mesh_name.init( 1024, resident_allocator );

#if ( DEBUG_DRAW_MESHLET_SPHERES || DEBUG_DRAW_POINT_LIGHT_SPHERES || DEBUG_DRAW_REFLECTION_PROBES )
    cstring filename = mesh_name.append_use_f( "%s/sphere.obj", RAPTOR_DATA_FOLDER );
    load_debug_mesh( filename, resident_allocator, renderer, sphere_index_count, &sphere_mesh_buffer, &sphere_mesh_indices );
#endif // DEBUG_DRAW_MESHLET_SPHERES | DEBUG_DRAW_POINT_LIGHT_SPHERES


#if DEBUG_DRAW_MESHLET_CONES
    filename = mesh_name.append_use_f( "%s/cone.obj", RAPTOR_DATA_FOLDER );
   load_debug_mesh( filename, resident_allocator, renderer, cone_index_count, &cone_mesh_buffer, &cone_mesh_indices );
#endif // DEBUG_DRAW_MESHLET_CONES

    mesh_name.shutdown();

#if DEBUG_DRAW_MESHLET_SPHERES || DEBUG_DRAW_MESHLET_CONES
    // Get all meshlets bounding spheres
    Array<glm::mat4> bounding_matrices;
    bounding_matrices.init( resident_allocator, scene.meshlets.size );

    Array<VkDrawIndexedIndirectCommand> sphere_indirect_commands;
    sphere_indirect_commands.init( resident_allocator, scene.meshlets.size );

    Array<glm::mat4> cone_matrices;
    cone_matrices.init( resident_allocator, 4096 );

    Array<VkDrawIndexedIndirectCommand> cone_indirect_commands;
    cone_indirect_commands.init( resident_allocator, 4096 );

    for ( u32 i = 0; i < scene.meshlets.size; ++i ) {
        GpuMeshlet& meshlet = scene.meshlets[ i ];

        if ( meshlet.radius > 80.0f ) {
            continue;
        }

        // TODO(marco): restore proper code once we support mesh instances
        Mesh& mesh = scene.meshes[ meshlet.mesh_index ];
        MeshInstance* mi = nullptr;
        for ( u32 m = 0; m < scene.mesh_instances.size; ++m ) {
            if ( scene.mesh_instances[ m ].mesh_index == meshlet.mesh_index ) {
                RASSERT( mi == nullptr ); // We should only have one instance per mesh for now
                mi = &scene.mesh_instances[ m ];
            }
        }

        bool padding_meshlet = ( i - mesh.meshlet_offset ) >= mesh.meshlet_count;
        if ( padding_meshlet ) RASSERT( meshlet.radius == 0.0f );

        glm::mat4 local_transform = scene.scene_graph->local_matrices[ mi->scene_graph_node_index ];

        // Meshlet bounding spheres
        glm::mat4 sphere_bounding_matrix = glm::mat4( 1.0f );
        sphere_bounding_matrix = glm::translate( sphere_bounding_matrix, meshlet.center );
        sphere_bounding_matrix = glm::scale( sphere_bounding_matrix, glm::vec3{ meshlet.radius, meshlet.radius, meshlet.radius } );
        sphere_bounding_matrix = local_transform * sphere_bounding_matrix;

        bounding_matrices.push( sphere_bounding_matrix );

        VkDrawIndexedIndirectCommand draw_command{ };
        draw_command.indexCount = sphere_index_count;
        draw_command.instanceCount = 1;

        sphere_indirect_commands.push( draw_command );

        // Meshlet cones
        glm::vec3 up{ 0.0f, 1.0f, 0.0f };

        glm::vec3 cone_axis{ meshlet.cone_axis[ 0 ] / 127.0f, meshlet.cone_axis[ 1 ] / 127.0f, meshlet.cone_axis[ 2 ] / 127.0f };
        cone_axis = glm::normalize( cone_axis );

        glm::quat qrotation = glm::quat( up, cone_axis );
        glm::mat4 rotation = glm::mat4_cast( qrotation );

        glm::mat4 id = glm::mat4( 1.0f );
        glm::mat4 t = glm::translate( id, meshlet.center );
        glm::mat4 s = glm::scale( id, glm::vec3{ meshlet.radius * 0.5f, meshlet.radius * 0.5f, meshlet.radius * 0.5f } );

        glm::mat4 cone_matrix = t * rotation * s;
        cone_matrix = local_transform * cone_matrix;

        cone_matrices.push( cone_matrix );

        draw_command = { };
        draw_command.indexCount = cone_index_count;
        draw_command.instanceCount = 1;

        cone_indirect_commands.push( draw_command );
    }

    bounding_sphere_count = bounding_matrices.size;
#endif
#if DEBUG_DRAW_MESHLET_SPHERES
    {
        const VkDeviceSize buffer_size = VkDeviceSize( bounding_matrices.size ) * sizeof( glm::mat4 );
        RASSERT( buffer_size > 0 );

        for ( u32 i = 0; i < k_max_frames; ++i ) {
            sphere_matrices_buffer[ i ] = renderer->create_buffer_with_upload( {
                .size = buffer_size,
                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                .name = "meshlet_bounding_spheres_transform"
            }, {
                .data = bounding_matrices.data,
                .policy = BufferUploadPolicy::HostWrite
            } );
        }
    }

    {
        const VkDeviceSize buffer_size =
            VkDeviceSize( sphere_indirect_commands.size ) * sizeof( VkDrawIndexedIndirectCommand );

        RASSERT( buffer_size > 0 );

        sphere_draw_indirect_buffer = renderer->create_buffer_with_upload( {
            .size = buffer_size,
            .usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
            .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
            .name = "meshlet_bound_sphere_draw_commands"
        }, {
            .data = sphere_indirect_commands.data,
            .policy = BufferUploadPolicy::HostWrite
        } );
    }

    {
        DescriptorSetBinder descriptors;

        for ( u32 i = 0; i < k_max_frames; ++i ) {
            ShaderReflectionInfo* shader_reflection = renderer->get_shader_reflection( debug_mesh_pipeline.pipeline );
            descriptors.reset();
            descriptors.name = "debug_mesh_sphere_ds";
            descriptors.ssbos.push( { sphere_matrices_buffer[ i ], 10 } );

            sphere_mesh_descriptor_set[ i ] = renderer->create_descriptor_set( descriptors, shader_reflection,
                debug_mesh_pipeline.pipeline, 0, *context.render_blackboard );

            descriptors.reset();
            ShaderReflectionInfo* compute_shader_reflection = renderer->get_shader_reflection( debug_update_sphere_matrices_pipeline.pipeline );
            descriptors.name = "debug_update_sphere_matrices_ds";
            descriptors.ssbos.push( { sphere_matrices_buffer[ i ], 10 } );

            debug_update_sphere_matrices_descriptor_set[ i ] = renderer->create_descriptor_set( descriptors, compute_shader_reflection,
            debug_update_sphere_matrices_pipeline.pipeline, 0, *context.render_blackboard );
        }
    }
#endif // DEBUG_DRAW_MESHLET_SPHERES
#if DEBUG_DRAW_MESHLET_CONES
    {
        const VkDeviceSize buffer_size = VkDeviceSize( cone_matrices.size ) * sizeof( glm::mat4 );
        RASSERT( buffer_size > 0 );

        for ( u32 i = 0; i < k_max_frames; ++i ) {
            cone_matrices_buffer[ i ] = renderer->create_buffer_with_upload( {
                .size = buffer_size,
                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                .name = "meshlet_cones_transform"
            }, {
                .data = cone_matrices.data,
                .policy = BufferUploadPolicy::HostWrite
            } );
        }
    }

    {
        const VkDeviceSize buffer_size =
            VkDeviceSize( cone_indirect_commands.size ) * sizeof( VkDrawIndexedIndirectCommand );

        RASSERT( buffer_size > 0 );

        cone_draw_indirect_buffer = renderer->create_buffer_with_upload( {
            .size = buffer_size,
            .usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
            .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
            .name = "meshlet_cone_draw_commands"
        }, {
            .data = cone_indirect_commands.data,
            .policy = BufferUploadPolicy::HostWrite
        } );
    }

    {
        DescriptorSetBinder descriptors;

        for ( u32 i = 0; i < k_max_frames; ++i ) {
            ShaderReflectionInfo* shader_reflection = renderer->get_shader_reflection( debug_mesh_pipeline.pipeline );
            descriptors.reset();
            descriptors.name = "debug_mesh_cone_ds";
            descriptors.ssbos.push( { cone_matrices_buffer[ i ], 10 } );

            cone_mesh_descriptor_set[ i ] = renderer->create_descriptor_set( descriptors, shader_reflection,
                debug_mesh_pipeline.pipeline, 0, *context.render_blackboard );

            descriptors.reset();
            ShaderReflectionInfo* compute_shader_reflection = renderer->get_shader_reflection( debug_update_cone_matrices_pipeline.pipeline );
            descriptors.name = "debug_update_cone_matrices_ds";
            descriptors.ssbos.push( { cone_matrices_buffer[ i ], 10 } );

            debug_update_cone_matrices_descriptor_set[ i ] = renderer->create_descriptor_set( descriptors, compute_shader_reflection,
            debug_update_cone_matrices_pipeline.pipeline, 0, *context.render_blackboard );
        }
    }
#endif // DEBUG_DRAW_MESHLET_CONES

#if DEBUG_DRAW_MESHLET_SPHERES || DEBUG_DRAW_MESHLET_CONES
    cone_matrices.shutdown();
    cone_indirect_commands.shutdown();
#endif

//#if DEBUG_DRAW_POINT_LIGHT_SPHERES
//    for ( u32 i = 0; i < scene.active_lights; ++i ) {
//        Light& light = scene.lights[ i ];
//
//        // Meshlet bounding spheres
//        glm::mat4 sphere_bounding_matrix = glms_mat4_identity();
//        sphere_bounding_matrix = glms_translate( sphere_bounding_matrix, light.world_position );
//        sphere_bounding_matrix = glms_scale( sphere_bounding_matrix, glm::vec3{ light.radius, light.radius, light.radius } );
//
//        bounding_matrices.push( sphere_bounding_matrix );
//
//        VkDrawIndexedIndirectCommand draw_command{ };
//        draw_command.indexCount = sphere_index_count;
//        draw_command.instanceCount = 1;
//
//        sphere_indirect_commands.push( draw_command );
//    }
//
//    bounding_sphere_count = bounding_matrices.size;
//
//    {
//        sizet buffer_size = bounding_matrices.size * sizeof( glm::mat4 );
//        //creation.set( VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, ResourceUsageType::Immutable, buffer_size ).set_data( bounding_matrices.data ).set_name( "lights_bounding_spheres_transform" );
//
//        sphere_matrices_buffer = renderer->create_buffer( {
//            .type_flags = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .usage = ResourceUsageType::Immutable,
//             .size = ( u32 )buffer_size, .initial_data = bounding_matrices.data,
//             .name = "lights_bounding_spheres_transform" } );
//    }
//
//    {
//        sizet buffer_size = sphere_indirect_commands.size * sizeof( VkDrawIndexedIndirectCommand );
//        //creation.set( VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, ResourceUsageType::Immutable, buffer_size ).set_data( sphere_indirect_commands.data ).set_name( "lights_bound_sphere_draw_commands" );
//
//        sphere_draw_indirect_buffer = renderer->create_buffer( {
//            .type_flags = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, .usage = ResourceUsageType::Immutable,
//             .size = ( u32 )buffer_size, .initial_data = sphere_indirect_commands.data,
//             .name = "lights_bound_sphere_draw_commands" } );
//    }
//
//#endif
//
#if DEBUG_DRAW_MESHLET_SPHERES || DEBUG_DRAW_MESHLET_CONES
    bounding_matrices.shutdown();
    sphere_indirect_commands.shutdown();
#endif

    // Prepare gpu debug line resources
    //{
    //    // Finalize pass
    //    u32 pass_index = main_technique->get_pass_index( "commands_finalize" );
    //    GpuTechniquePass& pass = main_technique->passes[ pass_index ];
    //    debug_lines_finalize_pipeline = pass.pipeline;
    //    DescriptorSetLayoutHandle layout = renderer->gpu->get_descriptor_set_layout( pass.pipeline, k_material_descriptor_set_index );

    //    descriptors.reset();
    //    debug_lines_finalize_set = scene.create_descriptor_set( descriptors, pass, layout, 0 );

    //    // Draw pass
    //    pass_index = main_technique->get_pass_index( "debug_line_gpu" );
    //    GpuTechniquePass& line_gpu_pass = main_technique->passes[ pass_index ];
    //    debug_lines_draw_pipeline = main_technique->passes[ pass_index ].pipeline;
    //    layout = renderer->gpu->get_descriptor_set_layout( line_gpu_pass.pipeline, k_material_descriptor_set_index );

    //    descriptors.reset();
    //    debug_lines_draw_set = scene.create_descriptor_set( descriptors, line_gpu_pass, layout, 0 );

    //    pass_index = main_technique->get_pass_index( "debug_line_2d_gpu" );
    //    GpuTechniquePass& line_2d_gpu_pass = main_technique->passes[ pass_index ];
    //    debug_lines_2d_draw_pipeline = line_2d_gpu_pass.pipeline;

    //    debug_line_commands_sb_cache = render_blackboard.debug_line_commands_sb;
    //}
}

void DebugPass::destroy_gpu_resources( FrameGraphResourceContext& context ) {
    if ( !enabled )
        return;

    Renderer* renderer = context.renderer;

#if ( DEBUG_DRAW_MESHLET_SPHERES || DEBUG_DRAW_POINT_LIGHT_SPHERES || DEBUG_DRAW_REFLECTION_PROBES )
    renderer->gpu->destroy_buffer( sphere_mesh_indices );
    renderer->gpu->destroy_buffer( sphere_mesh_buffer );
#endif

#if ( DEBUG_DRAW_MESHLET_SPHERES || DEBUG_DRAW_POINT_LIGHT_SPHERES )
    for ( u32 i = 0; i < k_max_frames; ++i ) {
         renderer->gpu->destroy_buffer( sphere_matrices_buffer[ i ] );
         renderer->gpu->destroy_descriptor_set( sphere_mesh_descriptor_set[ i ] );
    }
    renderer->gpu->destroy_buffer( sphere_draw_indirect_buffer );
#endif

#if DEBUG_DRAW_MESHLET_CONES
    renderer->gpu->destroy_buffer( cone_mesh_indices );
    renderer->gpu->destroy_buffer( cone_mesh_buffer );
    for ( u32 i = 0; i < k_max_frames; ++i ) {
        renderer->gpu->destroy_buffer( cone_matrices_buffer[ i ] );
        renderer->gpu->destroy_descriptor_set( cone_mesh_descriptor_set[ i ] );
    }
    renderer->gpu->destroy_buffer( cone_draw_indirect_buffer );
#endif

    renderer->gpu->destroy_descriptor_set( debug_mesh_descriptor_set );
    for ( u32 i = 0; i < k_max_frames; ++i ) {
        renderer->gpu->destroy_descriptor_set( debug_update_sphere_matrices_descriptor_set[ i ] );
        renderer->gpu->destroy_descriptor_set( debug_update_cone_matrices_descriptor_set[ i ] );
    }
    renderer->gpu->destroy_descriptor_set( debug_lines_finalize_set );
    renderer->gpu->destroy_descriptor_set( debug_lines_draw_set );
}

void DebugPass::update_dependent_resources( FrameGraphResourceContext& context ) {

    // TODO(marco): re-implement in update_psos and remove this function
    // GpuDevice& gpu = *renderer->gpu;
    // RenderBlackboard& render_blackboard = *context.render_blackboard;

    // GpuTechnique* technique = renderer->resource_cache.techniques.get( hash_calculate( "ddgi" ) );
    // if ( technique ) {
    //     gpu.destroy_descriptor_set( gi_debug_probes_descriptor_set );

    //     // Probe raytracing
    //     u32 pass_index = technique->get_pass_index( "debug_mesh" );
    //     GpuTechniquePass& pass = technique->passes[ pass_index ];

    //     gi_debug_probes_pipeline = pass.pipeline;

    //     DescriptorSetLayoutHandle layout = gpu.get_descriptor_set_layout( gi_debug_probes_pipeline, k_material_descriptor_set_index );

    //     DescriptorSetBinder descriptors;
    //     descriptors.ssbos.push( { render_blackboard.ddgi_probe_status_cache, 43 } );
    //     descriptors.dynamic_buffers.push( { 55, sizeof( GpuDDGIConstants ) } );

    //     gi_debug_probes_descriptor_set = context.render_scene->create_descriptor_set( descriptors, pass, layout, 0 );
    // }
}

// DebugDrawPass /////////////////////////////////////////////////////////
inline const VertexInputCreation vi_debug_line = {
    .bindings = {
        { 0, 32, VK_VERTEX_INPUT_RATE_INSTANCE }
    },
    .attributes = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
        { 1, 0, VK_FORMAT_R8G8B8A8_UINT, 12 },
        { 2, 0, VK_FORMAT_R32G32B32_SFLOAT, 16 },
        { 3, 0, VK_FORMAT_R8G8B8A8_UINT, 28 },
    }
};

static ShaderCompilationCreation scc_debug_line = {
    .stages = {
        ShaderCompilationStage{
            .source_file_path = "glsl/debug_line.glsl",
            .type = VK_SHADER_STAGE_VERTEX_BIT,
        },
        ShaderCompilationStage{
            .source_file_path = "glsl/debug_line.glsl",
            .type = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    },
    .name = "debug_line_cpu",
    .slang_input = 0,
};

static ShaderCompilationCreation scc_debug_line_2d = {
    .stages = {
        ShaderCompilationStage{
            .source_file_path = "glsl/debug_line.glsl",
            .type = VK_SHADER_STAGE_VERTEX_BIT,
        },
        ShaderCompilationStage{
            .source_file_path = "glsl/debug_line.glsl",
            .type = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    },
    .name = "debug_line_2d_cpu",
    .slang_input = 0,
};

static ShaderCompilationCreation scc_debug_line_gpu = {
    .stages = {
        ShaderCompilationStage{
            .source_file_path = "glsl/debug_line.glsl",
            .type = VK_SHADER_STAGE_VERTEX_BIT,
        },
        ShaderCompilationStage{
            .source_file_path = "glsl/debug_line.glsl",
            .type = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    },
    .name = "debug_line_gpu",
    .slang_input = 0,
};

static ShaderCompilationCreation scc_debug_line_2d_gpu = {
    .stages = {
        ShaderCompilationStage{
            .source_file_path = "glsl/debug_line.glsl",
            .type = VK_SHADER_STAGE_VERTEX_BIT,
        },
        ShaderCompilationStage{
            .source_file_path = "glsl/debug_line.glsl",
            .type = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    },
    .name = "debug_line_2d_gpu",
    .slang_input = 0,
};

static ShaderCompilationCreation scc_commands_finalize = {
    .stages = {
        ShaderCompilationStage{
            .source_file_path = "glsl/debug_line.glsl",
            .type = VK_SHADER_STAGE_COMPUTE_BIT,
        }
    },
    .name = "commands_finalize",
    .slang_input = 0,
};

static PipelineCreation pc_debug_line_cpu = {
    .rasterization = {
        .cull_mode = VK_CULL_MODE_NONE,
        .front = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .fill = VK_POLYGON_MODE_FILL,
    },
    .depth_stencil = {
        .front = {},
        .back = {},
        .depth_comparison = VK_COMPARE_OP_ALWAYS,
        .depth_enable = 1,
        .depth_write_enable = 0,
        .stencil_enable = 0,
    },
    .blend_state = {
        .blend_states = { blend_premultiplied },
    },
    .vertex_input = vi_debug_line,
    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    .flags = 0,

    .render_pass_output = {},
    .shader = {},

    .layout = {},
    .viewport = nullptr,

    .num_active_layouts = 0,
    .num_specialization_constants = 0,

    .name = "debug_line_cpu",
    .render_pass_name = "debug_draw_pass",
};


static PipelineCreation pc_debug_line_gpu = {
    .rasterization = {
        .cull_mode = VK_CULL_MODE_NONE,
        .front = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .fill = VK_POLYGON_MODE_FILL,
    },
    .depth_stencil = {
        .front = {},
        .back = {},
        .depth_comparison = VK_COMPARE_OP_ALWAYS,
        .depth_enable = 1,
        .depth_write_enable = 0,
        .stencil_enable = 0,
    },
    .blend_state = {
        .blend_states = { blend_premultiplied },
    },
    .vertex_input = {},
    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    .flags = 0,

    .render_pass_output = {},
    .shader = {},

    .layout = {},
    .viewport = nullptr,

    .num_active_layouts = 0,
    .num_specialization_constants = 0,

    .name = "debug_line_cpu",
    .render_pass_name = "debug_draw_pass",
};

void DebugDrawPass::declare_frame_graph_node( FrameGraphResourceContext& context ) {

    FrameGraphBuilder& builder = *context.frame_graph->builder;

    context.frame_graph->add_node_v2( {
        .inputs = {
            {
                .type = FrameGraphResourceType_Attachment,
                .handle = builder.get_output_handle( "transparent_pass", "final" )
            },
            {
                .type = FrameGraphResourceType_Attachment,
                .handle = builder.get_output_handle( "gbuffer_pass_early", "depth" )
            },
        },
        .outputs = {
            builder.create_output_reference(
                builder.get_output_handle( "transparent_pass", "final" ),
                FrameGraphResourceType_Reference )
        },
        .scheduling = { CommandQueueType::Graphics, 1 },
        .enabled = true,
        .name = "debug_draw_pass" } );
}

void DebugDrawPass::update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) {

    Renderer* renderer = context.renderer;

    if ( phase == PipelineUpdatePhase::Destroy ) {
        renderer->destroy_graphics_pipeline_state( cpu_line_draw_pipeline );
        renderer->destroy_graphics_pipeline_state( cpu_line_2d_draw_pipeline );
        renderer->destroy_graphics_pipeline_state( gpu_line_draw_pipeline );
        renderer->destroy_graphics_pipeline_state( gpu_line_2d_draw_pipeline );

        renderer->destroy_compute_pipeline_state( gpu_commands_finalize );

        return;
    }

    GraphicsPipelineTransaction transaction( renderer );

    GraphicsPipelineState& new_pipeline = transaction.add( cpu_line_draw_pipeline );
    GraphicsPipelineState& new_pipeline_2d = transaction.add( cpu_line_2d_draw_pipeline );
    GraphicsPipelineState& new_pipeline_gpu = transaction.add( gpu_line_draw_pipeline );
    GraphicsPipelineState& new_pipeline_2d_gpu = transaction.add( gpu_line_2d_draw_pipeline );

    pc_debug_line_cpu.name = "debug_line_cpu";
    renderer->create_graphics_pipeline_state( scc_debug_line, pc_debug_line_cpu,
                                              "debug_line_cpu", context.frame_graph, new_pipeline );

    pc_debug_line_cpu.name = "debug_line_2d_cpu";
    renderer->create_graphics_pipeline_state( scc_debug_line, pc_debug_line_cpu,
                                              "debug_line_2d_cpu", context.frame_graph, new_pipeline_2d );

    pc_debug_line_cpu.name = "debug_line_gpu";
    renderer->create_graphics_pipeline_state( scc_debug_line_gpu, pc_debug_line_gpu,
                                              "debug_line_gpu", context.frame_graph, new_pipeline_gpu );

    pc_debug_line_cpu.name = "debug_line_2d_gpu";
    renderer->create_graphics_pipeline_state( scc_debug_line_2d_gpu, pc_debug_line_gpu,
                                              "debug_line_2d_gpu", context.frame_graph, new_pipeline_2d_gpu );

    transaction.commit_or_rollback();

    ComputePipelineTransaction compute_transaction( renderer );
    ComputePipelineState& new_compute_pipeline = compute_transaction.add( gpu_commands_finalize );

    renderer->create_compute_pipeline_state( scc_commands_finalize, { .name = scc_commands_finalize.name.data },
                                             "debug", context.frame_graph, new_compute_pipeline );
    compute_transaction.commit_or_rollback();
}

void DebugDrawPass::pre_render( FrameGraphRenderContext& context ) {

    Renderer* renderer = context.renderer;
    RenderScene* render_scene = context.render_view->scene;
    u32 current_frame_index = context.current_frame_index;
    CommandBuffer* cb = context.gpu_commands;
    RenderBlackboard& render_blackboard = *context.render_blackboard;
    DebugDrawRuntimeData& debug_draw = context.render_blackboard->debug_draw;

    cb->add_buffer_barrier( debug_draw.gpu_line_commands_sb, 0, VK_WHOLE_SIZE,
                            { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_WRITE_BIT } );
    cb->flush_barriers();

    // Write final command
    cb->bind_pipeline( gpu_commands_finalize.pipeline );
    cb->bind_descriptor_set(
        { renderer->gpu->bindless_descriptor_set, gpu_commands_finalize_descriptor_set },
        {  } );

    cb->dispatch( 1, 1, 1 );

    cb->add_buffer_barrier( debug_draw.gpu_line_commands_sb, 0, VK_WHOLE_SIZE,
                            { VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                              VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT } );

}

void DebugDrawPass::render( FrameGraphRenderContext& context ) {

    Renderer* renderer = context.renderer;
    DebugDrawRuntimeData& debug_draw = context.render_blackboard->debug_draw;
    CommandBuffer* cb = context.gpu_commands;
    RenderBlackboard& render_blackboard = *context.render_blackboard;
    DebugDrawRenderConfig& debug_config = context.render_config->debug_draw;
    GpuDevice& gpu = *renderer->gpu;

    // Reset of the draw counts is done at the beginning of the frame

    if ( debug_config.show_cpu_draws ) {
        // 3D lines
        if ( debug_draw.cpu_lines_count ) {
            cb->bind_pipeline( cpu_line_draw_pipeline.pipeline );
            cb->bind_vertex_buffer( debug_draw.cpu_lines_vb, 0, 0 );
            cb->bind_descriptor_set(
                { renderer->gpu->bindless_descriptor_set, cpu_line_draw_descriptor_set },
                { context.render_blackboard->scene_cb_offset }
            );
            // Draw using instancing and 6 vertices.
            const uint32_t num_vertices = 6;
            cb->draw( TopologyType::Triangle, 0, num_vertices, 0, debug_draw.cpu_lines_count / 2 );
        }

        // 2D lines
        if ( debug_draw.cpu_lines2d_count ) {
            cb->bind_pipeline( cpu_line_2d_draw_pipeline.pipeline );
            cb->bind_vertex_buffer( debug_draw.cpu_lines2d_vb, 0, 0 );
            cb->bind_descriptor_set(
                { renderer->gpu->bindless_descriptor_set, cpu_line_draw_descriptor_set },
                { context.render_blackboard->scene_cb_offset }
            );
            // Draw using instancing and 6 vertices.
            const uint32_t num_vertices = 6;
            cb->draw( TopologyType::Triangle, 0, num_vertices, 0, debug_draw.cpu_lines2d_count / 2 );
        }
    }

    // Draw gpu written debug lines
    if ( debug_config.show_gpu_draws ) {

        cb->bind_pipeline( gpu_line_draw_pipeline.pipeline );
        cb->bind_descriptor_set(
            { renderer->gpu->bindless_descriptor_set, gpu_line_draw_descriptor_set },
            { render_blackboard.scene_cb_offset } );

        cb->draw_indirect( debug_draw.gpu_line_commands_sb, 1, 0, sizeof( VkDrawIndirectCommand ) );
        // Draw 2d lines
        cb->bind_pipeline( gpu_line_2d_draw_pipeline.pipeline );
        cb->bind_descriptor_set(
            { renderer->gpu->bindless_descriptor_set, gpu_line_draw_descriptor_set },
            { render_blackboard.scene_cb_offset } );

        cb->draw_indirect( debug_draw.gpu_line_commands_sb, 1, sizeof( VkDrawIndirectCommand ), sizeof( VkDrawIndirectCommand ) );
    }
}

void DebugDrawPass::create_gpu_resources( FrameGraphResourceContext& context ) {

    Renderer* renderer = context.renderer;
    GpuDevice& gpu = *renderer->gpu;

    DescriptorSetBinder descriptors;

    ShaderReflectionInfo* shader_reflection = renderer->get_shader_reflection( cpu_line_draw_pipeline.pipeline );
    descriptors.reset();
    descriptors.name = "debug_cpu_ds";
    cpu_line_draw_descriptor_set = renderer->create_descriptor_set( descriptors, shader_reflection, cpu_line_draw_pipeline.pipeline, 0, *context.render_blackboard );

    shader_reflection = renderer->get_shader_reflection( gpu_line_draw_pipeline.pipeline );
    descriptors.reset();
    descriptors.name = "debug_gpu_ds";
    gpu_line_draw_descriptor_set = renderer->create_descriptor_set( descriptors, shader_reflection, gpu_line_draw_pipeline.pipeline, 0, *context.render_blackboard );

    shader_reflection = renderer->get_shader_reflection( gpu_commands_finalize.pipeline );
    descriptors.reset();
    descriptors.name = "debug_finalize_ds";
    gpu_commands_finalize_descriptor_set = renderer->create_descriptor_set( descriptors, shader_reflection, gpu_commands_finalize.pipeline, 0, *context.render_blackboard );
}

void DebugDrawPass::destroy_gpu_resources( FrameGraphResourceContext& context ) {
    Renderer* renderer = context.renderer;
    GpuDevice& gpu = *renderer->gpu;

    gpu.destroy_descriptor_set( cpu_line_draw_descriptor_set );
    gpu.destroy_descriptor_set( gpu_line_draw_descriptor_set );
    gpu.destroy_descriptor_set( gpu_commands_finalize_descriptor_set );
}

} // namespace raptor
