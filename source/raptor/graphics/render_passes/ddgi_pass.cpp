#include "graphics/render_passes/ddgi_pass.hpp"
#include "graphics/render_scene.hpp"
#include "graphics/render_blackboard.hpp"

#if 0

#include "foundation/numerics.hpp"
#define GLM_ENABLE_EXPERIMENTAL
#include "external/glm/gtx/euler_angles.hpp"

namespace raptor {

constexpr cstring k_ddgi_step_names[] = {
    "probe_rt",
    "probe_rt_slang",
    "probe_update_irradiance",
    //"probe_update_irradiance_slang",
    "probe_update_visibility",
    //"probe_update_visibility_slang",
    "calculate_probe_offsets",
    //"calculate_probe_offsets_slang",
    "calculate_probe_statuses",
    //"calculate_probe_statuses_slang",
    "sample_irradiance",
    //"sample_irradiance_slang",
    "debug_mesh",
    //"debug_mesh_slang",
};

constexpr cstring k_render_pass_name = "indirect_lighting_pass";


GpuTechniquePassCreation tpc_probe_rt = {
    .shader_compilation = {
        .stages = {
            {
                .source_file_path = "ddgi.glsl",
                .headers = { "platform.h", "scene.h", "mesh.h", "ddgi.h" },
                .type = VK_SHADER_STAGE_RAYGEN_BIT_KHR,
            },
            {
                .source_file_path = "ddgi.glsl",
                .headers = { "platform.h", "scene.h", "mesh.h", "ddgi.h" },
                .type = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
            },
            {
                .source_file_path = "ddgi.glsl",
                .headers = { "platform.h", "ddgi.h" },
                .type = VK_SHADER_STAGE_MISS_BIT_KHR,
            },
        },
        .name = k_ddgi_step_names[ (u32)DDGISteps::ProbeRaytrace ],
        .slang_input = 0,
    },
    .pipeline_creation = {
        .name = k_ddgi_step_names[ (u32)DDGISteps::ProbeRaytrace ],
        .render_pass_name = k_render_pass_name,
    },
};

GpuTechniquePassCreation tpc_probe_rt_slang = {
    .shader_compilation = {
        .stages = {
            {
                .source_file_path = "slang/ddgi.slang",
                .headers = { /* no includes in JSON */ },
                .type = VK_SHADER_STAGE_RAYGEN_BIT_KHR,
            },
            {
                .source_file_path = "slang/ddgi.slang",
                .headers = { /* no includes in JSON */ },
                .type = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
            },
            {
                .source_file_path = "slang/ddgi.slang",
                .headers = { /* no includes in JSON */ },
                .type = VK_SHADER_STAGE_MISS_BIT_KHR,
            },
        },
        .name = k_ddgi_step_names[ (u32)DDGISteps::ProbeRaytraceSlang ],
        .slang_input = 1, // language: "slang"
    },
    .pipeline_creation = {
        .name = k_ddgi_step_names[ (u32)DDGISteps::ProbeRaytraceSlang ],
        .render_pass_name = k_render_pass_name,
    },
};

GpuTechniquePassCreation tpc_probe_update_irradiance = {
    .shader_compilation = {
        .stages = {
            {
                .source_file_path = "ddgi.glsl",
                .headers = { "platform.h", "scene.h", "ddgi.h" },
                .type = VK_SHADER_STAGE_COMPUTE_BIT,
            },
        },
        .name = k_ddgi_step_names[ (u32)DDGISteps::UpdateIrradiance ],
        .slang_input = 0,
    },
    .pipeline_creation = {
        .name = k_ddgi_step_names[ (u32)DDGISteps::UpdateIrradiance ],
        .render_pass_name = k_render_pass_name,
    },
};

GpuTechniquePassCreation tpc_probe_update_visibility = {
    .shader_compilation = {
        .stages = {
            {
                .source_file_path = "ddgi.glsl",
                .headers = { "platform.h", "scene.h", "ddgi.h" },
                .type = VK_SHADER_STAGE_COMPUTE_BIT,
            },
        },
        .name = k_ddgi_step_names[ (u32)DDGISteps::UpdateVisibility ],
        .slang_input = 0,
    },
    .pipeline_creation = {
        .name = k_ddgi_step_names[ (u32)DDGISteps::UpdateVisibility ],
        .render_pass_name = k_render_pass_name,
    },
};

GpuTechniquePassCreation tpc_calculate_probe_offsets = {
    .shader_compilation = {
        .stages = {
            {
                .source_file_path = "ddgi.glsl",
                .headers = { "platform.h", "scene.h", "ddgi.h" },
                .type = VK_SHADER_STAGE_COMPUTE_BIT,
            },
        },
        .name = k_ddgi_step_names[ (u32)DDGISteps::CalculateProbeOffsets ],
        .slang_input = 0,
    },
    .pipeline_creation = {
        .name = k_ddgi_step_names[ (u32)DDGISteps::CalculateProbeOffsets ],
        .render_pass_name = k_render_pass_name,
    },
};

GpuTechniquePassCreation tpc_calculate_probe_statuses = {
    .shader_compilation = {
        .stages = {
            {
                .source_file_path = "ddgi.glsl",
                .headers = { "platform.h", "scene.h", "ddgi.h" },
                .type = VK_SHADER_STAGE_COMPUTE_BIT,
            },
        },
        .name = k_ddgi_step_names[ (u32)DDGISteps::CalculateProbeStatuses ],
        .slang_input = 0,
    },
    .pipeline_creation = {
        .name = k_ddgi_step_names[ (u32)DDGISteps::CalculateProbeStatuses ],
        .render_pass_name = k_render_pass_name,
    },
};

GpuTechniquePassCreation tpc_sample_irradiance = {
    .shader_compilation = {
        .stages = {
            {
                .source_file_path = "ddgi.glsl",
                .headers = { "platform.h", "scene.h", "ddgi.h" },
                .type = VK_SHADER_STAGE_COMPUTE_BIT,
            },
        },
        .name = k_ddgi_step_names[ (u32)DDGISteps::SampleIrradiance ],
        .slang_input = 0,
    },
    .pipeline_creation = {
        .name = k_ddgi_step_names[ (u32)DDGISteps::SampleIrradiance ],
        .render_pass_name = k_render_pass_name,
    },
};

GpuTechniquePassCreation tpc_debug_mesh = {
    .shader_compilation = {
        .stages = {
            {
                .source_file_path = "ddgi.glsl",
                .headers = { "platform.h", "scene.h", "ddgi.h" },
                .type = VK_SHADER_STAGE_VERTEX_BIT,
            },
            {
                .source_file_path = "ddgi.glsl",
                .headers = { "platform.h", "ddgi.h" },
                .type = VK_SHADER_STAGE_FRAGMENT_BIT,
            },
        },
        .name = k_ddgi_step_names[ (u32)DDGISteps::DebugMesh ],
        .slang_input = 0,
    },
    .pipeline_creation = {
        .rasterization = {
            .cull_mode = VK_CULL_MODE_FRONT_BIT
        },
        .depth_stencil = {
            .depth_comparison = VK_COMPARE_OP_LESS_OR_EQUAL,
            .depth_enable = 1,
            .depth_write_enable = 1,
            .stencil_enable = 0,
        },
        .vertex_input = {
            .bindings = { { 0, 20, VK_VERTEX_INPUT_RATE_VERTEX} },
            .attributes = { { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 } }
        },
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .name = k_ddgi_step_names[ (u32)DDGISteps::DebugMesh ],
        .render_pass_name = "debug_pass",
    },
};

GpuTechniquePassCreation tpc_probe_update_irradiance_slang = {
    .shader_compilation = {
        .stages = {
            {
                .source_file_path = "slang/ddgi.slang",
                .headers = { /* no includes in JSON */ },
                .type = VK_SHADER_STAGE_COMPUTE_BIT,
            },
        },
        .name = "probe_update_irradiance_slang",
        .slang_input = 1,
    },
    .pipeline_creation = {
        .name = "probe_update_irradiance_slang",
        .render_pass_name = "indirect_lighting_pass",
    },
};

GpuTechniquePassCreation tpc_probe_update_visibility_slang = {
    .shader_compilation = {
        .stages = {
            {
                .source_file_path = "slang/ddgi.slang",
                .headers = { /* no includes in JSON */ },
                .type = VK_SHADER_STAGE_COMPUTE_BIT,
            },
        },
        .name = "probe_update_visibility_slang",
        .slang_input = 1,
    },
    .pipeline_creation = {
        .name = "probe_update_visibility_slang",
        .render_pass_name = "indirect_lighting_pass",
    },
};

GpuTechniquePassCreation tpc_calculate_probe_offsets_slang = {
    .shader_compilation = {
        .stages = {
            {
                .source_file_path = "slang/ddgi.slang",
                .headers = { /* no includes in JSON */ },
                .type = VK_SHADER_STAGE_COMPUTE_BIT,
            },
        },
        .name = "calculate_probe_offsets_slang",
        .slang_input = 1,
    },
    .pipeline_creation = {
        .name = "calculate_probe_offsets_slang",
        .render_pass_name = "indirect_lighting_pass",
    },
};

GpuTechniquePassCreation tpc_calculate_probe_statuses_slang = {
    .shader_compilation = {
        .stages = {
            {
                .source_file_path = "slang/ddgi.slang",
                .headers = { /* no includes in JSON */ },
                .type = VK_SHADER_STAGE_COMPUTE_BIT,
            },
        },
        .name = "calculate_probe_statuses_slang",
        .slang_input = 1,
    },
    .pipeline_creation = {
        .name = "calculate_probe_statuses_slang",
        .render_pass_name = "indirect_lighting_pass",
    },
};

GpuTechniquePassCreation tpc_sample_irradiance_slang = {
    .shader_compilation = {
        .stages = {
            {
                .source_file_path = "slang/ddgi.slang",
                .headers = { /* no includes in JSON */ },
                .type = VK_SHADER_STAGE_COMPUTE_BIT,
            },
        },
        .name = "sample_irradiance_slang",
        .slang_input = 1,
    },
    .pipeline_creation = {
        .name = "sample_irradiance_slang",
        .render_pass_name = "indirect_lighting_pass",
    },
};

GpuTechniquePassCreation tpc_debug_mesh_slang = {
    .shader_compilation = {
        .stages = {
            {
                .source_file_path = "slang/ddgi.slang",
                .headers = { /* no includes in JSON */ },
                .type = VK_SHADER_STAGE_VERTEX_BIT,
            },
            {
                .source_file_path = "slang/ddgi.slang",
                .headers = { /* no includes in JSON */ },
                .type = VK_SHADER_STAGE_FRAGMENT_BIT,
            },
        },
        .name = "debug_mesh_slang",
        .slang_input = 1,
    },
    .pipeline_creation = {
        .rasterization = {
            .cull_mode = VK_CULL_MODE_FRONT_BIT
        },
        .depth_stencil = {
            .depth_comparison = VK_COMPARE_OP_LESS_OR_EQUAL,
            .depth_enable = 1,
            .depth_write_enable = 1,
            .stencil_enable = 0,
        },
        .vertex_input = {
            .bindings = { { 0, 20, VK_VERTEX_INPUT_RATE_VERTEX} },
            .attributes = { { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 } }
        },
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .name = k_ddgi_step_names[ (u32)DDGISteps::DebugMesh ],
        .render_pass_name = "debug_pass",
    },
};


void DDGIPass::update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) {

    RASSERT( false );
    //GpuTechniqueCreation tc{};
    //tc.name = "ddgi";

    //tc.creations.push( tpc_calculate_probe_offsets );
    //tc.creations.push( tpc_calculate_probe_statuses );
    //tc.creations.push( tpc_debug_mesh );
    //tc.creations.push( tpc_probe_rt );
    //tc.creations.push( tpc_probe_rt_slang );
    //tc.creations.push( tpc_probe_update_irradiance );
    //tc.creations.push( tpc_probe_update_visibility );
    //tc.creations.push( tpc_sample_irradiance );
    ////tc.creations.push( tpc_calculate_probe_offsets_slang );
    ////tc.creations.push( tpc_calculate_probe_statuses_slang );
    ////tc.creations.push( tpc_debug_mesh_slang );
    ////tc.creations.push( tpc_probe_update_irradiance_slang );
    ////tc.creations.push( tpc_probe_update_visibility_slang );
    ////tc.creations.push( tpc_sample_irradiance_slang );

    //tc.name_buffer.init( rkilo( 16 ), resident_allocator );

    //GpuTechnique* technique = loader->create_technique( tc );

    //if ( technique ) {

    //    for ( u32 i = 0; i < (u32)DDGISteps::Count; ++i ) {
    //        pipelines[ i ] = technique->get_pipeline( k_ddgi_step_names[ i ] );
    //    }
    //}

    //tc.name_buffer.shutdown();
}

