#include "graphics/render_passes/temporal_anti_aliasing_pass.hpp"
#include "graphics/render_scene.hpp"
#include "graphics/render_blackboard.hpp"

#include "foundation/numerics.hpp"

#include "external/glm/vec2.hpp"
#include "../shaders/shared_structs.h"

namespace raptor {

// TemporalAntiAliasingPass ////////////////////////////////////////////////

static ShaderCompilationCreation scc_temporal_anti_aliasing = {
    .stages = {
        {
            .source_file_path = "glsl/temporal_anti_aliasing.glsl",
            .type = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    },
    .name = "temporal_aa",
    .slang_input = 0,
};

void TemporalAntiAliasingPass::declare_frame_graph_node( FrameGraphResourceContext& context ) {

    FrameGraphBuilder& builder = *context.frame_graph->builder;

    context.frame_graph->add_node_v2( {
        .inputs = {
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "transparent_pass", "final" )
            },
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "gbuffer_pass_late", "depth" )
            },
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "motion_vector_pass", "motion_vectors" )
            }
        },
        .outputs = {
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Texture,
                .resource_info{
                    .texture = {}
                },
                .name = "taa_output",
            } ),
        },
        .scheduling = { CommandQueueType::Graphics, 1 },
        .enabled = true,
        .compute = true,
        .name = "temporal_anti_aliasing_pass" } );
}

void TemporalAntiAliasingPass::update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) {

    Renderer* renderer = context.renderer;

    if ( phase == PipelineUpdatePhase::Destroy ) {
        renderer->destroy_compute_pipeline_state( taa_pipeline );

        return;
    }

    ComputePipelineTransaction transaction( renderer );

    PipelineCreation pipeline_creation = {
        .name = "temporal_anti_aliasing",
        .render_pass_name = "temporal_anti_aliasing_pass",
    };

    ComputePipelineState& pipeline = transaction.add( taa_pipeline );

    renderer->create_compute_pipeline_state( scc_temporal_anti_aliasing,
        pipeline_creation, "temporal_anti_aliasing", context.frame_graph, pipeline );

    transaction.commit_or_rollback();
}

void TemporalAntiAliasingPass::create_gpu_resources( FrameGraphResourceContext& context ) {

    FrameGraph* frame_graph = context.frame_graph;

    FrameGraphNode* node = frame_graph->get_node( "temporal_anti_aliasing_pass" );
    if ( node == nullptr ) {
        enabled = false;

        return;
    }

    enabled = node->enabled;

    Renderer* renderer = context.renderer;
    GpuDevice& gpu = *renderer->gpu;
    RenderBlackboard& render_blackboard = *context.render_blackboard;

    ImageCreation texture_creation;
    texture_creation.reset()
        .set_name( "taa_history_texture_0" )
        .set_size( render_blackboard.swapchain_width, render_blackboard.swapchain_height, 1 )
        .set_flags( TextureFlags::Compute_mask )
        .set_format_type( VK_FORMAT_R16G16B16A16_SFLOAT, TextureType::Texture2D );

    history_textures[ 0 ] = gpu.create_image( texture_creation );
    history_image_views[ 0 ] = gpu.create_image_view( {
        .parent_image = history_textures[ 0 ],
        .view_type = VK_IMAGE_VIEW_TYPE_2D,
        .sub_resource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        .name = "taa_history_texture_view_0" } );

    texture_creation.set_name( "taa_history_texture_1" );

    history_textures[ 1 ] = gpu.create_image( texture_creation );
    history_image_views[ 1 ] = gpu.create_image_view( {
        .parent_image = history_textures[ 1 ],
        .view_type = VK_IMAGE_VIEW_TYPE_2D,
        .sub_resource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        .name = "taa_history_texture_view_1" } );

    gpu.add_image_view_to_bindless( history_image_views[ 0 ] );
    gpu.add_image_view_to_bindless( history_image_views[ 1 ] );

    ShaderReflectionInfo* shader_reflection = renderer->get_shader_reflection( taa_pipeline.pipeline );

    DescriptorSetBinder descriptors;
    descriptors.reset();
    descriptors.name = "taa_ds";

    // Dynamic constants buffer.
    // Binding index assumed to be resolved via reflection in create_descriptor_set.
    u32 constants_index = renderer->get_binding_index( shader_reflection, "TaaConstants" );
    descriptors.dynamic_buffers.push( { constants_index, sizeof( GpuTaaConstants ) } );

    taa_descriptor_set = renderer->create_descriptor_set( descriptors, shader_reflection, 
                                                          taa_pipeline.pipeline, 0, *context.render_blackboard );

    current_history_texture_index = 0;
    previous_history_texture_index = 1;
}

