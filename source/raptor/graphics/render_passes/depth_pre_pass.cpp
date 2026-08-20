#include "graphics/render_passes/depth_pre_pass.hpp"
#include "graphics/render_scene.hpp"
#include "graphics/render_blackboard.hpp"

#include "graphics/render_passes/mesh_vertex_inputs.hpp"

namespace raptor {

ShaderCompilationCreation scc_mesh_depth_pre = {
    .stages = {
        ShaderCompilationStage{
            .source_file_path = "depth.glsl",
            .headers = { "platform.h", "scene.h", "mesh.h" },
            .type = VK_SHADER_STAGE_VERTEX_BIT,
        },
    },
    .name = "mesh_depth_pre",
    .slang_input = 0,
};

PipelineCreation pc_mesh_depth_pre = {
    .rasterization = {
        .cull_mode = VK_CULL_MODE_NONE,
        .front = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .fill = VK_POLYGON_MODE_FILL,
    },
    .depth_stencil = {
        .front = {},
        .back = {},
        .depth_comparison = VK_COMPARE_OP_LESS_OR_EQUAL,
        .depth_enable = 1,
        .depth_write_enable = 1,
        .stencil_enable = 0,
    },
    .blend_state = {
        .blend_states = {}, // default, no blending
    },
    .vertex_input = vi_depth_pre,
    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    .flags = 0,

    .render_pass_output = {},
    .shader = {},

    .layout = {},
    .viewport = nullptr,

    .num_active_layouts = 0,
    .num_specialization_constants = 0,

    .name = "mesh_depth_pre",
    .render_pass_name = "depth_pre_pass",
};

ShaderCompilationCreation scc_mesh_depth_pre_skinning = {
    .stages = {
        ShaderCompilationStage{
            .source_file_path = "depth_skinned.vert",
            .headers = { "platform.h", "scene.h", "mesh.h" },
            .type = VK_SHADER_STAGE_VERTEX_BIT,
        },
        ShaderCompilationStage{
            .source_file_path = "depth.glsl",
            .headers = { "platform.h", "scene.h", "mesh.h" },
            .type = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    },
    .name = "mesh_depth_pre_skinning",
    .slang_input = 0,
};

PipelineCreation pc_mesh_depth_pre_skinning = {
    .rasterization = {
        .cull_mode = VK_CULL_MODE_BACK_BIT,
        .front = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .fill = VK_POLYGON_MODE_FILL,
    },
    .depth_stencil = {
        .front = {},
        .back = {},
        .depth_comparison = VK_COMPARE_OP_LESS_OR_EQUAL,
        .depth_enable = 1,
        .depth_write_enable = 1,
        .stencil_enable = 0,
    },
    .blend_state = {
        .blend_states = {},
    },
    .vertex_input = vi_depth_pre_skinning,
    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    .flags = 0,

    .render_pass_output = {},
    .shader = {},

    .layout = {},
    .viewport = nullptr,

    .num_active_layouts = 0,
    .num_specialization_constants = 0,

    .name = "mesh_depth_pre_skinning",
    .render_pass_name = "depth_pre_pass",
};


//
// DepthPrePass ///////////////////////////////////////////////////////
void DepthPrePass::render( FrameGraphRenderContext& context ) {
    if ( !enabled )
        return;

    CommandBuffer* gpu_commands = context.gpu_commands;
    RenderScene* render_scene = context.render_view->scene;
    u32 current_frame_index = context.current_frame_index;
    RenderBlackboard& render_blackboard = *context.render_blackboard;

    if ( context.render_config->meshlets.use_meshlets ) {
        Renderer* renderer = render_scene->renderer;

        //// Draw meshlets
        //const u64 meshlet_hashed_name = hash_calculate( "meshlet" );
        //GpuTechnique* meshlet_technique = renderer->resource_cache.techniques.get( meshlet_hashed_name );

        //PipelineHandle pipeline = meshlet_technique->passes[ meshlet_technique_index ].pipeline;

        //gpu_commands->bind_pipeline( pipeline );

        //gpu_commands->bind_descriptor_set(
        //    { renderer->gpu->bindless_descriptor_set, render_blackboard.meshlets.meshlets_early_draw_descriptor_set[ current_frame_index ] },
        //    { render_blackboard.scene_cb_offset } );

        //gpu_commands->draw_mesh_task_indirect_count( render_blackboard.gpu_culling.meshlet_indirect_early_commands_sb[ current_frame_index ],
        //    offsetof( GpuMeshDrawCommand, indirectMS ), render_blackboard.gpu_culling.meshlet_indirect_early_commands_sb[ current_frame_index ],
        //    0, render_scene->mesh_instances.size, sizeof( GpuMeshDrawCommand ) );
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
}

void DepthPrePass::create_gpu_resources( FrameGraphResourceContext& context ) {
    
    FrameGraph* frame_graph = context.frame_graph;

    FrameGraphNode* node = frame_graph->get_node( "depth_pre_pass" );
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

    //// Copy all mesh draws and change only material.
    //for ( u32 i = 0; i < scene.mesh_instances.size; ++i ) {

    //    MeshInstance& mesh_instance = scene.mesh_instances[ i ];
    //    Mesh& mesh = scene.meshes[ mesh_instance.mesh_index ];
    //    if ( mesh.is_transparent() ) {
    //        continue;
    //    }

    //    MeshInstanceDraw mesh_instance_draw{};
    //    mesh_instance_draw.mesh_instance = &mesh_instance;
    //    mesh_instance_draw.material_pass_index = mesh.has_skinning() ? main_technique->get_pass_index( "depth_pre_skinning" ) : main_technique->get_pass_index( "depth_pre_slang" );

    //    mesh_instance_draws.push( mesh_instance_draw );
    //}

    GpuDevice& gpu = *renderer->gpu;

    // Cache meshlet technique index
    /*if ( gpu.mesh_shaders_extension_present ) {
        GpuTechnique* main_technique = renderer->resource_cache.techniques.get( hash_calculate( "meshlet" ) );
        meshlet_technique_index = main_technique->get_pass_index( "depth_pre_slang" );
    }*/
}

void DepthPrePass::destroy_gpu_resources( FrameGraphResourceContext& context ) {
    if ( !enabled )
        return;

    mesh_instance_draws.shutdown();
}

} // namespace raptor