void DDGIPass::pre_render( FrameGraphRenderContext& context ) {

}

void DDGIPass::render( FrameGraphRenderContext& context ) {

    if ( !enabled )
        return;

    RenderScene* render_scene = context.render_view->scene;
    u32 current_frame_index = context.current_frame_index;
    CommandBuffer* gpu_commands = context.gpu_commands;
    RenderBlackboard& render_blackboard = *context.render_blackboard;

    static i32 offsets_calculations_count = 24;
    if ( context.render_config->gi_recalculate_offsets ) {
        offsets_calculations_count = 24;
    }

    // When calculating offsets, needs all the probes to be updated.
    const u32 probe_count = offsets_calculations_count >= 0 ? get_total_probes() : per_frame_probe_updates;

    // Probe raytrace
    gpu_commands->push_marker( "RT" );
    //gpu_commands->issue_texture_barrier( probe_raytrace_radiance_image, RESOURCE_STATE_UNORDERED_ACCESS, 0, 1 );

    if ( context.render_config->use_slang_shaders ) {
        PipelineHandle pipeline = pipelines[ (u32)DDGISteps::ProbeRaytraceSlang ];
        gpu_commands->bind_pipeline( pipeline );
        gpu_commands->bind_descriptor_set(
            { renderer->gpu->bindless_descriptor_set, probe_raytrace_descriptor_set_slang },
            { render_blackboard.scene_cb_offset, render_blackboard.ddgi_constants_offset } );

        gpu_commands->trace_rays( pipeline, probe_rays, probe_count, 1 );
    }
    else {
        PipelineHandle probe_raytrace_pipeline = pipelines[ (u32)DDGISteps::ProbeRaytrace ];
        gpu_commands->bind_pipeline( probe_raytrace_pipeline );
        gpu_commands->bind_descriptor_set(
            { renderer->gpu->bindless_descriptor_set, probe_raytrace_descriptor_set },
            { render_blackboard.scene_cb_offset, render_blackboard.ddgi_constants_offset } );

        gpu_commands->trace_rays( probe_raytrace_pipeline, probe_rays, probe_count, 1 );
    }

    //gpu_commands->issue_texture_barrier( probe_raytrace_radiance_image, RESOURCE_STATE_UNORDERED_ACCESS, 0, 1 );
    gpu_commands->pop_marker();

    // Calculate probe offsets
    if ( offsets_calculations_count >= 0 ) {
        --offsets_calculations_count;
        gpu_commands->push_marker( "Offsets" );

        PipelineHandle calculate_probe_offset_pipeline = pipelines[ (u32)DDGISteps::CalculateProbeOffsets ];

        //gpu_commands->issue_texture_barrier( probe_offsets_image, RESOURCE_STATE_UNORDERED_ACCESS, 0, 1 );
        gpu_commands->bind_pipeline( calculate_probe_offset_pipeline );
        gpu_commands->bind_descriptor_set(
            { renderer->gpu->bindless_descriptor_set, sample_irradiance_descriptor_set },
            { render_blackboard.scene_cb_offset, render_blackboard.ddgi_constants_offset } );

        u32 first_frame = offsets_calculations_count == 23 ? 1 : 0;
        gpu_commands->push_constants( calculate_probe_offset_pipeline, 0, 4, &first_frame );
        gpu_commands->dispatch( raptor::ceilu32( probe_count / 32.f ), 1, 1 );
        gpu_commands->barrier_instant_compute_write_to_compute_read();
        gpu_commands->pop_marker();
    }

    gpu_commands->push_marker( "Statuses" );

    PipelineHandle calculate_probe_statuses_pipeline = pipelines[ (u32)DDGISteps::CalculateProbeStatuses ];
    //gpu_commands->issue_texture_barrier( probe_offsets_image, RESOURCE_STATE_UNORDERED_ACCESS, 0, 1 );
    gpu_commands->bind_pipeline( calculate_probe_statuses_pipeline );
    gpu_commands->bind_descriptor_set(
        { renderer->gpu->bindless_descriptor_set, sample_irradiance_descriptor_set },
        { render_blackboard.scene_cb_offset, render_blackboard.ddgi_constants_offset } );

    u32 first_frame = 0;
    gpu_commands->push_constants( calculate_probe_statuses_pipeline, 0, 4, &first_frame );
    gpu_commands->dispatch( raptor::ceilu32( probe_count / 32.f ), 1, 1 );
    gpu_commands->barrier_instant_compute_write_to_compute_read();
    gpu_commands->pop_marker();

    gpu_commands->push_marker( "Blend Irr" );
    // Probe grid update: irradiance
    //gpu_commands->issue_texture_barrier( probe_grid_irradiance_image, RESOURCE_STATE_UNORDERED_ACCESS, 0, 1 );

    PipelineHandle probe_grid_update_irradiance_pipeline = pipelines[ (u32)DDGISteps::UpdateIrradiance ];
    gpu_commands->bind_pipeline( probe_grid_update_irradiance_pipeline );
    gpu_commands->bind_descriptor_set(
        { renderer->gpu->bindless_descriptor_set, probe_grid_update_descriptor_set },
        { render_blackboard.scene_cb_offset, render_blackboard.ddgi_constants_offset } );
    gpu_commands->dispatch( raptor::ceilu32( irradiance_atlas_width / 8.f ),
                            raptor::ceilu32( irradiance_atlas_height / 8.f ), 1 );

    gpu_commands->barrier_instant_compute_write_to_compute_read();
    gpu_commands->pop_marker();

    gpu_commands->push_marker( "Blend Vis" );
    // Probe grid update: visibility
    //gpu_commands->issue_texture_barrier( probe_grid_visibility_image, RESOURCE_STATE_UNORDERED_ACCESS, 0, 1 );

    PipelineHandle probe_grid_update_visibility_pipeline = pipelines[ (u32)DDGISteps::UpdateVisibility ];
    gpu_commands->bind_pipeline( probe_grid_update_visibility_pipeline );
    gpu_commands->bind_descriptor_set(
        { renderer->gpu->bindless_descriptor_set, probe_grid_update_descriptor_set },
        { render_blackboard.scene_cb_offset, render_blackboard.ddgi_constants_offset } );
    gpu_commands->dispatch( raptor::ceilu32( visibility_atlas_width / 8.f ),
                            raptor::ceilu32( visibility_atlas_height / 8.f ), 1 );

    //gpu_commands->issue_texture_barrier( probe_grid_irradiance_image, RESOURCE_STATE_UNORDERED_ACCESS, 0, 1 );
    //gpu_commands->issue_texture_barrier( probe_grid_visibility_image, RESOURCE_STATE_UNORDERED_ACCESS, 0, 1 );

    gpu_commands->pop_marker();
    gpu_commands->barrier_instant_compute_write_to_compute_read();

    gpu_commands->push_marker( "Sample Irr" );
    // Sample irradiance
    //gpu_commands->issue_texture_barrier( indirect_image, RESOURCE_STATE_UNORDERED_ACCESS, 0, 1 );

    PipelineHandle sample_irradiance_pipeline = pipelines[ (u32)DDGISteps::SampleIrradiance ];
    gpu_commands->bind_pipeline( sample_irradiance_pipeline );
    gpu_commands->bind_descriptor_set(
        { renderer->gpu->bindless_descriptor_set, sample_irradiance_descriptor_set },
        { render_blackboard.scene_cb_offset, render_blackboard.ddgi_constants_offset } );

    u32 half_resolution = context.render_config->gi_use_half_resolution ? 1 : 0;
    gpu_commands->push_constants( sample_irradiance_pipeline, 0, 4, &half_resolution );

    const f32 resolution_divider = context.render_config->gi_use_half_resolution ? 0.5f : 1.0f;
    gpu_commands->dispatch( raptor::ceilu32( renderer->gpu->swapchain_width * resolution_divider / 8.0f ), raptor::ceilu32( renderer->gpu->swapchain_height * resolution_divider / 8.0f ), 1 );

    //gpu_commands->issue_texture_barrier( indirect_image, RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 0, 1 );
    gpu_commands->pop_marker();
}

