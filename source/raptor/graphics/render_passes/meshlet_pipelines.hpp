#pragma once

#include "graphics/gpu_resources.hpp"

namespace raptor {


// depth_pre
static ShaderCompilationCreation scc_meshlet_depth_pre = {
    .stages = {
        ShaderCompilationStage{
            .source_file_path = "meshlet.glsl",
            .headers = { "platform.h", "scene.h", "mesh.h", "meshlet.h" },
            .type = VK_SHADER_STAGE_MESH_BIT_EXT,
        },
        ShaderCompilationStage{
            .source_file_path = "meshlet.glsl",
            .headers = { "platform.h", "scene.h", "mesh.h", "debug_rendering.h", "meshlet.h", "culling.h" },
            .type = VK_SHADER_STAGE_TASK_BIT_EXT,
        },
    },
    .name = "depth_pre",
    .slang_input = 0,
};

static PipelineCreation pc_meshlet_depth_pre = {
    .rasterization = {
        .cull_mode = VK_CULL_MODE_NONE,
        .front     = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .fill      = VK_POLYGON_MODE_FILL,
    },
    .depth_stencil = {
        .front               = {},
        .back                = {},
        .depth_comparison    = VK_COMPARE_OP_LESS_OR_EQUAL,
        .depth_enable        = 1,
        .depth_write_enable  = 1,
        .stencil_enable      = 0,
    },
    .blend_state = {
        .blend_states = {},
    },
    .vertex_input = {},   // mesh shaders: vertex input non usato
    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    .flags = 0,

    .render_pass_output = {},
    .shader = {},

    .layout = {},
    .viewport = nullptr,

    .num_active_layouts = 0,
    .num_specialization_constants = 0,

    .name = "depth_pre",
    .render_pass_name = "depth_pre_pass",
};

// depth_pre_slang

static ShaderCompilationCreation scc_meshlet_depth_pre_slang = {
    .stages = {
        ShaderCompilationStage{
            .source_file_path = "slang/meshlet.slang",
            .type = VK_SHADER_STAGE_MESH_BIT_EXT,
        },
        ShaderCompilationStage{
            .source_file_path = "slang/meshlet.slang",
            .type = VK_SHADER_STAGE_TASK_BIT_EXT,
        },
    },
    .name = "depth_pre_slang",
    .slang_input = 1,
};

static PipelineCreation pc_meshlet_depth_pre_slang = {
    .rasterization = {
        .cull_mode = VK_CULL_MODE_NONE,
        .front     = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .fill      = VK_POLYGON_MODE_FILL,
    },
    .depth_stencil = {
        .front               = {},
        .back                = {},
        .depth_comparison    = VK_COMPARE_OP_LESS_OR_EQUAL,
        .depth_enable        = 1,
        .depth_write_enable  = 1,
        .stencil_enable      = 0,
    },
    .blend_state = {
        .blend_states = {},
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

    .name = "depth_pre_slang",
    .render_pass_name = "depth_pre_pass",
};

// gbuffer_culling

static ShaderCompilationCreation scc_meshlet_gbuffer_culling = {
    .stages = {
        ShaderCompilationStage{
            .source_file_path = "glsl/meshlet.glsl",
            //.headers = { "glsl/platform.h", "glsl/scene.h", "glsl/mesh.h", "glsl/meshlet.h" },
            .type = VK_SHADER_STAGE_MESH_BIT_EXT,
        },
        ShaderCompilationStage{
            .source_file_path = "glsl/meshlet.glsl",
            //.headers = { "glsl/platform.h", "glsl/scene.h", "glsl/mesh.h", "glsl/debug_rendering.h", "glsl/meshlet.h", "glsl/culling.h" },
            .type = VK_SHADER_STAGE_TASK_BIT_EXT,
        },
        ShaderCompilationStage{
            .source_file_path = "glsl/meshlet.glsl",
            //.headers = { "glsl/platform.h", "glsl/scene.h", "glsl/mesh.h", "glsl/meshlet.h" },
            .type = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    },
    .name = "gbuffer_culling",
    .slang_input = 0,
};

static PipelineCreation pc_meshlet_gbuffer_culling = {
    .rasterization = {
        .cull_mode = VK_CULL_MODE_BACK_BIT,
        .front     = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .fill      = VK_POLYGON_MODE_FILL,
    },
    .depth_stencil = {
        .front               = {},
        .back                = {},
        .depth_comparison    = VK_COMPARE_OP_LESS_OR_EQUAL,
        .depth_enable        = 1,
        .depth_write_enable  = 1,
        .stencil_enable      = 0,
    },
    .blend_state = {
        .blend_states = {},
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

    .name = "gbuffer_culling",
    .render_pass_name = "gbuffer_pass_early",
};

// gbuffer_culling_slang

static ShaderCompilationCreation scc_meshlet_gbuffer_culling_slang = {
    .stages = {
        ShaderCompilationStage{
            .source_file_path = "slang/meshlet.slang",
            .type = VK_SHADER_STAGE_MESH_BIT_EXT,
        },
        ShaderCompilationStage{
            .source_file_path = "slang/meshlet.slang",
            .type = VK_SHADER_STAGE_TASK_BIT_EXT,
        },
        ShaderCompilationStage{
            .source_file_path = "slang/meshlet.slang",
            .type = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    },
    .name = "gbuffer_culling_slang",
    .slang_input = 1,
};

static PipelineCreation pc_meshlet_gbuffer_culling_slang = {
    .rasterization = {
        .cull_mode = VK_CULL_MODE_BACK_BIT,
        .front     = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .fill      = VK_POLYGON_MODE_FILL,
    },
    .depth_stencil = {
        .front               = {},
        .back                = {},
        .depth_comparison    = VK_COMPARE_OP_LESS_OR_EQUAL,
        .depth_enable        = 1,
        .depth_write_enable  = 1,
        .stencil_enable      = 0,
    },
    .blend_state = {
        .blend_states = {},
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

    .name = "gbuffer_culling_slang",
    .render_pass_name = "gbuffer_pass_early",
};

// transparent_no_cull

static ShaderCompilationCreation scc_meshlet_transparent_no_cull = {
    .stages = {
        ShaderCompilationStage{
            .source_file_path = "glsl/meshlet.glsl",
            //.headers = { "glsl/platform.h", "glsl/scene.h", "glsl/mesh.h", "glsl/meshlet.h" },
            .type = VK_SHADER_STAGE_MESH_BIT_EXT,
        },
        ShaderCompilationStage{
            .source_file_path = "glsl/meshlet.glsl",
            //.headers = { "glsl/platform.h", "glsl/scene.h", "glsl/mesh.h", "glsl/debug_rendering.h", "glsl/meshlet.h", "glsl/culling.h" },
            .type = VK_SHADER_STAGE_TASK_BIT_EXT,
        },
        ShaderCompilationStage{
            .source_file_path = "glsl/meshlet.glsl",
            //.headers = { "platform.h", "scene.h", "mesh.h", "lighting.h" },
            .type = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    },
    .name = "transparent_no_cull",
    .slang_input = 0,
};

static PipelineCreation pc_meshlet_transparent_no_cull = {
    .rasterization = {
        .cull_mode = VK_CULL_MODE_NONE,
        .front     = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .fill      = VK_POLYGON_MODE_FILL,
    },
    .depth_stencil = {
        .front               = {},
        .back                = {},
        .depth_comparison    = VK_COMPARE_OP_LESS_OR_EQUAL,
        .depth_enable        = 1,
        .depth_write_enable  = 0,   // write: false
        .stencil_enable      = 0,
    },
    .blend_state = {
        .blend_states = {
            BlendState{
                .source_color        = VK_BLEND_FACTOR_SRC_ALPHA,
                .destination_color   = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                .color_operation     = VK_BLEND_OP_ADD,

                .source_alpha        = VK_BLEND_FACTOR_SRC_ALPHA,
                .destination_alpha   = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                .alpha_operation     = VK_BLEND_OP_ADD,

                .color_write_mask    = ColorWriteEnabled::All_mask,

                .blend_disabled      = 0,
                .separate_blend      = 0,
            },
        },
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

    .name = "transparent_no_cull",
    .render_pass_name = "transparent_pass",
};

// mesh

static ShaderCompilationCreation scc_meshlet_mesh = {
    .stages = {
        ShaderCompilationStage{
            .source_file_path = "meshlet.glsl",
            .headers = { "platform.h", "scene.h", "mesh.h", "meshlet.h" },
            .type = VK_SHADER_STAGE_MESH_BIT_EXT,
        },
        ShaderCompilationStage{
            .source_file_path = "meshlet.glsl",
            .headers = { "platform.h", "scene.h", "mesh.h" },
            .type = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    },
    .name = "mesh",
    .slang_input = 0,
};

static PipelineCreation pc_meshlet_mesh = {
    .rasterization = {
        .cull_mode = VK_CULL_MODE_NONE,
        .front     = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .fill      = VK_POLYGON_MODE_FILL,
    },
    .depth_stencil = {
        .front               = {},
        .back                = {},
        .depth_comparison    = VK_COMPARE_OP_LESS_OR_EQUAL,
        .depth_enable        = 1,
        .depth_write_enable  = 1,
        .stencil_enable      = 0,
    },
    .blend_state = {
        .blend_states = {},
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

    .name = "mesh",
    .render_pass_name = "mesh_pass",
};

// generate_meshlet_index_buffer (compute)

static ShaderCompilationCreation scc_generate_meshlet_index_buffer = {
    .stages = {
        ShaderCompilationStage{
            .source_file_path = "meshlet.glsl",
            .headers = { "platform.h", "scene.h", "mesh.h", "meshlet.h" },
            .type = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    },
    .name = "generate_meshlet_index_buffer",
    .slang_input = 0,
};

static PipelineCreation pc_generate_meshlet_index_buffer = {
    .rasterization = {},
    .depth_stencil = {},
    .blend_state   = {},
    .vertex_input  = {},
    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    .flags = 0,

    .render_pass_output = {},
    .shader = {},

    .layout = {},
    .viewport = nullptr,

    .num_active_layouts = 0,
    .num_specialization_constants = 0,

    .name = "generate_meshlet_index_buffer",
    .render_pass_name = "culling_pass",
};

// emulation_gbuffer_culling (vertex/frag)

static ShaderCompilationCreation scc_meshlet_emulation_gbuffer_culling = {
    .stages = {
        ShaderCompilationStage{
            .source_file_path = "meshlet.glsl",
            .headers = { "platform.h", "scene.h", "mesh.h", "meshlet.h" },
            .type = VK_SHADER_STAGE_VERTEX_BIT,
        },
        ShaderCompilationStage{
            .source_file_path = "meshlet.glsl",
            .headers = { "platform.h", "scene.h", "mesh.h", "debug_rendering.h" },
            .type = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    },
    .name = "emulation_gbuffer_culling",
    .slang_input = 0,
};

static PipelineCreation pc_meshlet_emulation_gbuffer_culling = {
    .rasterization = {
        .cull_mode = VK_CULL_MODE_BACK_BIT,
        .front     = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .fill      = VK_POLYGON_MODE_FILL,
    },
    .depth_stencil = {
        .front               = {},
        .back                = {},
        .depth_comparison    = VK_COMPARE_OP_LESS_OR_EQUAL,
        .depth_enable        = 1,
        .depth_write_enable  = 1,
        .stencil_enable      = 0,
    },
    .blend_state = {
        .blend_states = {},
    },
    .vertex_input = {},   // puoi agganciare un layout se serve
    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    .flags = 0,

    .render_pass_output = {},
    .shader = {},

    .layout = {},
    .viewport = nullptr,

    .num_active_layouts = 0,
    .num_specialization_constants = 0,

    .name = "emulation_gbuffer_culling",
    .render_pass_name = "gbuffer_pass_early",
};

// generate_meshlet_instances (compute)

static ShaderCompilationCreation scc_generate_meshlet_instances = {
    .stages = {
        ShaderCompilationStage{
            .source_file_path = "meshlet.glsl",
            .headers = { "platform.h", "scene.h", "mesh.h", "meshlet.h" },
            .type = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    },
    .name = "generate_meshlet_instances",
    .slang_input = 0,
};

static PipelineCreation pc_generate_meshlet_instances = {
    .rasterization = {},
    .depth_stencil = {},
    .blend_state   = {},
    .vertex_input  = {},
    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    .flags = 0,

    .render_pass_output = {},
    .shader = {},

    .layout = {},
    .viewport = nullptr,

    .num_active_layouts = 0,
    .num_specialization_constants = 0,

    .name = "generate_meshlet_instances",
    .render_pass_name = "gbuffer_pass_early",
};

// meshlet_instance_culling (compute)

static ShaderCompilationCreation scc_meshlet_instance_culling = {
    .stages = {
        ShaderCompilationStage{
            .source_file_path = "meshlet.glsl",
            .headers = { "platform.h", "scene.h", "mesh.h", "meshlet.h", "culling.h" },
            .type = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    },
    .name = "meshlet_instance_culling",
    .slang_input = 0,
};

static PipelineCreation pc_meshlet_instance_culling = {
    .rasterization = {},
    .depth_stencil = {},
    .blend_state   = {},
    .vertex_input  = {},
    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    .flags = 0,

    .render_pass_output = {},
    .shader = {},

    .layout = {},
    .viewport = nullptr,

    .num_active_layouts = 0,
    .num_specialization_constants = 0,

    .name = "meshlet_instance_culling",
    .render_pass_name = "gbuffer_pass_early",
};

// meshlet_write_counts (compute)

static ShaderCompilationCreation scc_meshlet_write_counts = {
    .stages = {
        ShaderCompilationStage{
            .source_file_path = "meshlet.glsl",
            .headers = { "platform.h", "scene.h", "mesh.h", "meshlet.h", "culling.h" },
            .type = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    },
    .name = "meshlet_write_counts",
    .slang_input = 0,
};

static PipelineCreation pc_meshlet_write_counts = {
    .rasterization = {},
    .depth_stencil = {},
    .blend_state   = {},
    .vertex_input  = {},
    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    .flags = 0,

    .render_pass_output = {},
    .shader = {},

    .layout = {},
    .viewport = nullptr,

    .num_active_layouts = 0,
    .num_specialization_constants = 0,

    .name = "meshlet_write_counts",
    .render_pass_name = "gbuffer_pass_early",
};

// depth_cubemap

static ShaderCompilationCreation scc_depth_cubemap_meshlet = {
    .stages = {
        ShaderCompilationStage{
            .source_file_path = "meshlet.glsl",
            .headers = { "platform.h", "scene.h", "mesh.h", "meshlet.h" },
            .type = VK_SHADER_STAGE_MESH_BIT_EXT,
        },
        ShaderCompilationStage{
            .source_file_path = "meshlet.glsl",
            .headers = { "platform.h", "scene.h", "mesh.h", "debug_rendering.h", "meshlet.h", "culling.h" },
            .type = VK_SHADER_STAGE_TASK_BIT_EXT,
        },
    },
    .name = "depth_cubemap",
    .slang_input = 0,
};

static PipelineCreation pc_depth_cubemap_meshlet = {
    .rasterization = {
        .cull_mode = VK_CULL_MODE_NONE,
        .front     = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .fill      = VK_POLYGON_MODE_FILL,
    },
    .depth_stencil = {
        .front               = {},
        .back                = {},
        .depth_comparison    = VK_COMPARE_OP_LESS_OR_EQUAL,
        .depth_enable        = 1,
        .depth_write_enable  = 1,
        .stencil_enable      = 0,
    },
    .blend_state = {
        .blend_states = {},
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

    .name = "depth_cubemap",
    .render_pass_name = "point_shadows_pass",
};

// depth_tetrahedron

static ShaderCompilationCreation scc_depth_tetrahedron = {
    .stages = {
        ShaderCompilationStage{
            .source_file_path = "meshlet.glsl",
            .headers = { "platform.h", "scene.h", "mesh.h", "meshlet.h" },
            .type = VK_SHADER_STAGE_MESH_BIT_EXT,
        },
        ShaderCompilationStage{
            .source_file_path = "meshlet.glsl",
            .headers = { "platform.h", "scene.h", "mesh.h", "debug_rendering.h", "meshlet.h", "culling.h" },
            .type = VK_SHADER_STAGE_TASK_BIT_EXT,
        },
    },
    .name = "depth_tetrahedron",
    .slang_input = 0,
};

static PipelineCreation pc_depth_tetrahedron = {
    .rasterization = {
        .cull_mode = VK_CULL_MODE_NONE,
        .front     = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .fill      = VK_POLYGON_MODE_FILL,
    },
    .depth_stencil = {
        .front               = {},
        .back                = {},
        .depth_comparison    = VK_COMPARE_OP_LESS_OR_EQUAL,
        .depth_enable        = 1,
        .depth_write_enable  = 1,
        .stencil_enable      = 0,
    },
    .blend_state = {
        .blend_states = {},
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

    .name = "depth_tetrahedron",
    .render_pass_name = "point_shadows_pass",
};

// meshlet_pointshadows_culling (compute)

static ShaderCompilationCreation scc_meshlet_pointshadows_culling = {
    .stages = {
        ShaderCompilationStage{
            .source_file_path = "meshlet.glsl",
            .headers = { "platform.h", "scene.h", "mesh.h", "meshlet.h", "culling.h", "debug_rendering.h" },
            .type = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    },
    .name = "meshlet_pointshadows_culling",
    .slang_input = 0,
};

static PipelineCreation pc_meshlet_pointshadows_culling = {
    .rasterization = {},
    .depth_stencil = {},
    .blend_state   = {},
    .vertex_input  = {},
    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    .flags = 0,

    .render_pass_output = {},
    .shader = {},

    .layout = {},
    .viewport = nullptr,

    .num_active_layouts = 0,
    .num_specialization_constants = 0,

    .name = "meshlet_pointshadows_culling",
    .render_pass_name = "gbuffer_pass_early",
};

// meshlet_pointshadows_commands_generation (compute)

static ShaderCompilationCreation scc_meshlet_pointshadows_commands_generation = {
    .stages = {
        ShaderCompilationStage{
            .source_file_path = "meshlet.glsl",
            .headers = { "platform.h", "scene.h" },
            .type = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    },
    .name = "meshlet_pointshadows_commands_generation",
    .slang_input = 0,
};

static PipelineCreation pc_meshlet_pointshadows_commands_generation = {
    .rasterization = {},
    .depth_stencil = {},
    .blend_state   = {},
    .vertex_input  = {},
    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    .flags = 0,

    .render_pass_output = {},
    .shader = {},

    .layout = {},
    .viewport = nullptr,

    .num_active_layouts = 0,
    .num_specialization_constants = 0,

    .name = "meshlet_pointshadows_commands_generation",
    .render_pass_name = "gbuffer_pass_early",
};

// pointshadows_resolution_calculation (compute)

static ShaderCompilationCreation scc_pointshadows_resolution_calculation = {
    .stages = {
        ShaderCompilationStage{
            .source_file_path = "meshlet.glsl",
            .headers = { "platform.h", "scene.h" },
            .type = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    },
    .name = "pointshadows_resolution_calculation",
    .slang_input = 0,
};

static PipelineCreation pc_pointshadows_resolution_calculation = {
    .rasterization = {},
    .depth_stencil = {},
    .blend_state   = {},
    .vertex_input  = {},
    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    .flags = 0,

    .render_pass_output = {},
    .shader = {},

    .layout = {},
    .viewport = nullptr,

    .num_active_layouts = 0,
    .num_specialization_constants = 0,

    .name = "pointshadows_resolution_calculation",
    .render_pass_name = "gbuffer_pass_early",
};


}  // namespace raptor