void TemporalAntiAliasingPass::destroy_gpu_resources( FrameGraphResourceContext& context ) {

    Renderer* renderer = context.renderer;
    GpuDevice& gpu = *renderer->gpu;

    gpu.destroy_descriptor_set( taa_descriptor_set );

    gpu.destroy_image_view( history_image_views[ 0 ] );
    gpu.destroy_image_view( history_image_views[ 1 ] );

    gpu.destroy_image( history_textures[ 0 ] );
    gpu.destroy_image( history_textures[ 1 ] );
}

void TemporalAntiAliasingPass::on_resize( FrameGraphResourceContext& context, u32 new_width, u32 new_height ) {

    Renderer* renderer = context.renderer;
    GpuDevice& gpu = *renderer->gpu;

    // Special case: use swapchain resolution
    u32 texture_width = context.render_blackboard->swapchain_width;
    u32 texture_height = context.render_blackboard->swapchain_height;

    gpu.resize_image( history_textures[ 0 ], texture_width, texture_height );
    gpu.resize_image( history_textures[ 1 ], texture_width, texture_height );

    gpu.recreate_image_view( history_image_views[ 0 ] );
    gpu.recreate_image_view( history_image_views[ 1 ] );

    gpu.add_image_view_to_bindless( history_image_views[ 0 ] );
    gpu.add_image_view_to_bindless( history_image_views[ 1 ] );
}

void TemporalAntiAliasingPass::upload_gpu_data( FrameGraphResourceContext& context ) {

    if ( !enabled ) {
        return;
    }

    Renderer* renderer = context.renderer;
    GpuDevice& gpu = *renderer->gpu;
    RenderBlackboard& render_blackboard = *context.render_blackboard;

    // Update blackboard
    render_blackboard.taa_output_image_view = history_image_views[ current_history_texture_index ];

    GpuTaaConstants* gpu_constants = gpu.dynamic_buffer_allocate<GpuTaaConstants>( &taa_constants_offset );
    if ( gpu_constants == nullptr ) {
        return;
    }

    TAARenderConfig& taa_config = context.render_config->taa;

    gpu_constants->history_color_texture_index = history_image_views[ previous_history_texture_index ].index();
    gpu_constants->taa_output_texture_index = history_image_views[ current_history_texture_index ].index();
    gpu_constants->velocity_texture_index = render_blackboard.motion_vector_image_view.index();

    FrameGraphResource* final_resource = context.frame_graph->get_resource( "final" );
    RASSERT( final_resource );
    gpu_constants->current_color_texture_index = final_resource->resource_info.texture.image_view.index();

    FrameGraphResource* depth_resource = context.frame_graph->get_resource( "depth" );
    RASSERT( depth_resource );
    gpu_constants->current_depth_texture_index = depth_resource->resource_info.texture.image_view.index();
    gpu_constants->previous_depth_texture_index = depth_resource->resource_info.texture.previous_image_view.index();

    gpu_constants->taa_modes = taa_config.mode;
    gpu_constants->options =
        ( ( taa_config.use_inverse_luminance_filtering ? 1 : 0 ) ) |
        ( ( taa_config.use_temporal_filtering ? 1 : 0 ) << 1 ) |
        ( ( taa_config.use_luminance_difference_filtering ? 1 : 0 ) << 2 ) |
        ( ( taa_config.use_ycocg ? 1 : 0 ) << 3 );

    gpu_constants->current_color_filter = taa_config.current_color_filter;
    gpu_constants->history_sampling_filter = taa_config.history_sampling_filter;
    gpu_constants->history_constraint_mode = taa_config.history_constraint_mode;
    gpu_constants->velocity_sampling_mode = taa_config.velocity_sampling_mode;

    gpu_constants->render_resolution[ 0 ] = ( f32 )render_blackboard.render_width;
    gpu_constants->render_resolution[ 1 ] = ( f32 )render_blackboard.render_height;

    gpu_constants->swapchain_resolution[ 0 ] = ( f32 )render_blackboard.swapchain_width;
    gpu_constants->swapchain_resolution[ 1 ] = ( f32 )render_blackboard.swapchain_height;

    gpu_constants->jitter[ 0 ] = render_blackboard.jitter_offsets.x;
    gpu_constants->jitter[ 1 ] = render_blackboard.jitter_offsets.y;

    gpu_constants->scale_factor = render_blackboard.render_scale_factor;
    gpu_constants->current_sample_sharpness = taa_config.current_sample_sharpness;
}