void DDGIPass::on_resize( FrameGraphResourceContext& context, u32 new_width, u32 new_height ) {

    if ( !enabled ) {
        return;
    }

    FrameGraph* frame_graph = context.frame_graph;
    GpuDevice& gpu = *renderer->gpu;

    new_width = half_resolution_output ? new_width / 2 : new_width;
    new_height = half_resolution_output ? new_height / 2 : new_height;
    gpu.resize_image( indirect_image, new_width, new_height );
    gpu.recreate_image_view( indirect_image_view );
    gpu.add_image_view_to_bindless( indirect_image_view );
}

void DDGIPass::create_gpu_resources( FrameGraphResourceContext& context ) {
    FrameGraph* frame_graph = context.frame_graph;
    RenderScene* scene = context.render_scene;
    RenderBlackboard& render_blackboard = *context.render_blackboard;

    FrameGraphNode* node = frame_graph->get_node( "indirect_lighting_pass" );
    if ( node == nullptr ) {
        enabled = false;

        return;
    }

    enabled = node->enabled;
    if ( !enabled ) {
        return;
    }

    GpuDevice& gpu = *renderer->gpu;

    per_frame_probe_updates = context.render_config->gi_per_frame_probes_update;

    const u32 num_probes = get_total_probes();
    // Cache count of probes for debug probe spheres drawing.
    context.render_config->gi_total_probes = num_probes;

    //buffer_creation.set( VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, ResourceUsageType::Immutable, sizeof( u32 ) * num_probes ).set_name( "ddgi_probe_status" );
    //ddgi_probe_status_buffer = gpu.create_buffer( {
    //    .type_flags = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .usage = ResourceUsageType::Immutable,
    //    .size = ( u32 )( sizeof( u32 ) * num_probes ), .name = "ddgi_probe_status" } );
    //// Cache status buffer
    //render_blackboard.ddgi_probe_status_cache = ddgi_probe_status_buffer;

    half_resolution_output = context.render_config->gi_use_half_resolution;

    // Create external texture used as pass output.
    // Having normal attachment will cause a crash in vmaCreateAliasingImage.
    ImageCreation texture_creation{ };
    u32 adjusted_width = context.render_config->gi_use_half_resolution ? ( renderer->gpu->swapchain_width ) / 2 : renderer->gpu->swapchain_width;
    u32 adjusted_height = context.render_config->gi_use_half_resolution ? ( renderer->gpu->swapchain_height ) / 2 : renderer->gpu->swapchain_height;
    texture_creation.set_size( adjusted_width, adjusted_height, 1 ).set_format_type( VK_FORMAT_R16G16B16A16_SFLOAT, TextureType::Texture2D ).set_mips( 1 ).set_layers( 1 ).set_flags( TextureFlags::Compute_mask ).set_name( "indirect_texture" );

    indirect_image = gpu.create_image( texture_creation );
    indirect_image_view = gpu.create_image_view( {
            .parent_image = indirect_image, .view_type = VK_IMAGE_VIEW_TYPE_2D,
            .sub_resource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }, .name = texture_creation.name } );
    gpu.add_image_view_to_bindless( indirect_image_view );

    FrameGraphResource* resource = frame_graph->get_resource( "indirect_lighting" );
    resource->resource_info.set_external_texture_2d( adjusted_width, adjusted_height, VK_FORMAT_R16G16B16A16_SFLOAT, 0, indirect_image, indirect_image_view );

    // Radiance texture
    const u32 num_rays = probe_rays;
    texture_creation.set_size( num_rays, num_probes, 1 ).set_format_type( VK_FORMAT_R16G16B16A16_SFLOAT, TextureType::Texture2D )
        .set_flags( TextureFlags::Compute_mask ).set_name( "probe_rt_radiance" );
    probe_raytrace_radiance_image = gpu.create_image( texture_creation );

    probe_raytrace_radiance_image_view = gpu.create_image_view( {
            .parent_image = probe_raytrace_radiance_image, .view_type = VK_IMAGE_VIEW_TYPE_2D,
            .sub_resource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }, .name = texture_creation.name } );
    gpu.add_image_view_to_bindless( probe_raytrace_radiance_image_view );

    // Irradiance texture, 6x6 plus additional 2 pixel border to allow bilinear interpolation
    const i32 octahedral_irradiance_size = irradiance_probe_size + 2;
    irradiance_atlas_width = ( octahedral_irradiance_size * probe_count_x * probe_count_y );
    irradiance_atlas_height = ( octahedral_irradiance_size * probe_count_z );
    texture_creation.set_size( irradiance_atlas_width, irradiance_atlas_height, 1 ).set_name( "probe_irradiance" );
    probe_grid_irradiance_image = gpu.create_image( texture_creation );

    probe_grid_irradiance_image_view = gpu.create_image_view( {
            .parent_image = probe_grid_irradiance_image, .view_type = VK_IMAGE_VIEW_TYPE_2D,
            .sub_resource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }, .name = texture_creation.name } );
    gpu.add_image_view_to_bindless( probe_grid_irradiance_image_view );

    // Visibility texture
    const i32 octahedral_visibility_size = visibility_probe_size + 2;
    visibility_atlas_width = ( octahedral_visibility_size * probe_count_x * probe_count_y );
    visibility_atlas_height = ( octahedral_visibility_size * probe_count_z );
    texture_creation.set_format_type( VK_FORMAT_R16G16_SFLOAT, TextureType::Texture2D ).set_size( visibility_atlas_width, visibility_atlas_height, 1 ).set_name( "probe_visibility" );
    probe_grid_visibility_image = gpu.create_image( texture_creation );

    probe_grid_visibility_image_view = gpu.create_image_view( {
            .parent_image = probe_grid_visibility_image, .view_type = VK_IMAGE_VIEW_TYPE_2D,
            .sub_resource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }, .name = texture_creation.name } );
    gpu.add_image_view_to_bindless( probe_grid_visibility_image_view );

    // Probe offsets texture
    texture_creation.set_format_type( VK_FORMAT_R16G16B16A16_SFLOAT, TextureType::Texture2D ).set_size( probe_count_x * probe_count_y, probe_count_z, 1 ).set_name( "probe_offsets" );
    probe_offsets_image = gpu.create_image( texture_creation );

    probe_offsets_image_view = gpu.create_image_view( {
            .parent_image = probe_offsets_image, .view_type = VK_IMAGE_VIEW_TYPE_2D,
            .sub_resource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }, .name = texture_creation.name } );
    gpu.add_image_view_to_bindless( probe_offsets_image_view );

    // Cache normals texture
    resource = frame_graph->get_resource( "gbuffer_normals" );
    normals_image_view = resource->resource_info.texture.image_view;

    resource = frame_graph->get_resource( "depth" );
    depth_fullscreen_image_view = resource->resource_info.texture.image_view;

    // TODO: at this point this resource is not created still.
    // Use manual assignment in FrameRenderer::upload_gpu_data as occlusion passes.
    //resource = frame_graph->get_resource( "depth_pyramid" );
    //depth_pyramid_texture = resource->resource_info.texture.handle;

    GpuTechnique* technique = renderer->resource_cache.techniques.get( hash_calculate( "ddgi" ) );
    if ( technique ) {
        // Probe raytracing
        u32 pass_index = technique->get_pass_index( k_ddgi_step_names[ (u32)DDGISteps::ProbeRaytrace ] );
        GpuTechniquePass& pass = technique->passes[ pass_index ];

        DescriptorSetLayoutHandle layout = gpu.get_descriptor_set_layout( pipelines[ (u32)DDGISteps::ProbeRaytrace ], k_material_descriptor_set_index);

        DescriptorSetBinder descriptors;
        descriptors.reset();

        descriptors.ssbos.push( { ddgi_probe_status_buffer, ( u16 )pass.get_binding_index( "ProbeStatusSSBO" ) } );
        descriptors.dynamic_buffers.push( { ( u16 )pass.get_binding_index( "DDGIConstants" ), sizeof( GpuDDGIConstants ) } );

        probe_raytrace_descriptor_set = scene->create_descriptor_set( descriptors, pass, layout, 0 );

        // Probe raytracing slang
        pass_index = technique->get_pass_index( k_ddgi_step_names[ (u32)DDGISteps::ProbeRaytraceSlang ] );
        GpuTechniquePass& pass_slang = technique->passes[ pass_index ];

        layout = gpu.get_descriptor_set_layout( pipelines[ (u32)DDGISteps::ProbeRaytraceSlang ], k_material_descriptor_set_index );

        descriptors.reset();

        descriptors.ssbos.push( { ddgi_probe_status_buffer, ( u16 )pass_slang.get_binding_index( "probe_status" ) } );
        descriptors.dynamic_buffers.push( { ( u16 )pass_slang.get_binding_index( "ddgi" ), sizeof( GpuDDGIConstants ) } );

        probe_raytrace_descriptor_set_slang = scene->create_descriptor_set( descriptors, pass_slang, layout, 0 );

        // Probe update irradiance
        pass_index = technique->get_pass_index( k_ddgi_step_names[ (u32)DDGISteps::UpdateIrradiance ] );
        GpuTechniquePass& pass1 = technique->passes[ pass_index ];

        layout = gpu.get_descriptor_set_layout( pipelines[ (u32)DDGISteps::UpdateIrradiance ], k_material_descriptor_set_index );

        descriptors.reset();

        descriptors.dynamic_buffers.push( { ( u16 )pass1.get_binding_index( "DDGIConstants" ), sizeof( GpuDDGIConstants ) } );
        descriptors.ssbos.push( { ddgi_probe_status_buffer, ( u16 )pass1.get_binding_index( "ProbeStatusSSBO" ) } );
        descriptors.images.push( { probe_grid_irradiance_image_view, (u16)pass1.get_binding_index( "irradiance_image" ) } );
        descriptors.images.push( { probe_grid_visibility_image_view, (u16)pass1.get_binding_index( "visibility_image" ) } );

        probe_grid_update_descriptor_set = scene->create_descriptor_set( descriptors, pass1, layout, 0 );

        // Sample irradiance
        pass_index = technique->get_pass_index( k_ddgi_step_names[ (u32)DDGISteps::SampleIrradiance ] );
        GpuTechniquePass& pass5 = technique->passes[ pass_index ];

        layout = gpu.get_descriptor_set_layout( pipelines[ (u32)DDGISteps::SampleIrradiance ], k_material_descriptor_set_index );

        descriptors.reset();

        descriptors.dynamic_buffers.push( { ( u16 )pass5.get_binding_index( "DDGIConstants" ), sizeof( GpuDDGIConstants ) } );
        descriptors.ssbos.push( { ddgi_probe_status_buffer, ( u16 )pass5.get_binding_index( "ProbeStatusSSBO" ) } );

        sample_irradiance_descriptor_set = scene->create_descriptor_set( descriptors, pass5, layout, 0 );
    }
}

