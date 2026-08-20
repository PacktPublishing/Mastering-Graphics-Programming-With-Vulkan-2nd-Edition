#pragma once

#include "graphics/gpu_resources.hpp"

namespace raptor {

static ShaderCompilationCreation scc_mesh_gbuffer_no_cull = {
    .stages = {
        ShaderCompilationStage{
            .source_file_path = "gbuffer.glsl",
            .headers = { "platform.h", "scene.h", "mesh.h" },
            .type = VK_SHADER_STAGE_VERTEX_BIT,
        },
        ShaderCompilationStage{
            .source_file_path = "gbuffer.glsl",
            .headers = { "platform.h", "scene.h", "mesh.h" },
            .type = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    },
    .name = "mesh_gbuffer_no_cull",
    .slang_input = 0,
};

static PipelineCreation pc_mesh_gbuffer_no_cull = {
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
        .blend_states = {},
    },
    .vertex_input = vi_gbuffer,
    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    .flags = 0,

    .render_pass_output = {},
    .shader = {},

    .layout = {},
    .viewport = nullptr,

    .num_active_layouts = 0,
    .num_specialization_constants = 0,

    .name = "mesh_gbuffer_no_cull",
    .render_pass_name = "gbuffer_pass_early",
};

static ShaderCompilationCreation scc_mesh_gbuffer_cull = {
    .stages = {
        ShaderCompilationStage{
            .source_file_path = "gbuffer.glsl",
            .headers = { "platform.h", "scene.h", "mesh.h" },
            .type = VK_SHADER_STAGE_VERTEX_BIT,
        },
        ShaderCompilationStage{
            .source_file_path = "gbuffer.glsl",
            .headers = { "platform.h", "scene.h", "mesh.h" },
            .type = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    },
    .name = "mesh_gbuffer_cull",
    .slang_input = 0,
};

static PipelineCreation pc_mesh_gbuffer_cull = {
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
    .vertex_input = vi_gbuffer,
    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    .flags = 0,

    .render_pass_output = {},
    .shader = {},

    .layout = {},
    .viewport = nullptr,

    .num_active_layouts = 0,
    .num_specialization_constants = 0,

    .name = "mesh_gbuffer_cull",
    .render_pass_name = "gbuffer_pass_early",
};

static ShaderCompilationCreation scc_mesh_gbuffer_skinning = {
    .stages = {
        ShaderCompilationStage{
            .source_file_path = "glsl/skinning.glsl",
            // .headers = { "platform.h", "scene.h", "mesh.h" },
            .type = VK_SHADER_STAGE_VERTEX_BIT,
        },
        ShaderCompilationStage{
            .source_file_path = "glsl/meshlet.glsl",
            // .headers = { "platform.h", "scene.h", "mesh.h" },
            .type = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    },
    .name = "gbuffer_skinning",
    .slang_input = 0,
};

static PipelineCreation pc_mesh_gbuffer_skinning = {
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
    .vertex_input = vi_gbuffer_skinning,
    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    .flags = 0,

    .render_pass_output = {},
    .shader = {},

    .layout = {},
    .viewport = nullptr,

    .num_active_layouts = 0,
    .num_specialization_constants = 0,

    .name = "gbuffer_skinning",
    .render_pass_name = "gbuffer_pass_early",
};



static ShaderCompilationCreation scc_mesh_transparent_no_cull = {
    .stages = {
        ShaderCompilationStage{
            .source_file_path = "transparent.glsl",
            .headers = { "platform.h", "scene.h", "mesh.h" },
            .type = VK_SHADER_STAGE_VERTEX_BIT,
        },
        ShaderCompilationStage{
            .source_file_path = "transparent.glsl",
            .headers = { "platform.h", "scene.h", "mesh.h", "lighting.h" },
            .type = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    },
    .name = "mesh_transparent_no_cull",
    .slang_input = 0,
};

static PipelineCreation pc_mesh_transparent_no_cull = {
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
        .depth_write_enable = 0, // write : false
        .stencil_enable = 0,
    },
    .blend_state = {
        .blend_states = {
            BlendState{
                .source_color = VK_BLEND_FACTOR_SRC_ALPHA,
                .destination_color = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                .color_operation = VK_BLEND_OP_ADD,

                .source_alpha = VK_BLEND_FACTOR_SRC_ALPHA,
                .destination_alpha = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                .alpha_operation = VK_BLEND_OP_ADD,

                .color_write_mask = ColorWriteEnabled::All_mask,

                .blend_disabled = 0,
                .separate_blend = 0,
            },
        },
    },
    .vertex_input = vi_gbuffer,
    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    .flags = 0,

    .render_pass_output = {},
    .shader = {},

    .layout = {},
    .viewport = nullptr,

    .num_active_layouts = 0,
    .num_specialization_constants = 0,

    .name = "mesh_transparent_no_cull",
    .render_pass_name = "swapchain",
};

static ShaderCompilationCreation scc_mesh_transparent_cull = {
    .stages = {
        ShaderCompilationStage{
            .source_file_path = "transparent.glsl",
            .headers = { "platform.h", "scene.h", "mesh.h" },
            .type = VK_SHADER_STAGE_VERTEX_BIT,
        },
        ShaderCompilationStage{
            .source_file_path = "transparent.glsl",
            .headers = { "platform.h", "scene.h", "mesh.h", "lighting.h" },
            .type = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    },
    .name = "mesh_transparent_cull",
    .slang_input = 0,
};

static PipelineCreation pc_mesh_transparent_cull = {
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
        .depth_write_enable = 0,
        .stencil_enable = 0,
    },
    .blend_state = {
        .blend_states = {
            BlendState{
                .source_color = VK_BLEND_FACTOR_SRC_ALPHA,
                .destination_color = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                .color_operation = VK_BLEND_OP_ADD,

                .source_alpha = VK_BLEND_FACTOR_SRC_ALPHA,
                .destination_alpha = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                .alpha_operation = VK_BLEND_OP_ADD,

                .color_write_mask = ColorWriteEnabled::All_mask,

                .blend_disabled = 0,
                .separate_blend = 0,
            },
        },
    },
    .vertex_input = vi_gbuffer,
    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    .flags = 0,

    .render_pass_output = {},
    .shader = {},

    .layout = {},
    .viewport = nullptr,

    .num_active_layouts = 0,
    .num_specialization_constants = 0,

    .name = "mesh_transparent_cull",
    .render_pass_name = "swapchain",
};

static ShaderCompilationCreation scc_mesh_transparent_skinning_no_cull = {
    .stages = {
        ShaderCompilationStage{
            .source_file_path = "skinning.glsl",
            .headers = { "platform.h", "scene.h", "mesh.h" },
            .type = VK_SHADER_STAGE_VERTEX_BIT,
        },
        ShaderCompilationStage{
            .source_file_path = "transparent.glsl",
            .headers = { "platform.h", "scene.h", "mesh.h", "lighting.h" },
            .type = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    },
    .name = "mesh_transparent_skinning_no_cull",
    .slang_input = 0,
};

static PipelineCreation pc_mesh_transparent_skinning_no_cull = {
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
        .depth_write_enable = 0,
        .stencil_enable = 0,
    },
    .blend_state = {
        .blend_states = {
            BlendState{
                .source_color = VK_BLEND_FACTOR_SRC_ALPHA,
                .destination_color = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                .color_operation = VK_BLEND_OP_ADD,

                .source_alpha = VK_BLEND_FACTOR_SRC_ALPHA,
                .destination_alpha = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                .alpha_operation = VK_BLEND_OP_ADD,

                .color_write_mask = ColorWriteEnabled::All_mask,

                .blend_disabled = 0,
                .separate_blend = 0,
            },
        },
    },
    .vertex_input = vi_gbuffer_skinning,
    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    .flags = 0,

    .render_pass_output = {},
    .shader = {},

    .layout = {},
    .viewport = nullptr,

    .num_active_layouts = 0,
    .num_specialization_constants = 0,

    .name = "mesh_transparent_skinning_no_cull",
    .render_pass_name = "swapchain",
};

}  // namespace raptor
