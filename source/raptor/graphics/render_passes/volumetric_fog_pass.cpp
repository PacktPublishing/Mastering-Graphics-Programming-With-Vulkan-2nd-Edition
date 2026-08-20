
#include "graphics/render_passes/volumetric_fog_pass.hpp"
#include "graphics/render_scene.hpp"
#include "graphics/render_blackboard.hpp"

#include "foundation/numerics.hpp"

#include "external/glm/vec3.hpp"
#include "../shaders/shared_structs.h"

namespace raptor {

ShaderCompilationCreation scc_inject_data = {
    .stages = {
        {
            .source_file_path = "glsl/volumetric_fog.glsl",
            .type = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    },
    .name = "inject_data",
    .slang_input = 0,
};

PipelineCreation pc_inject_data = {
    .name = "inject_data",
    .render_pass_name = "volumetric_fog_pass",
};

ShaderCompilationCreation scc_inject_data_slang = {
    .stages = {
        {
            .source_file_path = "slang/volumetric_fog.slang",
            .type = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    },
    .name = "inject_data_slang",
    .slang_input = 1,
};

PipelineCreation pc_inject_data_slang = {
    .name = "inject_data_slang",
    .render_pass_name = "volumetric_fog_pass",
};

ShaderCompilationCreation scc_light_scattering = {
    .stages = {
        {
            .source_file_path = "glsl/volumetric_fog.glsl",
            .type = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    },
    .name = "light_scattering",
    .slang_input = 0,
};

PipelineCreation pc_light_scattering = {
    .name = "light_scattering",
    .render_pass_name = "volumetric_fog_pass",
};

ShaderCompilationCreation scc_light_scattering_slang = {
    .stages = {
        {
            .source_file_path = "slang/volumetric_fog.slang",
            .type = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    },
    .name = "light_scattering_slang",
    .slang_input = 1,
};

PipelineCreation pc_light_scattering_slang = {
    .name = "light_scattering_slang",
    .render_pass_name = "volumetric_fog_pass",
};

ShaderCompilationCreation scc_light_integration = {
    .stages = {
        {
            .source_file_path = "glsl/volumetric_fog.glsl",
            .type = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    },
    .name = "light_integration",
    .slang_input = 0,
};

PipelineCreation pc_light_integration = {
    .name = "light_integration",
    .render_pass_name = "volumetric_fog_pass",
};

ShaderCompilationCreation scc_light_integration_slang = {
    .stages = {
        {
            .source_file_path = "slang/volumetric_fog.slang",
            .type = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    },
    .name = "light_integration_slang",
    .slang_input = 1,
};

PipelineCreation pc_light_integration_slang = {
    .name = "light_integration_slang",
    .render_pass_name = "volumetric_fog_pass",
};

ShaderCompilationCreation scc_spatial_filtering = {
    .stages = {
        {
            .source_file_path = "glsl/volumetric_fog.glsl",
            .type = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    },
    .name = "spatial_filtering",
    .slang_input = 0,
};

PipelineCreation pc_spatial_filtering = {
    .name = "spatial_filtering",
    .render_pass_name = "volumetric_fog_pass",
};

ShaderCompilationCreation scc_spatial_filtering_slang = {
    .stages = {
        {
            .source_file_path = "slang/volumetric_fog.slang",
            .type = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    },
    .name = "spatial_filtering_slang",
    .slang_input = 1,
};

PipelineCreation pc_spatial_filtering_slang = {
    .name = "spatial_filtering_slang",
    .render_pass_name = "volumetric_fog_pass",
};

ShaderCompilationCreation scc_temporal_filtering = {
    .stages = {
        {
            .source_file_path = "glsl/volumetric_fog.glsl",
            .type = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    },
    .name = "temporal_filtering",
    .slang_input = 0,
};

PipelineCreation pc_temporal_filtering = {
    .name = "temporal_filtering",
    .render_pass_name = "volumetric_fog_pass",
};

ShaderCompilationCreation scc_temporal_filtering_slang = {
    .stages = {
        {
            .source_file_path = "slang/volumetric_fog.slang",
            .type = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    },
    .name = "temporal_filtering_slang",
    .slang_input = 1,
};

PipelineCreation pc_temporal_filtering_slang = {
    .name = "temporal_filtering_slang",
    .render_pass_name = "volumetric_fog_pass",
};

ShaderCompilationCreation scc_volumetric_noise_baking = {
    .stages = {
        {
            .source_file_path = "glsl/volumetric_fog.glsl",
            .type = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    },
    .name = "volumetric_noise_baking",
    .slang_input = 0,
};

PipelineCreation pc_volumetric_noise_baking = {
    .name = "volumetric_noise_baking",
    .render_pass_name = "volumetric_fog_pass",
};

ShaderCompilationCreation scc_volumetric_noise_baking_slang = {
    .stages = {
        {
            .source_file_path = "slang/volumetric_fog.slang",
            .type = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    },
    .name = "volumetric_noise_baking_slang",
    .slang_input = 1,
};

PipelineCreation pc_volumetric_noise_baking_slang = {
    .name = "volumetric_noise_baking_slang",
    .render_pass_name = "volumetric_fog_pass",
};


void VolumetricFogPass::declare_frame_graph_node( FrameGraphResourceContext& context ) {
    FrameGraphBuilder& builder = *context.frame_graph->builder;

    context.frame_graph->add_node_v2( {
        //.inputs = {
        //    {
        //        .type = FrameGraphResourceType_Texture,
        //        .handle = builder.get_output_handle( "gbuffer_pass_early", "depth" )
        //    }
        //},
        .outputs = {
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Attachment,
                .resource_info = {
                    .external = true
                },
                .name = "volumetric_fog_texture",
            } ),
        },
        .scheduling = { CommandQueueType::Compute, 0 },
        .enabled = true,
        .compute = true,
        .name = "volumetric_fog_pass" } );
}

void VolumetricFogPass::update_psos( FrameGraphResourceContext& context,
                                     PipelineUpdatePhase phase ) {

    Renderer* renderer = context.renderer;

    if ( phase == PipelineUpdatePhase::Destroy ) {

        renderer->destroy_compute_pipeline_state( inject_data_pipeline );
        renderer->destroy_compute_pipeline_state( inject_data_pipeline_slang );

        renderer->destroy_compute_pipeline_state( light_scattering_pipeline );
        renderer->destroy_compute_pipeline_state( light_scattering_pipeline_slang );

        renderer->destroy_compute_pipeline_state( light_integration_pipeline );
        renderer->destroy_compute_pipeline_state( light_integration_pipeline_slang );

        renderer->destroy_compute_pipeline_state( spatial_filtering_pipeline );
        renderer->destroy_compute_pipeline_state( spatial_filtering_pipeline_slang );

        renderer->destroy_compute_pipeline_state( temporal_filtering_pipeline );
        renderer->destroy_compute_pipeline_state( temporal_filtering_pipeline_slang );

        renderer->destroy_compute_pipeline_state( volumetric_noise_baking_pipeline );
        renderer->destroy_compute_pipeline_state( volumetric_noise_baking_pipeline_slang );

        return;
    }

    ComputePipelineTransaction tx( renderer );
    // Inject data
    renderer->create_compute_pipeline_state( scc_inject_data, pc_inject_data, "inject_data",
                                             context.frame_graph, tx.add( inject_data_pipeline ) );

    renderer->create_compute_pipeline_state( scc_inject_data_slang, pc_inject_data_slang, "inject_data",
                                             context.frame_graph, tx.add( inject_data_pipeline_slang ) );
    // Light scattering
    renderer->create_compute_pipeline_state( scc_light_scattering, pc_light_scattering, "light_scattering",
                                             context.frame_graph, tx.add( light_scattering_pipeline ) );

    renderer->create_compute_pipeline_state( scc_light_scattering_slang, pc_light_scattering_slang, "light_scattering",
                                             context.frame_graph, tx.add( light_scattering_pipeline_slang ) );
    // Light integration
    renderer->create_compute_pipeline_state( scc_light_integration, pc_light_integration, "light_integration",
                                             context.frame_graph, tx.add( light_integration_pipeline ) );

    renderer->create_compute_pipeline_state( scc_light_integration_slang, pc_light_integration_slang, "light_integration",
                                             context.frame_graph, tx.add( light_integration_pipeline_slang ) );
    // Spatial filtering
    renderer->create_compute_pipeline_state( scc_spatial_filtering, pc_spatial_filtering, "spatial_filtering",
                                             context.frame_graph, tx.add( spatial_filtering_pipeline ) );

    renderer->create_compute_pipeline_state( scc_spatial_filtering_slang, pc_spatial_filtering_slang, "spatial_filtering",
                                             context.frame_graph, tx.add( spatial_filtering_pipeline_slang ) );
    // Temporal filtering
    renderer->create_compute_pipeline_state( scc_temporal_filtering, pc_temporal_filtering, "temporal_filtering",
                                             context.frame_graph, tx.add( temporal_filtering_pipeline ) );

    renderer->create_compute_pipeline_state( scc_temporal_filtering_slang, pc_temporal_filtering_slang, "temporal_filtering",
                                             context.frame_graph, tx.add( temporal_filtering_pipeline_slang ) );
    // Volumetric noise baking
    renderer->create_compute_pipeline_state( scc_volumetric_noise_baking, pc_volumetric_noise_baking, "volumetric_noise_baking",
                                             context.frame_graph, tx.add( volumetric_noise_baking_pipeline ) );

    renderer->create_compute_pipeline_state( scc_volumetric_noise_baking_slang, pc_volumetric_noise_baking_slang, "volumetric_noise_baking",
                                             context.frame_graph, tx.add( volumetric_noise_baking_pipeline_slang ) );

    tx.commit_or_rollback();
}

void VolumetricFogPass::pre_render( FrameGraphRenderContext& context ) {
    if ( !enabled )
        return;

    RenderScene* render_scene = context.render_view->scene;
    u32 current_frame_index = context.current_frame_index;
    CommandBuffer* gpu_commands = context.gpu_commands;
    RenderBlackboard& render_blackboard = *context.render_blackboard;
    Renderer* renderer = context.renderer;

    VkImageSubresourceRange range_3d = range_aspect( VK_IMAGE_ASPECT_COLOR_BIT,
                                                    0, VK_REMAINING_MIP_LEVELS,
                                                    0, VK_REMAINING_ARRAY_LAYERS );

    static i32 times = 3;
    if ( times >= 0 ) {
        --times;
        has_baked_noise = true;

        gpu_commands->add_image_barrier( volumetric_noise_image, range_3d,
                                        { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                          VK_ACCESS_2_SHADER_WRITE_BIT,
                                          VK_IMAGE_LAYOUT_GENERAL } );

        gpu_commands->flush_barriers();

        if ( context.render_config->use_slang_shaders ) {
            gpu_commands->bind_pipeline( volumetric_noise_baking_pipeline_slang.pipeline );
            gpu_commands->bind_descriptor_set( { renderer->gpu->bindless_descriptor_set },
                                                {  } );
        }
        else {
            gpu_commands->bind_pipeline( volumetric_noise_baking_pipeline.pipeline );
            gpu_commands->bind_descriptor_set( { renderer->gpu->bindless_descriptor_set, fog_descriptor_set },
                                                { render_blackboard.scene_cb_offset, fog_constants_offset } );
        }

        u32 image_view_index = volumetric_noise_image_view.index();
        gpu_commands->push_constants( volumetric_noise_baking_pipeline.pipeline, 0, 4, &image_view_index );
        gpu_commands->dispatch( 64 / 8, 64 / 8, 64 );

        gpu_commands->add_memory_barrier( VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT );

        gpu_commands->flush_barriers();
    }

    // Inject data ///////////////////////////////////////////////////////
    gpu_commands->push_marker( "VolFog Inject" );

    gpu_commands->add_image_barrier( volumetric_noise_image, range_3d,
                                    { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                      VK_ACCESS_2_SHADER_READ_BIT,
                                      VK_IMAGE_LAYOUT_GENERAL } );

    gpu_commands->add_image_barrier( froxel_data_image_0, range_3d,
                                    { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                      VK_ACCESS_2_SHADER_WRITE_BIT,
                                      VK_IMAGE_LAYOUT_GENERAL } );
    gpu_commands->flush_barriers();

    gpu_commands->bind_pipeline( context.render_config->use_slang_shaders ? inject_data_pipeline_slang.pipeline : inject_data_pipeline.pipeline );
    gpu_commands->bind_descriptor_set( { renderer->gpu->bindless_descriptor_set, fog_descriptor_set },
                                        { render_blackboard.scene_cb_offset, fog_constants_offset } );

    const u32 dispatch_group_x = ceilu32( context.render_config->volumetric_fog.tile_count_x / 8.0f );
    const u32 dispatch_group_y = ceilu32( context.render_config->volumetric_fog.tile_count_y / 8.0f );
    gpu_commands->dispatch( dispatch_group_x, dispatch_group_y, context.render_config->volumetric_fog.slices );

    gpu_commands->add_memory_barrier( VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT );
    gpu_commands->flush_barriers();

    gpu_commands->pop_marker();

    // Light scattering //////////////////////////////////////////////////
    gpu_commands->push_marker( "VolFog Light Scattering" );

    ImageHandle temporal_prev = temporal_history_image[ temporal_previous_index ];
    ImageHandle temporal_cur = temporal_history_image[ temporal_current_index ];

    gpu_commands->add_image_barrier( froxel_data_image_0, range_3d,
                                    { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                      VK_ACCESS_2_SHADER_READ_BIT,
                                      VK_IMAGE_LAYOUT_GENERAL } );

    gpu_commands->add_image_barrier( raw_light_scattering_image, range_3d,
                                    { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                      VK_ACCESS_2_SHADER_WRITE_BIT,
                                      VK_IMAGE_LAYOUT_GENERAL } );

    gpu_commands->flush_barriers();

    if ( context.render_config->use_slang_shaders ) {
        gpu_commands->bind_pipeline( light_scattering_pipeline_slang.pipeline );
        gpu_commands->bind_descriptor_set( { renderer->gpu->bindless_descriptor_set, light_scattering_descriptor_set_slang[ current_frame_index ] },
                                            { render_blackboard.scene_cb_offset, render_blackboard.lighting.lighting_constants_cb_offset, fog_constants_offset } );
    }
    else {
        gpu_commands->bind_pipeline( light_scattering_pipeline.pipeline );
        gpu_commands->bind_descriptor_set( { renderer->gpu->bindless_descriptor_set, light_scattering_descriptor_set[ current_frame_index ] },
                                            { render_blackboard.scene_cb_offset, render_blackboard.lighting.lighting_constants_cb_offset, fog_constants_offset } );
    }

    gpu_commands->dispatch( dispatch_group_x, dispatch_group_y, context.render_config->volumetric_fog.slices );

    gpu_commands->add_memory_barrier( VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT );

    gpu_commands->flush_barriers();

    gpu_commands->pop_marker();

    // Spatial filtering /////////////////////////////////////////////////
    gpu_commands->push_marker( "VolFog Spatial" );

    // Reads light scattering texture and writes froxel_data_0
    gpu_commands->add_image_barrier( raw_light_scattering_image, range_3d,
                                    { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                      VK_ACCESS_2_SHADER_READ_BIT,
                                      VK_IMAGE_LAYOUT_GENERAL } );

    gpu_commands->add_image_barrier( froxel_data_image_0, range_3d,
                                    { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                      VK_ACCESS_2_SHADER_WRITE_BIT,
                                      VK_IMAGE_LAYOUT_GENERAL } );

    gpu_commands->flush_barriers();

    gpu_commands->bind_pipeline( context.render_config->use_slang_shaders ? spatial_filtering_pipeline_slang.pipeline : spatial_filtering_pipeline.pipeline );
    gpu_commands->bind_descriptor_set( { renderer->gpu->bindless_descriptor_set, fog_descriptor_set },
                                        { render_blackboard.scene_cb_offset, fog_constants_offset } );
    gpu_commands->dispatch( dispatch_group_x, dispatch_group_y, context.render_config->volumetric_fog.slices );

    gpu_commands->add_memory_barrier( VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT );

    gpu_commands->flush_barriers();

    gpu_commands->pop_marker();

    // VolFog Integration ////////////////////////////////////////////////
    gpu_commands->push_marker( "VolFog Integration" );

    gpu_commands->add_image_barrier( froxel_data_image_0, range_3d,
                                     { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                       VK_ACCESS_2_SHADER_READ_BIT,
                                       VK_IMAGE_LAYOUT_GENERAL } );
    gpu_commands->add_image_barrier( raw_light_scattering_image, range_3d,
                                    { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                      VK_ACCESS_2_SHADER_WRITE_BIT,
                                      VK_IMAGE_LAYOUT_GENERAL } );

    gpu_commands->flush_barriers();
    // Light integration
    gpu_commands->bind_pipeline( context.render_config->use_slang_shaders ? light_integration_pipeline_slang.pipeline : light_integration_pipeline.pipeline );
    gpu_commands->bind_descriptor_set( { renderer->gpu->bindless_descriptor_set, fog_descriptor_set },
                                        { render_blackboard.scene_cb_offset, fog_constants_offset } );

    // NOTE: Z = 1 as we integrate inside the shader.
    gpu_commands->dispatch( dispatch_group_x, dispatch_group_y, 1 );

    gpu_commands->add_memory_barrier( VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT );
    gpu_commands->flush_barriers();
    gpu_commands->pop_marker();


    // VolFog Temporal ///////////////////////////////////////////////////
    gpu_commands->push_marker( "VolFog Temporal" );

    gpu_commands->add_image_barrier( raw_light_scattering_image, range_3d,
                                     { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                       VK_ACCESS_2_SHADER_READ_BIT,
                                       VK_IMAGE_LAYOUT_GENERAL } );

    gpu_commands->add_image_barrier( temporal_prev, range_3d,
                                     { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                       VK_ACCESS_2_SHADER_READ_BIT,
                                       VK_IMAGE_LAYOUT_GENERAL } );

    gpu_commands->add_image_barrier( temporal_cur, range_3d,
                                     { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                       VK_ACCESS_2_SHADER_WRITE_BIT,
                                       VK_IMAGE_LAYOUT_GENERAL } );

    gpu_commands->flush_barriers();
    // Temporal filtering
    // Reads froxel_data_0 and writes light scattering texture
    gpu_commands->bind_pipeline( context.render_config->use_slang_shaders ? temporal_filtering_pipeline_slang.pipeline : temporal_filtering_pipeline.pipeline );
    gpu_commands->bind_descriptor_set( { renderer->gpu->bindless_descriptor_set, fog_descriptor_set },
                                        { render_blackboard.scene_cb_offset, fog_constants_offset } );
    gpu_commands->dispatch( dispatch_group_x, dispatch_group_y, context.render_config->volumetric_fog.slices );

    gpu_commands->add_memory_barrier( VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT );

    gpu_commands->flush_barriers();

    gpu_commands->pop_marker();

    context.render_config->volumetric_fog.texture_index = temporal_cur.index();

}

void VolumetricFogPass::render( FrameGraphRenderContext& context ) {
    if ( !enabled )
        return;
}

void VolumetricFogPass::on_resize( FrameGraphResourceContext& context, u32 new_width, u32 new_height ) {
    if ( !enabled )
        return;

    // TODO: resizable volumetric fog texture
    create_descriptors( context );
}

void VolumetricFogPass::create_gpu_resources( FrameGraphResourceContext& context ) {

    FrameGraph* frame_graph = context.frame_graph;
    RenderScene& scene = *context.render_scene;
    RenderBlackboard& render_blackboard = *context.render_blackboard;

    FrameGraphNode* node = frame_graph->get_node( "volumetric_fog_pass" );
    if ( node == nullptr ) {
        enabled = false;

        return;
    }

    enabled = node->enabled;

    Renderer* renderer = context.renderer;
    GpuDevice& gpu = *renderer->gpu;

    // Calculate froxel dimensions
    context.render_config->volumetric_fog.tile_count_x = ceilu32( ( f32 )render_blackboard.render_width / context.render_config->volumetric_fog.tile_size );
    context.render_config->volumetric_fog.tile_count_y = ceilu32( ( f32 )render_blackboard.render_height / context.render_config->volumetric_fog.tile_size );

    raptor::ImageCreation texture_creation;
    texture_creation.reset().set_size( context.render_config->volumetric_fog.tile_count_x, context.render_config->volumetric_fog.tile_count_y, context.render_config->volumetric_fog.slices )
        .set_format_type( VK_FORMAT_R16G16B16A16_SFLOAT, TextureType::Texture3D ).set_flags( raptor::TextureFlags::Compute_mask ).set_name( "froxel_data_texture_0" );

    froxel_data_image_0 = gpu.create_image( texture_creation );

    froxel_data_image_view_0 = gpu.create_image_view( {
        .parent_image = froxel_data_image_0, .view_type = VK_IMAGE_VIEW_TYPE_3D,
        .sub_resource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }, .name = texture_creation.name } );

    // Temporal reprojection uses those two textures.
    texture_creation.set_name( "vol_fog_temporal_history_0" );
    temporal_history_image[ 0 ] = gpu.create_image( texture_creation );

    temporal_history_image_view[ 0 ] = gpu.create_image_view( {
        .parent_image = temporal_history_image[ 0 ], .view_type = VK_IMAGE_VIEW_TYPE_3D,
        .sub_resource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }, .name = texture_creation.name } );

    texture_creation.set_name( "vol_fog_temporal_history_1" );
    temporal_history_image[ 1 ] = gpu.create_image( texture_creation );

    temporal_history_image_view[ 1 ] = gpu.create_image_view( {
        .parent_image = temporal_history_image[ 1 ], .view_type = VK_IMAGE_VIEW_TYPE_3D,
        .sub_resource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }, .name = texture_creation.name } );

    texture_creation.set_name( "raw_light_scattering_texture" );
    raw_light_scattering_image = gpu.create_image( texture_creation );

    raw_light_scattering_image_view = gpu.create_image_view( {
        .parent_image = raw_light_scattering_image, .view_type = VK_IMAGE_VIEW_TYPE_3D,
        .sub_resource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }, .name = texture_creation.name } );

    // Create volumetric noise texture
    texture_creation.reset().set_size( 64, 64, 64 ).set_format_type( VK_FORMAT_R8_UNORM, TextureType::Texture3D )
        .set_flags( raptor::TextureFlags::Compute_mask ).set_name( "volumetric_noise" );
    volumetric_noise_image = gpu.create_image( texture_creation );

    volumetric_noise_image_view = gpu.create_image_view( {
        .parent_image = volumetric_noise_image, .view_type = VK_IMAGE_VIEW_TYPE_3D,
        .sub_resource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }, .name = texture_creation.name } );

    gpu.add_image_view_to_bindless( froxel_data_image_view_0 );
    gpu.add_image_view_to_bindless( temporal_history_image_view[ 0 ] );
    gpu.add_image_view_to_bindless( temporal_history_image_view[ 1 ] );
    gpu.add_image_view_to_bindless( raw_light_scattering_image_view );
    gpu.add_image_view_to_bindless( volumetric_noise_image_view );

    SamplerCreation fog_sampler_creation;
    fog_sampler_creation.set_address_mode_uvw( VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE )
                        .set_min_mag_mip( VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST )
                        .set_name( "volumetric_fog_linear_sampler" );

    volumetric_fog_sampler = gpu.create_sampler( fog_sampler_creation );

    // Link sampler to all 3D fog images that you sample with texture()/textureLod()
    gpu.link_image_sampler( froxel_data_image_0, volumetric_fog_sampler );
    gpu.link_image_sampler( temporal_history_image[ 0 ], volumetric_fog_sampler );
    gpu.link_image_sampler( temporal_history_image[ 1 ], volumetric_fog_sampler );
    gpu.link_image_sampler( raw_light_scattering_image, volumetric_fog_sampler );

    // Create tiling sampler for volumetric noise texture
    SamplerCreation sampler_creation;
    sampler_creation.set_address_mode_uvw( VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT )
        .set_min_mag_mip( VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR ).set_name( "volumetric_tiling_sampler" );
    volumetric_tiling_sampler = gpu.create_sampler( sampler_creation );
    gpu.link_image_sampler( volumetric_noise_image, volumetric_tiling_sampler );

    // Cache texture index
    //context.render_config->volumetric_fog.texture_index = integrated_light_scattering_image_view.index();

    // NEW
    // Layout for simpler shaders. For now just light scattering needs lighting bindings.
    DescriptorSetLayoutHandle common_layout = gpu.get_descriptor_set_layout( inject_data_pipeline.pipeline, k_material_descriptor_set_index );
    ShaderReflectionInfo* reflection_info = renderer->get_shader_reflection( inject_data_pipeline.pipeline );

    u32 constants_index = renderer->get_binding_index( reflection_info, "VolumetricFogConstants" );

    DescriptorSetBinder descriptors;
    descriptors.dynamic_buffers.push( { constants_index, sizeof( GpuVolumetricFogConstants ) } );
    descriptors.name = "vol_fog_ds";

    fog_descriptor_set = renderer->create_descriptor_set( descriptors, reflection_info, inject_data_pipeline.pipeline, 0, render_blackboard );
    create_descriptors( context );

    // TODO: create methods ?
    FrameGraphResource* resource = ( FrameGraphResource* )frame_graph->get_resource( "volumetric_fog_texture" );
    RASSERT( resource );

    Image* fog_image = gpu.get_image( temporal_history_image[ 0 ] );
    resource->resource_info.set_external_texture_3d(
        fog_image->width, fog_image->height, fog_image->depth,
        texture_creation.format,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
        temporal_history_image[ 0 ],
        temporal_history_image_view[ 0 ]);
}

void VolumetricFogPass::upload_gpu_data( FrameGraphResourceContext& context ) {
    if ( !enabled )
        return;

    Renderer* renderer = context.renderer;
    GpuDevice& gpu = *renderer->gpu;
    RenderScene& scene = *context.render_scene;
    RenderBlackboard* blackboard = context.render_blackboard;

    temporal_previous_index = temporal_current_index;
    temporal_current_index = ( temporal_current_index + 1 ) % 2;

    // Update per mesh material buffer
    // TODO: update only changed stuff, this is now dynamic so it can't be done.
    GpuVolumetricFogConstants* gpu_constants = gpu.dynamic_buffer_allocate<GpuVolumetricFogConstants>( &fog_constants_offset );
    if ( gpu_constants ) {

        //const mat4s& view = scene.frame_data.world_to_camera;
        // TODO: custom near and far for froxels
        //mat4s froxel_ortho = glms_perspective( glm_rad( field_of_view_y ), aspect_ratio, near_plane, far_plane );
        //gpu_constants->froxel_inverse_view_projection = glms_mat4_inv( glms_mat4_mul( projection, view ) );
        // TODO: customize near/far and recalculate projection.
        gpu_constants->froxel_inverse_view_projection = blackboard->main_view.inverse_view_projection;
        gpu_constants->temporal_current_texture_index = temporal_history_image_view[ temporal_current_index ].index();
        gpu_constants->temporal_previous_texture_index = temporal_history_image_view[ temporal_previous_index ].index();
        gpu_constants->froxel_data_texture_index = froxel_data_image_view_0.index();
        gpu_constants->raw_light_scattering_texture_index = raw_light_scattering_image_view.index();

        gpu_constants->froxel_near = blackboard->main_view.near;
        gpu_constants->froxel_far = blackboard->main_view.far;
        gpu_constants->light_scattering_jitter_animated = context.render_config->volumetric_fog.light_scattering_jitter_animated ? 1 : 0;
        gpu_constants->light_scattering_jitter_scale = context.render_config->volumetric_fog.light_scattering_jitter_scale;

        // TODO: add tweakability for this
        gpu_constants->density_modifier = context.render_config->volumetric_fog.density;
        gpu_constants->scattering_factor = context.render_config->volumetric_fog.scattering_factor;
        gpu_constants->temporal_reprojection_percentage = context.render_config->volumetric_fog.temporal_reprojection_percentage;
        gpu_constants->use_temporal_reprojection = context.render_config->volumetric_fog.use_temporal_reprojection ? 1 : 0;
        gpu_constants->time_random_01 = get_random_value( 0.0f, 1.0f );
        gpu_constants->phase_anisotropy_01 = context.render_config->volumetric_fog.phase_anisotropy_01;

        gpu_constants->froxel_dimensions.x = context.render_config->volumetric_fog.tile_count_x;
        gpu_constants->froxel_dimensions.y = context.render_config->volumetric_fog.tile_count_y;
        gpu_constants->froxel_dimensions.z = context.render_config->volumetric_fog.slices;
        gpu_constants->phase_function_type = context.render_config->volumetric_fog.phase_function_type;

        gpu_constants->height_fog_density = context.render_config->volumetric_fog.height_fog_density;
        gpu_constants->height_fog_falloff = context.render_config->volumetric_fog.height_fog_falloff;
        gpu_constants->noise_scale = context.render_config->volumetric_fog.noise_scale;
        gpu_constants->lighting_noise_scale = context.render_config->volumetric_fog.lighting_noise_scale;
        gpu_constants->noise_type = context.render_config->volumetric_fog.noise_type;
        gpu_constants->use_spatial_filtering = context.render_config->volumetric_fog.use_spatial_filtering;
        gpu_constants->temporal_reprojection_jitter_scale = context.render_config->volumetric_fog.temporal_reprojection_jittering_scale;

        gpu_constants->volumetric_noise_texture_index = volumetric_noise_image_view.index();
        gpu_constants->volumetric_noise_position_multiplier = context.render_config->volumetric_fog.noise_position_scale;
        gpu_constants->volumetric_noise_speed_multiplier = context.render_config->volumetric_fog.noise_speed_scale * 0.001f;

        gpu_constants->box_color = context.render_config->volumetric_fog.box_color;
        gpu_constants->box_fog_density = context.render_config->volumetric_fog.box_density;
        gpu_constants->box_position = context.render_config->volumetric_fog.box_position;
        gpu_constants->box_half_size = context.render_config->volumetric_fog.box_size * 0.5f;
    }
}

void VolumetricFogPass::destroy_gpu_resources( FrameGraphResourceContext& context ) {

    Renderer* renderer = context.renderer;
    GpuDevice& gpu = *renderer->gpu;

    gpu.destroy_image( froxel_data_image_0 );
    gpu.destroy_image( temporal_history_image[ 0 ] );
    gpu.destroy_image( temporal_history_image[ 1 ] );
    gpu.destroy_image( raw_light_scattering_image );
    gpu.destroy_image( volumetric_noise_image );

    gpu.destroy_image_view( froxel_data_image_view_0 );
    gpu.destroy_image_view( temporal_history_image_view[ 0 ] );
    gpu.destroy_image_view( temporal_history_image_view[ 1 ] );
    gpu.destroy_image_view( raw_light_scattering_image_view );
    gpu.destroy_image_view( volumetric_noise_image_view );

    for ( u32 i = 0; i < k_max_frames; ++i ) {
        gpu.destroy_descriptor_set( light_scattering_descriptor_set[ i ] );
        gpu.destroy_descriptor_set( light_scattering_descriptor_set_slang[ i ] );
    }

    gpu.destroy_sampler( volumetric_tiling_sampler );
    gpu.destroy_sampler( volumetric_fog_sampler );

    gpu.destroy_descriptor_set( fog_descriptor_set );
}

void VolumetricFogPass::update_dependent_resources( FrameGraphResourceContext& context ) {
    if ( !enabled )
        return;

    create_descriptors( context );
}

void VolumetricFogPass::create_descriptors( FrameGraphResourceContext& context ) {

    Renderer* renderer = context.renderer;
    GpuDevice* gpu = renderer->gpu;
    RenderBlackboard& render_blackboard = *context.render_blackboard;

    ShaderReflectionInfo* reflection_info = renderer->get_shader_reflection( light_scattering_pipeline.pipeline );
    DescriptorSetLayoutHandle light_scattering_layout = gpu->get_descriptor_set_layout( light_scattering_pipeline.pipeline, k_material_descriptor_set_index );
    DescriptorSetBinder descriptors;

    for ( u32 i = 0; i < k_max_frames; ++i ) {
        gpu->destroy_descriptor_set( light_scattering_descriptor_set[ i ] );
        gpu->destroy_descriptor_set( light_scattering_descriptor_set_slang[ i ] );
    }

    u32 constants_index = renderer->get_binding_index( reflection_info, "VolumetricFogConstants" );

    for ( u32 i = 0; i < k_max_frames; ++i ) {

        gpu->destroy_descriptor_set( light_scattering_descriptor_set[ i ] );

        descriptors.reset();
        descriptors.dynamic_buffers.push( { constants_index, sizeof( GpuVolumetricFogConstants ) } );
        descriptors.name = "vol_fog_scattering_ds";

        light_scattering_descriptor_set[ i ] = renderer->create_descriptor_set( descriptors, reflection_info, light_scattering_pipeline.pipeline, i, render_blackboard );
    }

    reflection_info = renderer->get_shader_reflection( light_scattering_pipeline_slang.pipeline );
    light_scattering_layout = gpu->get_descriptor_set_layout( light_scattering_pipeline_slang.pipeline, k_material_descriptor_set_index );

    for ( u32 i = 0; i < k_max_frames; ++i ) {

        gpu->destroy_descriptor_set( light_scattering_descriptor_set_slang[ i ] );

        descriptors.reset();
        descriptors.dynamic_buffers.push( { constants_index, sizeof( GpuVolumetricFogConstants ) } );
        descriptors.name = "vol_fog_scattering_slang_ds";

        light_scattering_descriptor_set_slang[ i ] = renderer->create_descriptor_set( descriptors, reflection_info, light_scattering_pipeline_slang.pipeline, i, render_blackboard );
    }
}

} // namespace raptor