void DDGIPass::upload_gpu_data( FrameGraphResourceContext& context ) {

    if ( !enabled )
        return;

    GpuDevice& gpu = *renderer->gpu;
    RenderScene& scene = *context.render_scene;
    RenderBlackboard& render_blackboard = *context.render_blackboard;

    GpuDDGIConstants* gpu_constants = gpu.dynamic_buffer_allocate<GpuDDGIConstants>( &render_blackboard.ddgi_constants_offset );
    if ( gpu_constants ) {
        gpu_constants->radiance_output_index = probe_raytrace_radiance_image_view.index();
        gpu_constants->grid_irradiance_output_index = probe_grid_irradiance_image_view.index();
        gpu_constants->indirect_output_index = indirect_image_view.index();
        gpu_constants->normal_texture_index = normals_image_view.index();

        gpu_constants->depth_pyramid_texture_index = depth_pyramid_texture_index;
        gpu_constants->depth_fullscreen_texture_index = depth_fullscreen_image_view.index();
        gpu_constants->grid_visibility_texture_index = probe_grid_visibility_image_view.index();
        gpu_constants->probe_offset_texture_index = probe_offsets_image_view.index();

        gpu_constants->probe_grid_position = context.render_config->gi_probe_grid_position;
        gpu_constants->probe_sphere_scale = context.render_config->gi_probe_sphere_scale;

        gpu_constants->hysteresis = context.render_config->gi_hysteresis;
        gpu_constants->infinte_bounces_multiplier = context.render_config->gi_infinite_bounces_multiplier;
        gpu_constants->max_probe_offset = context.render_config->gi_max_probe_offset;

        gpu_constants->probe_spacing = context.render_config->gi_probe_spacing;
        gpu_constants->reciprocal_probe_spacing = { 1.f / context.render_config->gi_probe_spacing.x, 1.f / context.render_config->gi_probe_spacing.y, 1.f / context.render_config->gi_probe_spacing.z };
        gpu_constants->self_shadow_bias = context.render_config->gi_self_shadow_bias;

        gpu_constants->probe_counts[ 0 ] = probe_count_x;
        gpu_constants->probe_counts[ 1 ] = probe_count_y;
        gpu_constants->probe_counts[ 2 ] = probe_count_z;
        gpu_constants->debug_options = ( ( context.render_config->gi_debug_border ? 1 : 0 ) )
            | ( ( context.render_config->gi_debug_border_type ? 1 : 0 ) << 1 )
            | ( ( context.render_config->gi_debug_border_source ? 1 : 0 ) << 2 )
            | ( ( context.render_config->gi_use_visibility ? 1 : 0 ) << 3 )
            | ( ( context.render_config->gi_use_backface_smoothing ? 1 : 0 ) << 4 )
            | ( ( context.render_config->gi_use_perceptual_encoding ? 1 : 0 ) << 5 )
            | ( ( context.render_config->gi_use_backface_blending ? 1 : 0 ) << 6 )
            | ( ( context.render_config->gi_use_probe_offsetting ? 1 : 0 ) << 7 )
            | ( ( context.render_config->gi_use_probe_status ? 1 : 0 ) << 8 )
            | ( ( context.render_config->gi_use_infinite_bounces ? 1 : 0 ) << 9 );

        gpu_constants->irradiance_texture_width = irradiance_atlas_width;
        gpu_constants->irradiance_texture_height = irradiance_atlas_height;
        gpu_constants->irradiance_side_length = irradiance_probe_size;
        gpu_constants->probe_rays = probe_rays;

        gpu_constants->visibility_texture_width = visibility_atlas_width;
        gpu_constants->visibility_texture_height = visibility_atlas_height;
        gpu_constants->visibility_side_length = visibility_probe_size;

        gpu_constants->probe_update_offset = probe_update_offset;
        gpu_constants->probe_update_count = per_frame_probe_updates;

        const f32 rotation_scaler = 0.001f;
        gpu_constants->random_rotation = glm::eulerAngleXYZ( get_random_value( -1,1 ) * rotation_scaler, get_random_value( -1,1 ) * rotation_scaler, get_random_value( -1,1 ) * rotation_scaler );

        const u32 num_probes = probe_count_x * probe_count_y * probe_count_z;
        probe_update_offset = ( probe_update_offset + per_frame_probe_updates ) % num_probes;
        per_frame_probe_updates = context.render_config->gi_per_frame_probes_update;
    }
}

void DDGIPass::destroy_gpu_resources( FrameGraphResourceContext& context ) {

    GpuDevice& gpu = *renderer->gpu;

    gpu.destroy_buffer( ddgi_probe_status_buffer );
    gpu.destroy_descriptor_set( probe_raytrace_descriptor_set );
    gpu.destroy_descriptor_set( probe_raytrace_descriptor_set_slang );
    gpu.destroy_descriptor_set( probe_grid_update_descriptor_set );
    gpu.destroy_descriptor_set( sample_irradiance_descriptor_set );

    gpu.destroy_image_view( probe_raytrace_radiance_image_view );
    gpu.destroy_image_view( probe_grid_irradiance_image_view );
    gpu.destroy_image_view( probe_grid_visibility_image_view );
    gpu.destroy_image_view( probe_offsets_image_view );
    gpu.destroy_image_view( indirect_image_view );

    gpu.destroy_image( probe_raytrace_radiance_image );
    gpu.destroy_image( probe_grid_irradiance_image );
    gpu.destroy_image( probe_grid_visibility_image );
    gpu.destroy_image( probe_offsets_image );
    gpu.destroy_image( indirect_image );
}

} // namespace raptor

#endif // 0