void TemporalAntiAliasingPass::pre_render( FrameGraphRenderContext& context ) {

    if ( !enabled ) {
        return;
    }

    RenderBlackboard& render_blackboard = *context.render_blackboard;

    previous_history_texture_index = current_history_texture_index;
    current_history_texture_index = ( current_history_texture_index + 1 ) & 1;

    render_blackboard.taa_output_image_view = history_image_views[ current_history_texture_index ];
}

void TemporalAntiAliasingPass::render( FrameGraphRenderContext& context ) {

    if ( !enabled ) {
        return;
    }

    Renderer* renderer = context.renderer;
    CommandBuffer* cb = context.gpu_commands;
    RenderBlackboard& render_blackboard = *context.render_blackboard;
    const u32 current_frame_index = context.current_frame_index;

    VkImageSubresourceRange range = range_aspect( VK_IMAGE_ASPECT_COLOR_BIT,
                                                  0, VK_REMAINING_MIP_LEVELS,
                                                  0, VK_REMAINING_ARRAY_LAYERS );

    BufferHandle dynamic_cb = renderer->gpu->dynamic_buffer;
    cb->add_buffer_barrier(
        dynamic_cb, taa_constants_offset, sizeof( GpuTaaConstants ),
        { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          VK_ACCESS_2_SHADER_READ_BIT } );

    cb->add_image_barrier( history_textures[ current_history_texture_index ], range,
        {
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_IMAGE_LAYOUT_GENERAL
        } );

    cb->add_image_barrier( history_textures[ previous_history_texture_index ], range,
        {
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT,
            VK_IMAGE_LAYOUT_GENERAL
        } );

    cb->flush_barriers();

    cb->bind_pipeline( taa_pipeline.pipeline );
    cb->bind_descriptor_set(
        { renderer->gpu->bindless_descriptor_set, taa_descriptor_set },
        { render_blackboard.scene_cb_offset, taa_constants_offset } );

    const u32 group_x = raptor::ceilu32( render_blackboard.swapchain_width / 8.0f );
    const u32 group_y = raptor::ceilu32( render_blackboard.swapchain_height / 8.0f );
    cb->dispatch( group_x, group_y, 1 );

    cb->add_image_barrier( history_textures[ current_history_texture_index ], range,
        {
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT,
            VK_IMAGE_LAYOUT_GENERAL
        } );

    cb->flush_barriers();
}

void TemporalAntiAliasingPass::update_dependent_resources( FrameGraphResourceContext& context ) {}

} // namespace raptor