#include "graphics/render_passes/svgf_pass.hpp"
#include "graphics/render_scene.hpp"
#include "graphics/render_blackboard.hpp"

#include "foundation/numerics.hpp"

namespace raptor {

static void cache_svgf_common_resources( FrameGraph* frame_graph, SVGFCommonResources* resources ) {

    FrameGraphResource* resource = frame_graph->get_resource( "gbuffer_normals" );
    resources->normals_texture = resource->resource_info.texture.image;
    resources->normals_image_view = resource->resource_info.texture.image_view;

    resource = frame_graph->get_resource( "depth" );
    resources->depth_texture = resource->resource_info.texture.image;
    resources->depth_image_view = resource->resource_info.texture.image_view;

    resource = frame_graph->get_resource( "mesh_id" );
    resources->mesh_id_texture = resource->resource_info.texture.image;
    resources->mesh_id_image_view = resource->resource_info.texture.image_view;

    resource = frame_graph->get_resource( "motion_vectors" );
    resources->motion_vectors_texture = resource->resource_info.texture.image;
    resources->motion_vectors_image_view = resource->resource_info.texture.image_view;

    resource = frame_graph->get_resource( "reflections" );
    resources->reflections_texture = resource->resource_info.texture.image;
    resources->reflections_image_view = resource->resource_info.texture.image_view;

    resource = frame_graph->get_resource( "restirgi_output" );
    resources->restirgi_output_texture = resource->resource_info.texture.image;
    resources->restirgi_output_image_view = resource->resource_info.texture.image_view;

    resource = frame_graph->get_resource( "z_normal_fwidth" );
    resources->depth_normal_fwidth_texture = resource->resource_info.texture.image;
    resources->depth_normal_fwidth_image_view = resource->resource_info.texture.image_view;

    resource = frame_graph->get_resource( "linear_z_dd" );
    resources->linear_z_dd_texture = resource->resource_info.texture.image;
    resources->linear_z_dd_image_view = resource->resource_info.texture.image_view;

    resource = frame_graph->get_resource( "integrated_reflection_color" );
    resources->integrated_reflection_color_texture = resource->resource_info.texture.image;
    resources->integrated_reflection_color_image_view = resource->resource_info.texture.image_view;

    resource = frame_graph->get_resource( "integrated_reflection_moments" );
    resources->integrated_reflection_moments_texture = resource->resource_info.texture.image;
    resources->integrated_reflection_moments_image_view = resource->resource_info.texture.image_view;

    resource = frame_graph->get_resource( "integrated_restirgi_color" );
    resources->integrated_restirgi_color_texture = resource->resource_info.texture.image;
    resources->integrated_restirgi_color_image_view = resource->resource_info.texture.image_view;

    resource = frame_graph->get_resource( "integrated_restirgi_moments" );
    resources->integrated_restirgi_moments_texture = resource->resource_info.texture.image;
    resources->integrated_restirgi_moments_image_view = resource->resource_info.texture.image_view;
}

static void cache_svgf_accumulation_resources( FrameGraph* frame_graph, SVGFAccumulationOutput* resources ) {

    FrameGraphResource* resource = frame_graph->get_resource( "reflections_history" );
    resources->reflections_history_texture = resource->resource_info.texture.image;
    resources->reflections_history_image_view = resource->resource_info.texture.image_view;

    resource = frame_graph->get_resource( "reflections_moments_history" );
    resources->reflections_moments_history_texture = resource->resource_info.texture.image;
    resources->reflections_moments_history_image_view = resource->resource_info.texture.image_view;

    resource = frame_graph->get_resource( "restirgi_history" );
    resources->restirgi_history_texture = resource->resource_info.texture.image;
    resources->restirgi_history_image_view = resource->resource_info.texture.image_view;

    resource = frame_graph->get_resource( "restirgi_moments_history" );
    resources->restirgi_moments_history_texture = resource->resource_info.texture.image;
    resources->restirgi_moments_history_image_view = resource->resource_info.texture.image_view;

    resource = frame_graph->get_resource( "normals_history" );
    resources->last_frame_normals_texture = resource->resource_info.texture.image;
    resources->last_frame_normals_image_view = resource->resource_info.texture.image_view;

    resource = frame_graph->get_resource( "mesh_id_history" );
    resources->last_frame_mesh_id_texture = resource->resource_info.texture.image;
    resources->last_frame_mesh_id_image_view = resource->resource_info.texture.image_view;

    resource = frame_graph->get_resource( "linear_depth_history" );
    resources->last_frame_linear_depth_texture = resource->resource_info.texture.image;
    resources->last_frame_linear_depth_image_view = resource->resource_info.texture.image_view;
}

static void cache_svgf_current_guide_resources( FrameGraph* frame_graph, SVGFGuideResources* resources ) {

    FrameGraphResource* resource = frame_graph->get_resource( "svgf_current_normals" );
    resources->normals_texture = resource->resource_info.texture.image;
    resources->normals_image_view = resource->resource_info.texture.image_view;

    resource = frame_graph->get_resource( "svgf_current_mesh_id" );
    resources->mesh_id_texture = resource->resource_info.texture.image;
    resources->mesh_id_image_view = resource->resource_info.texture.image_view;

    resource = frame_graph->get_resource( "svgf_current_linear_depth" );
    resources->linear_depth_texture = resource->resource_info.texture.image;
    resources->linear_depth_image_view = resource->resource_info.texture.image_view;

    resource = frame_graph->get_resource( "svgf_current_depth_normal_fwidth" );
    resources->depth_normal_fwidth_texture = resource->resource_info.texture.image;
    resources->depth_normal_fwidth_image_view = resource->resource_info.texture.image_view;

    resource = frame_graph->get_resource( "svgf_current_motion_vectors" );
    resources->motion_vectors_texture = resource->resource_info.texture.image;
    resources->motion_vectors_image_view = resource->resource_info.texture.image_view;
}

struct SVGFGpuConstants {
    u32 motion_vectors_texture_index;
    u32 mesh_id_texture_index;
    u32 normals_texture_index;
    u32 depth_normal_fwidth_texture_index;

    // Current half-res guides
    u32 current_motion_vectors_texture_index;
    u32 current_mesh_id_texture_index;
    u32 current_normals_texture_index;
    u32 current_depth_normal_fwidth_texture_index;

    u32 current_linear_z_dd_texture_index;
    u32 history_mesh_id_texture_index;
    u32 history_normals_texture_index;
    u32 history_linear_depth_texture;

    u32 output_texture_index;
    u32 history_output_texture_index;
    u32 history_moments_texture_index;
    u32 integrated_color_texture_index;

    u32 integrated_moments_texture_index;
    u32 variance_texture_index;
    u32 filtered_color_texture_index;
    u32 updated_variance_texture_index;

    u32 linear_z_dd_texture_index;
    f32 output_resolution_scale;
    f32 output_resolution_scale_rcp;
    f32 temporal_depth_difference;

    f32 temporal_normal_difference;
    f32 input_resolution_scale;
    f32 input_resolution_scale_rcp;
    f32 pad002;
};

struct SVGFPushConstants {
    u32         step_size = 1;
    f32         sigma_z = 1.0;
    f32         sigma_n = 128.0;
    f32         sigma_l = 4.0;
};


// SVGFGuideDownsamplePass ///////////////////////////////////////////////
void SVGFGuideDownsamplePass::declare_frame_graph_node( FrameGraphResourceContext& context ) {
    FrameGraphBuilder& builder = *context.frame_graph->builder;
    renderer = context.renderer;

    RenderBlackboard& render_blackboard = *context.render_blackboard;
    texture_scale = context.render_config->raytraced_reflections.reflections_scale;

    const u32 half_width = ceilu32( render_blackboard.render_width * texture_scale );
    const u32 half_height = ceilu32( render_blackboard.render_height * texture_scale );

    context.frame_graph->add_node_v2( {
        .inputs = {
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "gbuffer_pass_late", "depth" )
            },
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "gbuffer_pass_late", "gbuffer_normals" )
            },
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "gbuffer_pass_late", "mesh_id" )
            },
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "gbuffer_pass_late", "linear_z_dd" )
            },
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "gbuffer_pass_late", "z_normal_fwidth" )
            },
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "motion_vector_pass", "visibility_motion_vectors" )
            },
        },
        .outputs = {
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Attachment,
                .resource_info = {
                    .texture = {
                        .width = half_width,
                        .height = half_height,
                        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                        .flags = TextureFlags::Compute_mask,
                        .compute = true
                    }
                },
                .name = "svgf_current_normals",
            } ),
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Attachment,
                .resource_info = {
                    .texture = {
                        .width = half_width,
                        .height = half_height,
                        .format = VK_FORMAT_R32_UINT,
                        .flags = TextureFlags::Compute_mask,
                        .compute = true
                    }
                },
                .name = "svgf_current_mesh_id",
            } ),
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Attachment,
                .resource_info = {
                    .texture = {
                        .width = half_width,
                        .height = half_height,
                        .format = VK_FORMAT_R16G16_SFLOAT,
                        .flags = TextureFlags::Compute_mask,
                        .compute = true
                    }
                },
                .name = "svgf_current_linear_depth",
            } ),
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Attachment,
                .resource_info = {
                    .texture = {
                        .width = half_width,
                        .height = half_height,
                        .format = VK_FORMAT_R16G16_SFLOAT,
                        .flags = TextureFlags::Compute_mask,
                        .compute = true
                    }
                },
                .name = "svgf_current_depth_normal_fwidth",
            } ),
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Attachment,
                .resource_info = {
                    .texture = {
                        .width = half_width,
                        .height = half_height,
                        .format = VK_FORMAT_R16G16_SFLOAT,
                        .flags = TextureFlags::Compute_mask,
                        .compute = true
                    }
                },
                .name = "svgf_current_motion_vectors",
            } ),
        },
        .scheduling = { CommandQueueType::Graphics, 0 },
        .enabled = true,
        .compute = true,
        .name = "svgf_guide_downsample_pass" } );
}

void SVGFGuideDownsamplePass::update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) {
    Renderer* renderer = context.renderer;

    if ( phase == PipelineUpdatePhase::Destroy ) {
        renderer->destroy_compute_pipeline_state( pipeline );

        return;
    }

    ComputePipelineTransaction compute_transaction( renderer );

    ComputePipelineState& svgf_downsample = compute_transaction.add( pipeline );

    renderer->create_compute_pipeline_state(
        {
        .stages = {
            {
                .source_file_path = "glsl/reflections.glsl",
                .type = VK_SHADER_STAGE_COMPUTE_BIT,
            },
        },
        .name = "svgf_downsample" },
        {
            .name = "svgf_downsample",
            .render_pass_name = "svgf_guide_downsample_pass",
        },
        "svgf",
        context.frame_graph, svgf_downsample );

    compute_transaction.commit_or_rollback();
}

void SVGFGuideDownsamplePass::render( FrameGraphRenderContext& context ) {
    if ( !enabled ) {
        return;
    }

    CommandBuffer* gpu_commands = context.gpu_commands;
    RenderBlackboard& render_blackboard = *context.render_blackboard;

    gpu_commands->bind_pipeline( pipeline.pipeline );

    SVGFPushConstants push_constants;
    push_constants.step_size = 0;
    push_constants.sigma_l = 0.0f;
    push_constants.sigma_n = 0.0f;
    push_constants.sigma_z = 0.0f;

    gpu_commands->push_constants( pipeline.pipeline, 0, sizeof( SVGFPushConstants ), &push_constants );

    gpu_commands->bind_descriptor_set( { renderer->gpu->bindless_descriptor_set, descriptor_set },
        { render_blackboard.scene_cb_offset, constants_offset } );

    gpu_commands->dispatch( raptor::ceilu32( render_blackboard.render_width * texture_scale / 8.0f ),
                            raptor::ceilu32( render_blackboard.render_height * texture_scale / 8.0f ), 1 );
}

void SVGFGuideDownsamplePass::on_resize( FrameGraphResourceContext& context, u32 new_width, u32 new_height ) {
    if ( !enabled ) {
        return;
    }
}

void SVGFGuideDownsamplePass::create_gpu_resources( FrameGraphResourceContext& context ) {
    FrameGraph* frame_graph = context.frame_graph;
    RenderScene& scene = *context.render_scene;

    FrameGraphNode* node = frame_graph->get_node( "svgf_guide_downsample_pass" );
    if ( node == nullptr ) {
        enabled = false;

        return;
    }

    enabled = node->enabled;
    if ( !enabled ) {
        return;
    }

    GpuDevice& gpu = *renderer->gpu;

    cache_svgf_common_resources( frame_graph, &resources );
    cache_svgf_current_guide_resources( frame_graph, &output );

    gpu.link_image_sampler( resources.mesh_id_texture, gpu.global_samplers[ GlobalSamplers::NearestClamp ] );
    //gpu.link_image_sampler( accumulation_input.last_frame_mesh_id_texture, gpu.global_samplers[ GlobalSamplers::NearestClamp ] );

    create_descriptors( context );
}

void SVGFGuideDownsamplePass::upload_gpu_data( FrameGraphResourceContext& context ) {
    if ( !enabled ) {
        return;
    }

    GpuDevice& gpu = *renderer->gpu;
    RenderScene& scene = *context.render_scene;

    SVGFGpuConstants* gpu_constants = gpu.dynamic_buffer_allocate<SVGFGpuConstants>( &constants_offset );
    if ( gpu_constants ) {

        gpu_constants->motion_vectors_texture_index = resources.motion_vectors_image_view.index();
        gpu_constants->mesh_id_texture_index = resources.mesh_id_image_view.index();
        gpu_constants->normals_texture_index = resources.normals_image_view.index();
        gpu_constants->linear_z_dd_texture_index = resources.linear_z_dd_image_view.index();
        gpu_constants->depth_normal_fwidth_texture_index = resources.depth_normal_fwidth_image_view.index();

        gpu_constants->current_motion_vectors_texture_index = output.motion_vectors_image_view.index();
        gpu_constants->current_mesh_id_texture_index = output.mesh_id_image_view.index();
        gpu_constants->current_normals_texture_index = output.normals_image_view.index();
        gpu_constants->current_linear_z_dd_texture_index = output.linear_depth_image_view.index();
        gpu_constants->current_depth_normal_fwidth_texture_index = output.depth_normal_fwidth_image_view.index();

        gpu_constants->history_mesh_id_texture_index = 0;
        gpu_constants->history_normals_texture_index = 0;
        gpu_constants->history_linear_depth_texture = 0;

        gpu_constants->output_texture_index = 0;
        gpu_constants->history_output_texture_index = 0;
        gpu_constants->history_moments_texture_index = 0;
        gpu_constants->integrated_color_texture_index = 0;
        gpu_constants->integrated_moments_texture_index = 0;

        gpu_constants->variance_texture_index = 0;
        gpu_constants->filtered_color_texture_index = 0;
        gpu_constants->updated_variance_texture_index = 0;

        // NOTE(marco): unused
        gpu_constants->filtered_color_texture_index = 0;
        gpu_constants->updated_variance_texture_index = 0;

        gpu_constants->output_resolution_scale = texture_scale;
        gpu_constants->output_resolution_scale_rcp = 1.0f / texture_scale;
        gpu_constants->temporal_depth_difference = context.render_config->raytraced_reflections.temporal_depth_difference;
        gpu_constants->temporal_normal_difference = context.render_config->raytraced_reflections.temporal_normal_difference;
    }
}

void SVGFGuideDownsamplePass::destroy_gpu_resources( FrameGraphResourceContext& context ) {
    if ( !enabled ) {
        return;
    }

    GpuDevice& gpu = *renderer->gpu;

    gpu.destroy_descriptor_set( descriptor_set );
}

void SVGFGuideDownsamplePass::create_descriptors( FrameGraphResourceContext& context ) {

    GpuDevice* gpu = renderer->gpu;
    RenderBlackboard& render_blackboard = *context.render_blackboard;

    ShaderReflectionInfo* reflection_info = renderer->get_shader_reflection( pipeline.pipeline );

    DescriptorSetBinder descriptors;
    descriptors.dynamic_buffers.push( { 40, sizeof( SVGFGpuConstants ) } );
    descriptors.name = "svgf_guide_downsample_ds";

    descriptor_set = renderer->create_descriptor_set( descriptors, reflection_info, pipeline.pipeline, 0, render_blackboard );
}

// SVGFAccumulationPass ///////////////////////////////////////////////////////////
void SVGFAccumulationPass::declare_frame_graph_node( FrameGraphResourceContext& context ) {
    FrameGraphBuilder& builder = *context.frame_graph->builder;
    renderer = context.renderer;

    // Cache the texture scale from the render config for use in creating the output textures.
    texture_scale = context.render_config->raytraced_reflections.reflections_scale;

    context.frame_graph->add_node_v2( {
        .inputs = {
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "reflections_pass", "reflections" )
            },
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "restirgi_pass", "restirgi_output" )
            },
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "svgf_guide_downsample_pass", "svgf_current_normals" )
            },
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "svgf_guide_downsample_pass", "svgf_current_mesh_id" )
            },
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "svgf_guide_downsample_pass", "svgf_current_linear_depth" )
            },
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "svgf_guide_downsample_pass", "svgf_current_depth_normal_fwidth" )
            },
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "svgf_guide_downsample_pass", "svgf_current_motion_vectors" )
            },
        },
        .outputs = {
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Attachment,
                .resource_info = {
                    .texture = {
                        .scale_width = texture_scale,
                        .scale_height = texture_scale,
                        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                        .flags = TextureFlags::Compute_mask,
                        .compute = true
                    }
                },
                .name = "integrated_reflection_color",
            } ),
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Attachment,
                .resource_info = {
                    .texture = {
                        .scale_width = texture_scale,
                        .scale_height = texture_scale,
                        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                        .flags = TextureFlags::Compute_mask,
                        .compute = true
                    }
                },
                .name = "integrated_restirgi_color",
            } ),
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Attachment,
                .resource_info = {
                    .texture = {
                        .scale_width = texture_scale,
                        .scale_height = texture_scale,
                        .format = VK_FORMAT_R16G16_SFLOAT,
                        .flags = TextureFlags::Compute_mask,
                        .compute = true
                    }
                },
                .name = "integrated_reflection_moments",
            } ),
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Attachment,
                .resource_info = {
                    .texture = {
                        .scale_width = texture_scale,
                        .scale_height = texture_scale,
                        .format = VK_FORMAT_R16G16_SFLOAT,
                        .flags = TextureFlags::Compute_mask,
                        .compute = true
                    }
                },
                .name = "integrated_restirgi_moments",
            } ),
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Texture,
                .resource_info = {
                    .external = true
                },
                .name = "reflections_history",
            } ),
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Texture,
                .resource_info = {
                    .external = true
                },
                .name = "restirgi_history",
            } ),
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Texture,
                .resource_info = {
                    .external = true
                },
                .name = "reflections_moments_history",
            } ),
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Texture,
                .resource_info = {
                    .external = true
                },
                .name = "restirgi_moments_history",
            } ),
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Texture,
                .resource_info = {
                    .external = true
                },
                .name = "normals_history",
            } ),
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Texture,
                .resource_info = {
                    .external = true
                },
                .name = "linear_depth_history",
            } ),
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Texture,
                .resource_info = {
                    .external = true
                },
                .name = "mesh_id_history",
            } )
        },
        .scheduling = { CommandQueueType::Graphics, 0 },
        .enabled = true,
        .compute = true,
        .name = "svgf_accumulation_pass" } );
}

void SVGFAccumulationPass::update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) {
    Renderer* renderer = context.renderer;

    if ( phase == PipelineUpdatePhase::Destroy ) {
        renderer->destroy_compute_pipeline_state( pipeline );

        return;
    }

    ComputePipelineTransaction compute_transaction( renderer );

    ComputePipelineState& temporal_accumulation = compute_transaction.add( pipeline );

    renderer->create_compute_pipeline_state(
        {
        .stages = {
            {
                .source_file_path = "glsl/reflections.glsl",
                .type = VK_SHADER_STAGE_COMPUTE_BIT,
            },
        },
        .name = "svgf_accumulation" },
        {
            .name = "svgf_temporal_accumulation",
            .render_pass_name = "svgf_accumulation_pass",
        },
        "svgf",
        context.frame_graph, temporal_accumulation );

    compute_transaction.commit_or_rollback();
}

void SVGFAccumulationPass::pre_render( FrameGraphRenderContext& context ) {
    if ( !enabled ) {
        return;
    }
}

void SVGFAccumulationPass::render( FrameGraphRenderContext& context ) {
    if ( !enabled ) {
        return;
    }

    CommandBuffer* gpu_commands = context.gpu_commands;
    RenderBlackboard& render_blackboard = *context.render_blackboard;

    if ( !render_blackboard.tlas.is_valid() ) {
        return;
    }

    gpu_commands->bind_pipeline( pipeline.pipeline );

    SVGFPushConstants push_constants;
    push_constants.step_size = reset_history;
    if ( reset_history ) {
        reset_history = false;
    }

    gpu_commands->push_constants( pipeline.pipeline, 0, sizeof( SVGFPushConstants ), &push_constants );

    gpu_commands->bind_descriptor_set(
        { renderer->gpu->bindless_descriptor_set, descriptor_set },
        { render_blackboard.scene_cb_offset, reflections_constants_offset } );

    gpu_commands->add_image_barrier( resources.integrated_reflection_color_texture, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS ),
                        { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_WRITE_BIT,
                        VK_IMAGE_LAYOUT_GENERAL } );
    gpu_commands->flush_barriers();

    gpu_commands->dispatch( raptor::ceilu32( render_blackboard.render_width * texture_scale / 8.0f ), raptor::ceilu32( render_blackboard.render_height * texture_scale / 8.0f ), 1 );

    gpu_commands->bind_descriptor_set(
        { renderer->gpu->bindless_descriptor_set, descriptor_set },
        { render_blackboard.scene_cb_offset, restirgi_constants_offset } );

    gpu_commands->add_image_barrier( resources.integrated_restirgi_color_texture, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS ),
                          { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_WRITE_BIT,
                            VK_IMAGE_LAYOUT_GENERAL } );
    gpu_commands->flush_barriers();

    gpu_commands->dispatch( raptor::ceilu32( render_blackboard.render_width * texture_scale / 8.0f ), raptor::ceilu32( render_blackboard.render_height * texture_scale / 8.0f ), 1 );
}

void SVGFAccumulationPass::on_resize(FrameGraphResourceContext& context, u32 new_width, u32 new_height ) {
    if ( !enabled ) {
        return;
    }

    GpuDevice& gpu = *renderer->gpu;

    const u32 adjusted_width = ceilu32( new_width * texture_scale );
    const u32 adjusted_height = ceilu32( new_height * texture_scale );

    gpu.resize_image( output.last_frame_normals_texture, adjusted_width, adjusted_height );
    gpu.resize_image( output.last_frame_mesh_id_texture, adjusted_width, adjusted_height );
    gpu.resize_image( output.last_frame_linear_depth_texture, adjusted_width, adjusted_height );
    gpu.resize_image( output.reflections_history_texture, adjusted_width, adjusted_height );
    gpu.resize_image( output.reflections_moments_history_texture, adjusted_width, adjusted_height );
    gpu.resize_image( output.restirgi_history_texture, adjusted_width, adjusted_height );
    gpu.resize_image( output.restirgi_moments_history_texture, adjusted_width, adjusted_height );

    gpu.recreate_image_view( output.last_frame_normals_image_view );
    gpu.recreate_image_view( output.last_frame_mesh_id_image_view );
    gpu.recreate_image_view( output.last_frame_linear_depth_image_view );
    gpu.recreate_image_view( output.reflections_history_image_view );
    gpu.recreate_image_view( output.reflections_moments_history_image_view );
    gpu.recreate_image_view( output.restirgi_history_image_view );
    gpu.recreate_image_view( output.restirgi_moments_history_image_view );

    gpu.add_image_view_to_bindless( output.last_frame_normals_image_view );
    gpu.add_image_view_to_bindless( output.last_frame_mesh_id_image_view );
    gpu.add_image_view_to_bindless( output.last_frame_linear_depth_image_view );
    gpu.add_image_view_to_bindless( output.reflections_history_image_view );
    gpu.add_image_view_to_bindless( output.reflections_moments_history_image_view );
    gpu.add_image_view_to_bindless( output.restirgi_history_image_view );
    gpu.add_image_view_to_bindless( output.restirgi_moments_history_image_view );

    reset_history = true;
}

static void create_2d_texture_and_add_to_framegraph( GpuDevice& gpu, FrameGraph* frame_graph, u32 width, u32 height,
    VkFormat format, cstring texture_name, cstring graph_name, ImageHandle& out_texture, ImageViewHandle& out_image_view ) {

    ImageCreation texture_creation{ };
    texture_creation.set_size( width, height, 1 )
                    .set_format_type( format, TextureType::Texture2D )
                    .set_mips( 1 )
                    .set_layers( 1 )
                    .set_flags( TextureFlags::Compute_mask )
                    .set_name( texture_name );
    out_texture = gpu.create_image( texture_creation );

    out_image_view = gpu.create_image_view( {
        .parent_image = out_texture,
        .view_type = VK_IMAGE_VIEW_TYPE_2D,
        .sub_resource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }, .name = texture_creation.name } );

    FrameGraphResource* resource = frame_graph->get_resource( graph_name );
    resource->resource_info.set_external_texture_2d( width, height, format, 0, out_texture, out_image_view );

    gpu.add_image_view_to_bindless( out_image_view );
}

void SVGFAccumulationPass::create_gpu_resources( FrameGraphResourceContext& context ) {
    FrameGraph* frame_graph = context.frame_graph;
    RenderScene& scene = *context.render_scene;

    FrameGraphNode* node = frame_graph->get_node( "svgf_accumulation_pass" );
    if ( node == nullptr ) {
        enabled = false;

        return;
    }

    enabled = node->enabled;
    if ( !enabled ) {
        return;
    }

    RenderBlackboard& render_blackboard = *context.render_blackboard;
    GpuDevice& gpu = *renderer->gpu;

    cache_svgf_common_resources( frame_graph, &resources );
    cache_svgf_current_guide_resources( frame_graph, &guide );

    const u32 adjusted_width = ceilu32( render_blackboard.render_width * texture_scale );
    const u32 adjusted_height = ceilu32( render_blackboard.render_height * texture_scale );

    create_2d_texture_and_add_to_framegraph( gpu, frame_graph, adjusted_width, adjusted_height, VK_FORMAT_R16G16B16A16_SFLOAT, "reflections_history", "reflections_history", output.reflections_history_texture, output.reflections_history_image_view );
    create_2d_texture_and_add_to_framegraph( gpu, frame_graph, adjusted_width, adjusted_height, VK_FORMAT_R16G16_SFLOAT, "reflections_moments_history", "reflections_moments_history", output.reflections_moments_history_texture, output.reflections_moments_history_image_view );
    create_2d_texture_and_add_to_framegraph( gpu, frame_graph, adjusted_width, adjusted_height, VK_FORMAT_R16G16B16A16_SFLOAT, "restirgi_history", "restirgi_history", output.restirgi_history_texture, output.restirgi_history_image_view );
    create_2d_texture_and_add_to_framegraph( gpu, frame_graph, adjusted_width, adjusted_height, VK_FORMAT_R16G16_SFLOAT, "restirgi_moments_history", "restirgi_moments_history", output.restirgi_moments_history_texture, output.restirgi_moments_history_image_view );
    create_2d_texture_and_add_to_framegraph( gpu, frame_graph, adjusted_width, adjusted_height, VK_FORMAT_R16G16B16A16_SFLOAT, "normals_history", "normals_history", output.last_frame_normals_texture, output.last_frame_normals_image_view );
    create_2d_texture_and_add_to_framegraph( gpu, frame_graph, adjusted_width, adjusted_height, VK_FORMAT_R16G16_SFLOAT, "linear_depth_history", "linear_depth_history", output.last_frame_linear_depth_texture, output.last_frame_linear_depth_image_view );
    create_2d_texture_and_add_to_framegraph( gpu, frame_graph, adjusted_width, adjusted_height, VK_FORMAT_R32_UINT, "mesh_id_history", "mesh_id_history", output.last_frame_mesh_id_texture, output.last_frame_mesh_id_image_view );

    create_descriptors( context );
}

static void svgf_cache_reflections_image_view_indices( SVGFAccumulationOutput& accumulation, SVGFCommonResources& resources, SVGFGuideResources& current_guide, SVGFGpuConstants* gpu_constants ) {
    gpu_constants->motion_vectors_texture_index = resources.motion_vectors_image_view.index();
    gpu_constants->mesh_id_texture_index = resources.mesh_id_image_view.index();
    gpu_constants->normals_texture_index = resources.normals_image_view.index();
    gpu_constants->linear_z_dd_texture_index = resources.linear_z_dd_image_view.index();
    gpu_constants->current_motion_vectors_texture_index = current_guide.motion_vectors_image_view.index();
    gpu_constants->current_mesh_id_texture_index = current_guide.mesh_id_image_view.index();
    gpu_constants->current_normals_texture_index = current_guide.normals_image_view.index();
    gpu_constants->current_linear_z_dd_texture_index = current_guide.linear_depth_image_view.index();
    gpu_constants->current_depth_normal_fwidth_texture_index = current_guide.depth_normal_fwidth_image_view.index();
    gpu_constants->history_mesh_id_texture_index = accumulation.last_frame_mesh_id_image_view.index();
    gpu_constants->history_normals_texture_index = accumulation.last_frame_normals_image_view.index();
    gpu_constants->history_linear_depth_texture = accumulation.last_frame_linear_depth_image_view.index();
    gpu_constants->output_texture_index = resources.reflections_image_view.index();
    gpu_constants->history_output_texture_index = accumulation.reflections_history_image_view.index();
    gpu_constants->history_moments_texture_index = accumulation.reflections_moments_history_image_view.index();
    gpu_constants->integrated_color_texture_index = resources.integrated_reflection_color_image_view.index();
    gpu_constants->integrated_moments_texture_index = resources.integrated_reflection_moments_image_view.index();
    gpu_constants->depth_normal_fwidth_texture_index = resources.depth_normal_fwidth_image_view.index();
}

static void svgf_cache_restirgi_image_view_indices( SVGFAccumulationOutput& accumulation, SVGFCommonResources& resources, SVGFGuideResources& current_guide, SVGFGpuConstants* gpu_constants ) {
    gpu_constants->motion_vectors_texture_index = resources.motion_vectors_image_view.index();
    gpu_constants->mesh_id_texture_index = resources.mesh_id_image_view.index();
    gpu_constants->normals_texture_index = resources.normals_image_view.index();
    gpu_constants->linear_z_dd_texture_index = resources.linear_z_dd_image_view.index();
    gpu_constants->current_motion_vectors_texture_index = current_guide.motion_vectors_image_view.index();
    gpu_constants->current_mesh_id_texture_index = current_guide.mesh_id_image_view.index();
    gpu_constants->current_normals_texture_index = current_guide.normals_image_view.index();
    gpu_constants->current_linear_z_dd_texture_index = current_guide.linear_depth_image_view.index();
    gpu_constants->current_depth_normal_fwidth_texture_index = current_guide.depth_normal_fwidth_image_view.index();
    gpu_constants->history_mesh_id_texture_index = accumulation.last_frame_mesh_id_image_view.index();
    gpu_constants->history_normals_texture_index = accumulation.last_frame_normals_image_view.index();
    gpu_constants->history_linear_depth_texture = accumulation.last_frame_linear_depth_image_view.index();
    gpu_constants->output_texture_index = resources.restirgi_output_image_view.index();
    gpu_constants->history_output_texture_index = accumulation.restirgi_history_image_view.index();
    gpu_constants->history_moments_texture_index = accumulation.restirgi_moments_history_image_view.index();
    gpu_constants->integrated_color_texture_index = resources.integrated_restirgi_color_image_view.index();
    gpu_constants->integrated_moments_texture_index = resources.integrated_restirgi_moments_image_view.index();
    gpu_constants->depth_normal_fwidth_texture_index = resources.depth_normal_fwidth_image_view.index();
}

void SVGFAccumulationPass::upload_gpu_data( FrameGraphResourceContext& context ) {
    if ( !enabled ) {
        return;
    }

    GpuDevice& gpu = *renderer->gpu;
    RenderScene& scene = *context.render_scene;

    SVGFGpuConstants* gpu_constants = gpu.dynamic_buffer_allocate<SVGFGpuConstants>( &reflections_constants_offset );
    if ( gpu_constants ) {
        svgf_cache_reflections_image_view_indices( output, resources, guide, gpu_constants );
        // NOTE(marco): unused
        gpu_constants->variance_texture_index = 0;
        gpu_constants->filtered_color_texture_index = 0;
        gpu_constants->updated_variance_texture_index = 0;

        gpu_constants->output_resolution_scale = texture_scale;
        gpu_constants->output_resolution_scale_rcp = 1.0f / texture_scale;
        gpu_constants->input_resolution_scale = texture_scale;
        gpu_constants->input_resolution_scale_rcp = 1.0f / texture_scale;
        gpu_constants->temporal_depth_difference = context.render_config->raytraced_reflections.temporal_depth_difference;
        gpu_constants->temporal_normal_difference = context.render_config->raytraced_reflections.temporal_normal_difference;
    }

    gpu_constants = gpu.dynamic_buffer_allocate<SVGFGpuConstants>( &restirgi_constants_offset );
    if ( gpu_constants ) {
        svgf_cache_restirgi_image_view_indices( output, resources, guide, gpu_constants );
        // NOTE(marco): unused
        gpu_constants->variance_texture_index = 0;
        gpu_constants->filtered_color_texture_index = 0;
        gpu_constants->updated_variance_texture_index = 0;

        // NOTE(marco): restirgi is full screen
        gpu_constants->output_resolution_scale = texture_scale;
        gpu_constants->output_resolution_scale_rcp = 1.0f / texture_scale;
        gpu_constants->input_resolution_scale = 1.0f;
        gpu_constants->input_resolution_scale_rcp = 1.0f;
        gpu_constants->temporal_depth_difference = context.render_config->raytraced_reflections.temporal_depth_difference;
        gpu_constants->temporal_normal_difference = context.render_config->raytraced_reflections.temporal_normal_difference;
    }
}

void SVGFAccumulationPass::destroy_gpu_resources( FrameGraphResourceContext& context ) {
    if ( !enabled ) {
        return;
    }

    GpuDevice& gpu = *renderer->gpu;

    gpu.destroy_image( output.last_frame_normals_texture );
    gpu.destroy_image_view( output.last_frame_normals_image_view );
    gpu.destroy_image( output.last_frame_linear_depth_texture );
    gpu.destroy_image_view( output.last_frame_linear_depth_image_view );
    gpu.destroy_image( output.last_frame_mesh_id_texture );
    gpu.destroy_image_view( output.last_frame_mesh_id_image_view );

    gpu.destroy_image( output.reflections_history_texture );
    gpu.destroy_image_view( output.reflections_history_image_view );
    gpu.destroy_image( output.reflections_moments_history_texture );
    gpu.destroy_image_view( output.reflections_moments_history_image_view );

    gpu.destroy_image( output.restirgi_history_texture );
    gpu.destroy_image_view( output.restirgi_history_image_view );
    gpu.destroy_image( output.restirgi_moments_history_texture );
    gpu.destroy_image_view( output.restirgi_moments_history_image_view );

    gpu.destroy_descriptor_set( descriptor_set );
}

void SVGFAccumulationPass::update_dependent_resources( FrameGraphResourceContext& context ) {

}

void SVGFAccumulationPass::create_descriptors( FrameGraphResourceContext& context ) {

    GpuDevice* gpu = renderer->gpu;
    RenderBlackboard& render_blackboard = *context.render_blackboard;

    ShaderReflectionInfo* reflection_info = renderer->get_shader_reflection( pipeline.pipeline );

    DescriptorSetBinder descriptors;
    descriptors.dynamic_buffers.push( { 40, sizeof( SVGFGpuConstants ) } );
    descriptors.name = "svgf_accumulation_pass_ds";

    descriptor_set = renderer->create_descriptor_set( descriptors, reflection_info, pipeline.pipeline, 0, render_blackboard );
}

// SVGFVariancePass ///////////////////////////////////////////////////////////
void SVGFVariancePass::declare_frame_graph_node( FrameGraphResourceContext& context ) {
    FrameGraphBuilder& builder = *context.frame_graph->builder;
    renderer = context.renderer;

    RenderBlackboard& render_blackboard = *context.render_blackboard;

    // Cache the texture scale from the render config for use in creating the output textures.
    texture_scale = context.render_config->raytraced_reflections.reflections_scale;

    context.frame_graph->add_node_v2( {
        .inputs = {
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "svgf_accumulation_pass", "integrated_reflection_color" )
            },
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "svgf_accumulation_pass", "integrated_reflection_moments" )
            },
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "svgf_accumulation_pass", "integrated_restirgi_color" )
            },
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "svgf_accumulation_pass", "integrated_restirgi_moments" )
            },
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "svgf_guide_downsample_pass", "svgf_current_normals" )
            },
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "svgf_guide_downsample_pass", "svgf_current_linear_depth" )
            },
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "svgf_guide_downsample_pass", "svgf_current_mesh_id" )
            },
        },
        .outputs = {
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Attachment,
                .resource_info = {
                    .texture = {
                        .width = ceilu32( render_blackboard.render_width * texture_scale ),
                        .height = ceilu32( render_blackboard.render_height * texture_scale ),
                        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                        .compute = true
                    }
                },
                .name = "reflections_variance",
            } ),
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Attachment,
                .resource_info = {
                    .texture = {
                        .width = ceilu32( render_blackboard.render_width * texture_scale ),
                        .height = ceilu32( render_blackboard.render_height * texture_scale ),
                        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                        .compute = true,
                    }
                },
                .name = "restirgi_variance",
            } ),
        },
        .scheduling = { CommandQueueType::Graphics, 0 },
        .enabled = true,
        .compute = true,
        .name = "svgf_variance_pass" } );
}

void SVGFVariancePass::update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) {
    Renderer* renderer = context.renderer;

    if ( phase == PipelineUpdatePhase::Destroy ) {
        renderer->destroy_compute_pipeline_state( pipeline );

        return;
    }

    ComputePipelineTransaction compute_transaction( renderer );

    ComputePipelineState& svgf_variance = compute_transaction.add( pipeline );

    renderer->create_compute_pipeline_state(
        {
        .stages = {
            {
                .source_file_path = "glsl/reflections.glsl",
                .type = VK_SHADER_STAGE_COMPUTE_BIT,
            },
        },
        .name = "svgf_variance" },
        {
            .name = "svgf_variance",
            .render_pass_name = "svgf_variance_pass",
        },
        "svgf",
        context.frame_graph, svgf_variance );

    compute_transaction.commit_or_rollback();
}

void SVGFVariancePass::pre_render( FrameGraphRenderContext& context ) {
    if ( !enabled ) {
        return;
    }
}

void SVGFVariancePass::render( FrameGraphRenderContext& context ) {
    if ( !enabled ) {
        return;
    }

    CommandBuffer* gpu_commands = context.gpu_commands;
    RenderBlackboard& render_blackboard = *context.render_blackboard;

    if ( !render_blackboard.tlas.is_valid() ) {
        return;
    }

    SVGFPushConstants push_constants;
    push_constants.sigma_l = context.render_config->raytraced_reflections.wavelet_sigma_l;
    push_constants.sigma_n = context.render_config->raytraced_reflections.wavelet_sigma_n;
    push_constants.sigma_z = context.render_config->raytraced_reflections.wavelet_sigma_z;


    gpu_commands->bind_pipeline( pipeline.pipeline );

    gpu_commands->push_constants( pipeline.pipeline, 0, sizeof( SVGFPushConstants ), &push_constants );

    gpu_commands->bind_descriptor_set(
        { renderer->gpu->bindless_descriptor_set, descriptor_set },
        { render_blackboard.scene_cb_offset, reflections_constants_offset } );
    gpu_commands->dispatch( raptor::ceilu32( render_blackboard.render_width * texture_scale / 8.0f ), raptor::ceilu32( render_blackboard.render_height * texture_scale / 8.0f ), 1 );

    gpu_commands->bind_descriptor_set(
        { renderer->gpu->bindless_descriptor_set, descriptor_set },
        {  render_blackboard.scene_cb_offset, restirgi_constants_offset } );

    gpu_commands->dispatch( raptor::ceilu32( render_blackboard.render_width * texture_scale / 8.0f ), raptor::ceilu32( render_blackboard.render_height * texture_scale / 8.0f ), 1 );
}

void SVGFVariancePass::on_resize(FrameGraphResourceContext& context, u32 new_width, u32 new_height ) {
    if ( !enabled ) {
        return;
    }
}

void SVGFVariancePass::create_gpu_resources( FrameGraphResourceContext& context ) {
    FrameGraph* frame_graph = context.frame_graph;
    RenderScene& scene = *context.render_scene;

    FrameGraphNode* node = frame_graph->get_node( "svgf_variance_pass" );
    if ( node == nullptr ) {
        enabled = false;

        return;
    }

    enabled = node->enabled;
    if ( !enabled ) {
        return;
    }

    GpuDevice& gpu = *renderer->gpu;

    cache_svgf_common_resources( frame_graph, &resources );
    cache_svgf_accumulation_resources( frame_graph, &accumulation_input );
    cache_svgf_current_guide_resources( frame_graph, &guide );

    gpu.link_image_sampler( resources.mesh_id_texture, gpu.global_samplers[ GlobalSamplers::NearestClamp ] );
    gpu.link_image_sampler( accumulation_input.last_frame_mesh_id_texture, gpu.global_samplers[ GlobalSamplers::NearestClamp ] );

    // NOTE(marco): cache textures from previous passes
    FrameGraphResource* resource = frame_graph->get_resource( "reflections_variance" );
    reflections_variance_texture = resource->resource_info.texture.image;
    reflections_variance_image_view = resource->resource_info.texture.image_view;

    resource = frame_graph->get_resource( "restirgi_variance" );
    restirgi_variance_texture = resource->resource_info.texture.image;
    restirgi_variance_image_view = resource->resource_info.texture.image_view;

    create_descriptors( context );
}

void SVGFVariancePass::upload_gpu_data( FrameGraphResourceContext& context ) {
    if ( !enabled ) {
        return;
    }

    GpuDevice& gpu = *renderer->gpu;
    RenderScene& scene = *context.render_scene;

    SVGFGpuConstants* gpu_constants = gpu.dynamic_buffer_allocate<SVGFGpuConstants>( &reflections_constants_offset );
    if ( gpu_constants ) {
        svgf_cache_reflections_image_view_indices( accumulation_input, resources, guide, gpu_constants );
        gpu_constants->variance_texture_index = reflections_variance_image_view.index();

        // NOTE(marco): unused
        gpu_constants->filtered_color_texture_index = 0;
        gpu_constants->updated_variance_texture_index = 0;

        gpu_constants->output_resolution_scale = texture_scale;
        gpu_constants->output_resolution_scale_rcp = 1.0f / texture_scale;
        gpu_constants->input_resolution_scale = texture_scale;
        gpu_constants->input_resolution_scale_rcp = 1.0f / texture_scale;
        gpu_constants->temporal_depth_difference = context.render_config->raytraced_reflections.temporal_depth_difference;
        gpu_constants->temporal_normal_difference = context.render_config->raytraced_reflections.temporal_normal_difference;
    }

    gpu_constants = gpu.dynamic_buffer_allocate<SVGFGpuConstants>( &restirgi_constants_offset );
    if ( gpu_constants ) {
        svgf_cache_restirgi_image_view_indices( accumulation_input, resources, guide, gpu_constants );
        gpu_constants->variance_texture_index = restirgi_variance_image_view.index();

        // NOTE(marco): unused
        gpu_constants->filtered_color_texture_index = 0;
        gpu_constants->updated_variance_texture_index = 0;

        gpu_constants->output_resolution_scale = texture_scale;
        gpu_constants->output_resolution_scale_rcp = 1.0f / texture_scale;
        gpu_constants->input_resolution_scale = texture_scale;
        gpu_constants->input_resolution_scale_rcp = 1.0f / texture_scale;
        gpu_constants->temporal_depth_difference = context.render_config->raytraced_reflections.temporal_depth_difference;
        gpu_constants->temporal_normal_difference = context.render_config->raytraced_reflections.temporal_normal_difference;
    }
}

void SVGFVariancePass::destroy_gpu_resources( FrameGraphResourceContext& context ) {
    if ( !enabled ) {
        return;
    }

    GpuDevice& gpu = *renderer->gpu;

    gpu.destroy_descriptor_set( descriptor_set );
}

void SVGFVariancePass::update_dependent_resources( FrameGraphResourceContext& context ) {
    if ( !enabled ) {
        return;
    }
}

void SVGFVariancePass::create_descriptors( FrameGraphResourceContext& context ) {

    GpuDevice* gpu = renderer->gpu;
    RenderBlackboard& render_blackboard = *context.render_blackboard;

    ShaderReflectionInfo* reflection_info = renderer->get_shader_reflection( pipeline.pipeline );

    DescriptorSetBinder descriptors;
    descriptors.dynamic_buffers.push( { 40, sizeof( SVGFGpuConstants ) } );
    descriptors.name = "svgf_variance_ds";

    descriptor_set = renderer->create_descriptor_set( descriptors, reflection_info, pipeline.pipeline, 0, render_blackboard );
}

// SVGFWaveletPass ///////////////////////////////////////////////////////////
void SVGFWaveletPass::declare_frame_graph_node( FrameGraphResourceContext& context ) {
    FrameGraphBuilder& builder = *context.frame_graph->builder;
    renderer = context.renderer;

    // Cache the texture scale from the render config for use in creating the output textures.
    texture_scale = context.render_config->raytraced_reflections.reflections_scale;

    context.frame_graph->add_node_v2( {
        .inputs = {
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "svgf_accumulation_pass", "integrated_reflection_color" )
            },
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "svgf_accumulation_pass", "integrated_restirgi_color" )
            },
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "svgf_variance_pass", "reflections_variance" )
            },
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "svgf_variance_pass", "restirgi_variance" )
            },
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "svgf_guide_downsample_pass", "svgf_current_normals" )
            },
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "svgf_guide_downsample_pass", "svgf_current_linear_depth" )
            },
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "svgf_guide_downsample_pass", "svgf_current_mesh_id" )
            },
        },
        .outputs = {
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Attachment,
                .resource_info = {
                    .external = true
                },
                .name = "reflections_denoised_output",
            } ),
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Attachment,
                .resource_info = {
                    .external = true
                },
                .name = "restirgi_denoised_output",
            } ),
        },
        .scheduling = { CommandQueueType::Graphics, 0 },
        .enabled = true,
        .compute = true,
        .name = "svgf_wavelet_pass" } );
}

void SVGFWaveletPass::update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) {
    Renderer* renderer = context.renderer;

    if ( phase == PipelineUpdatePhase::Destroy ) {
        renderer->destroy_compute_pipeline_state( pipeline );

        return;
    }

    ComputePipelineTransaction compute_transaction( renderer );

    ComputePipelineState& temporal_accumulation = compute_transaction.add( pipeline );

    renderer->create_compute_pipeline_state(
        {
        .stages = {
            {
                .source_file_path = "glsl/reflections.glsl",
                .type = VK_SHADER_STAGE_COMPUTE_BIT,
            },
        },
        .name = "svgf_wavelet" },
        {
            .name = "svgf_wavelet",
            .render_pass_name = "svgf_wavelet_pass",
        },
        "svgf",
        context.frame_graph, temporal_accumulation );

    compute_transaction.commit_or_rollback();
}

void SVGFWaveletPass::pre_render( FrameGraphRenderContext& context ) {
    if ( !enabled ) {
        return;
    }
}

void SVGFWaveletPass::render( FrameGraphRenderContext& context ) {
    if ( !enabled ) {
        return;
    }

    CommandBuffer* gpu_commands = context.gpu_commands;
    RenderScene* render_scene = context.render_view->scene;
    RenderBlackboard& render_blackboard = *context.render_blackboard;

    if ( !render_blackboard.tlas.is_valid() ) {
        return;
    }

    gpu_commands->bind_pipeline( pipeline.pipeline );

    SVGFPushConstants push_constants;
    push_constants.sigma_l = context.render_config->raytraced_reflections.wavelet_sigma_l;
    push_constants.sigma_n = context.render_config->raytraced_reflections.wavelet_sigma_n;
    push_constants.sigma_z = context.render_config->raytraced_reflections.wavelet_sigma_z;

    for ( u32 i = 0; i < k_num_passes; ++i ) {
        gpu_commands->bind_descriptor_set(
            { renderer->gpu->bindless_descriptor_set, descriptor_set[ i ] },
            { render_blackboard.scene_cb_offset, reflections_constant_offsets[ i ] } );

        if ( ( i % 2 ) == 0 ) {
            gpu_commands->add_image_barrier( resources.integrated_reflection_color_texture, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS ),
                          { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_READ_BIT,
                            VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL } );
            gpu_commands->add_image_barrier( reflections_variance_texture, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS ),
                          { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_READ_BIT,
                            VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL } );

            gpu_commands->add_image_barrier( reflections_ping_pong_color_image, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS ),
                          { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_WRITE_BIT,
                            VK_IMAGE_LAYOUT_GENERAL } );
            gpu_commands->add_image_barrier( ping_pong_variance_image, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS ),
                          { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_WRITE_BIT,
                            VK_IMAGE_LAYOUT_GENERAL } );
        } else {
            gpu_commands->add_image_barrier( resources.integrated_reflection_color_texture, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS ),
                          { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_WRITE_BIT,
                            VK_IMAGE_LAYOUT_GENERAL } );
            gpu_commands->add_image_barrier( reflections_variance_texture, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS ),
                          { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_WRITE_BIT,
                            VK_IMAGE_LAYOUT_GENERAL } );

            gpu_commands->add_image_barrier( reflections_ping_pong_color_image, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS ),
                          { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_READ_BIT,
                            VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL } );
            gpu_commands->add_image_barrier( ping_pong_variance_image, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS ),
                          { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_READ_BIT,
                            VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL } );
        }
        gpu_commands->flush_barriers();

        push_constants.step_size = 1 << i;

        gpu_commands->push_constants( pipeline.pipeline, 0, sizeof( SVGFPushConstants ), &push_constants );
        gpu_commands->dispatch( raptor::ceilu32( render_blackboard.render_width * texture_scale / 8.0f ), raptor::ceilu32( render_blackboard.render_height * texture_scale / 8.0f ), 1 );

        if ( i == 0 ) {
            gpu_commands->copy_image( reflections_ping_pong_color_image, accumulation_input.reflections_history_texture,
                { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL } );
        }
    }

    push_constants.step_size = 0;
    for ( u32 i = 0; i < k_num_passes; ++i ) {
        gpu_commands->bind_descriptor_set(
            { renderer->gpu->bindless_descriptor_set, descriptor_set[ i ] },
            { render_blackboard.scene_cb_offset, restirgi_constant_offsets[ i ] } );

        if ( ( i % 2 ) == 0 ) {
            gpu_commands->add_image_barrier( resources.integrated_restirgi_color_texture, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS ),
                          { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_READ_BIT,
                            VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL } );
            gpu_commands->add_image_barrier( restirgi_variance_texture, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS ),
                          { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_READ_BIT,
                            VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL } );

            gpu_commands->add_image_barrier( restirgi_ping_pong_color_image, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS ),
                          { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_WRITE_BIT,
                            VK_IMAGE_LAYOUT_GENERAL } );
            gpu_commands->add_image_barrier( ping_pong_variance_image, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS ),
                          { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_WRITE_BIT,
                            VK_IMAGE_LAYOUT_GENERAL } );
        } else {
            gpu_commands->add_image_barrier( resources.integrated_restirgi_color_texture, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS ),
                          { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_WRITE_BIT,
                            VK_IMAGE_LAYOUT_GENERAL } );
            gpu_commands->add_image_barrier( restirgi_variance_texture, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS ),
                          { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_WRITE_BIT,
                            VK_IMAGE_LAYOUT_GENERAL } );

            gpu_commands->add_image_barrier( restirgi_ping_pong_color_image, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS ),
                          { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_READ_BIT,
                            VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL } );
            gpu_commands->add_image_barrier( ping_pong_variance_image, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS ),
                          { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_READ_BIT,
                            VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL } );
        }
        gpu_commands->flush_barriers();

        push_constants.step_size = 1 << i;

        gpu_commands->push_constants( pipeline.pipeline, 0, sizeof( SVGFPushConstants ), &push_constants );
        gpu_commands->dispatch( raptor::ceilu32( render_blackboard.render_width * texture_scale / 8.0f ), raptor::ceilu32( render_blackboard.render_height * texture_scale / 8.0f ), 1 );

        if ( i == 0 ) {
            gpu_commands->copy_image( restirgi_ping_pong_color_image, accumulation_input.restirgi_history_texture,
                { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL } );
        }
    }

    // Current half-res guide -> previous-frame history guide
    gpu_commands->copy_image( guide.normals_texture,
        accumulation_input.last_frame_normals_texture,
        { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          VK_ACCESS_2_SHADER_READ_BIT,
          VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL } );

    gpu_commands->copy_image( guide.mesh_id_texture,
        accumulation_input.last_frame_mesh_id_texture,
        { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          VK_ACCESS_2_SHADER_READ_BIT,
          VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL } );

    gpu_commands->copy_image( guide.linear_depth_texture,
        accumulation_input.last_frame_linear_depth_texture,
        { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          VK_ACCESS_2_SHADER_READ_BIT,
          VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL } );

    // Moments -> moments history
    gpu_commands->copy_image( resources.integrated_reflection_moments_texture,
        accumulation_input.reflections_moments_history_texture,
        { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          VK_ACCESS_2_SHADER_READ_BIT,
          VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL } );

    gpu_commands->copy_image( resources.integrated_restirgi_moments_texture,
        accumulation_input.restirgi_moments_history_texture,
        { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          VK_ACCESS_2_SHADER_READ_BIT,
          VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL } );
}

void SVGFWaveletPass::on_resize(FrameGraphResourceContext& context, u32 new_width, u32 new_height ) {
    if ( !enabled ) {
        return;
    }

    GpuDevice& gpu = *renderer->gpu;

    const u32 adjusted_width = ceilu32( new_width * texture_scale );
    const u32 adjusted_height = ceilu32( new_height * texture_scale );

    gpu.resize_image( reflections_ping_pong_color_image, adjusted_width, adjusted_height );
    gpu.resize_image( restirgi_ping_pong_color_image, adjusted_width, adjusted_height );
    gpu.resize_image( ping_pong_variance_image, adjusted_width, adjusted_height );

    gpu.recreate_image_view( reflections_ping_pong_color_image_view );
    gpu.recreate_image_view( restirgi_ping_pong_color_image_view );
    gpu.recreate_image_view( ping_pong_variance_image_view );

    gpu.add_image_view_to_bindless( reflections_ping_pong_color_image_view );
    gpu.add_image_view_to_bindless( restirgi_ping_pong_color_image_view );
    gpu.add_image_view_to_bindless( ping_pong_variance_image_view );
}

void SVGFWaveletPass::create_gpu_resources( FrameGraphResourceContext& context ) {
    FrameGraph* frame_graph = context.frame_graph;
    RenderScene& scene = *context.render_scene;

    FrameGraphNode* node = frame_graph->get_node( "svgf_wavelet_pass" );
    if ( node == nullptr ) {
        enabled = false;

        return;
    }

    enabled = node->enabled;
    if ( !enabled ) {
        return;
    }

    RenderBlackboard& render_blackboard = *context.render_blackboard;
    GpuDevice& gpu = *renderer->gpu;

    const u32 adjusted_width = ceilu32( render_blackboard.render_width * texture_scale );
    const u32 adjusted_height = ceilu32( render_blackboard.render_height * texture_scale );

    create_2d_texture_and_add_to_framegraph( gpu, frame_graph, adjusted_width, adjusted_height, VK_FORMAT_R16G16B16A16_SFLOAT, "reflections_denoised_output", "reflections_denoised_output", reflections_ping_pong_color_image, reflections_ping_pong_color_image_view );
    create_2d_texture_and_add_to_framegraph( gpu, frame_graph, adjusted_width, adjusted_height, VK_FORMAT_R16G16B16A16_SFLOAT, "restirgi_denoised_output", "restirgi_denoised_output", restirgi_ping_pong_color_image, restirgi_ping_pong_color_image_view );

    ImageCreation texture_creation{ };
    texture_creation.set_size( adjusted_width, adjusted_height, 1 ).set_format_type( VK_FORMAT_R32_SFLOAT, TextureType::Texture2D ).set_flags( TextureFlags::Compute_mask ).set_name( "ping_pong_variance_texture" );
    ping_pong_variance_image = gpu.create_image( texture_creation );

    ping_pong_variance_image_view = gpu.create_image_view( {
        .parent_image = ping_pong_variance_image,
        .view_type = VK_IMAGE_VIEW_TYPE_2D,
        .sub_resource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }, .name = texture_creation.name } );

    gpu.add_image_view_to_bindless( ping_pong_variance_image_view );

    cache_svgf_common_resources( frame_graph, &resources );
    cache_svgf_accumulation_resources( frame_graph, &accumulation_input );
    cache_svgf_current_guide_resources( frame_graph, &guide );

    gpu.link_image_sampler( resources.mesh_id_texture, gpu.global_samplers[ GlobalSamplers::NearestClamp ] );
    gpu.link_image_sampler( accumulation_input.last_frame_mesh_id_texture, gpu.global_samplers[ GlobalSamplers::NearestClamp ] );

    // NOTE(marco): cache textures from previous passes
    FrameGraphResource* resource = frame_graph->get_resource( "reflections_variance" );
    reflections_variance_texture = resource->resource_info.texture.image;
    reflections_variance_image_view = resource->resource_info.texture.image_view;

    resource = frame_graph->get_resource( "restirgi_variance" );
    restirgi_variance_texture = resource->resource_info.texture.image;
    restirgi_variance_image_view = resource->resource_info.texture.image_view;

    create_descriptors( context );
}

void SVGFWaveletPass::upload_gpu_data( FrameGraphResourceContext& context ) {
    if ( !enabled ) {
        return;
    }

    GpuDevice& gpu = *renderer->gpu;
    RenderScene& scene = *context.render_scene;

    for ( u32 i = 0; i < k_num_passes; ++i ) {

        SVGFGpuConstants* gpu_constants = gpu.dynamic_buffer_allocate<SVGFGpuConstants>( &reflections_constant_offsets[ i ] );
        if ( gpu_constants ) {
            svgf_cache_reflections_image_view_indices( accumulation_input, resources, guide, gpu_constants );

            gpu_constants->integrated_color_texture_index = ( i % 2 == 0 ) ? resources.integrated_reflection_color_image_view.index() : reflections_ping_pong_color_image_view.index();
            gpu_constants->variance_texture_index = ( i % 2 == 0 ) ? reflections_variance_image_view.index() : ping_pong_variance_image_view.index();

            gpu_constants->filtered_color_texture_index = ( i % 2 == 1 ) ? resources.integrated_reflection_color_image_view.index() : reflections_ping_pong_color_image_view.index();
            gpu_constants->updated_variance_texture_index = ( i % 2 == 1 ) ? reflections_variance_image_view.index() : ping_pong_variance_image_view.index();

            gpu_constants->output_resolution_scale = texture_scale;
            gpu_constants->output_resolution_scale_rcp = 1.0f / texture_scale;
            gpu_constants->input_resolution_scale = texture_scale;
            gpu_constants->input_resolution_scale_rcp = 1.0f / texture_scale;
            gpu_constants->temporal_depth_difference = context.render_config->raytraced_reflections.temporal_depth_difference;
            gpu_constants->temporal_normal_difference = context.render_config->raytraced_reflections.temporal_normal_difference;
        }

        gpu_constants = gpu.dynamic_buffer_allocate<SVGFGpuConstants>( &restirgi_constant_offsets[ i ] );
        if ( gpu_constants ) {
            svgf_cache_restirgi_image_view_indices( accumulation_input, resources, guide, gpu_constants );

            gpu_constants->integrated_color_texture_index = ( i % 2 == 0 ) ? resources.integrated_restirgi_color_image_view.index() : restirgi_ping_pong_color_image_view.index();
            gpu_constants->variance_texture_index = ( i % 2 == 0 ) ? restirgi_variance_image_view.index() : ping_pong_variance_image_view.index();

            gpu_constants->filtered_color_texture_index = ( i % 2 == 1 ) ? resources.integrated_restirgi_color_image_view.index() : restirgi_ping_pong_color_image_view.index();
            gpu_constants->updated_variance_texture_index = ( i % 2 == 1 ) ? restirgi_variance_image_view.index() : ping_pong_variance_image_view.index();

            gpu_constants->output_resolution_scale = texture_scale;
            gpu_constants->output_resolution_scale_rcp = 1.0f / texture_scale;
            gpu_constants->input_resolution_scale = texture_scale;
            gpu_constants->input_resolution_scale_rcp = 1.0f / texture_scale;
            gpu_constants->temporal_depth_difference = context.render_config->raytraced_reflections.temporal_depth_difference;
            gpu_constants->temporal_normal_difference = context.render_config->raytraced_reflections.temporal_normal_difference;
        }
    }
}

void SVGFWaveletPass::destroy_gpu_resources( FrameGraphResourceContext& context ) {
    if ( !enabled ) {
        return;
    }

    GpuDevice& gpu = *renderer->gpu;

    gpu.destroy_image( reflections_ping_pong_color_image );
    gpu.destroy_image( restirgi_ping_pong_color_image );
    gpu.destroy_image( ping_pong_variance_image );
    gpu.destroy_image_view( reflections_ping_pong_color_image_view );
    gpu.destroy_image_view( restirgi_ping_pong_color_image_view );
    gpu.destroy_image_view( ping_pong_variance_image_view );

    for ( u32 i = 0; i < k_num_passes; ++i ) {
        gpu.destroy_descriptor_set( descriptor_set[ i ] );
    }
}

void SVGFWaveletPass::create_descriptors( FrameGraphResourceContext& context ) {

    GpuDevice* gpu = renderer->gpu;
    RenderBlackboard& render_blackboard = *context.render_blackboard;

    ShaderReflectionInfo* reflection_info = renderer->get_shader_reflection( pipeline.pipeline );

    DescriptorSetBinder descriptors;

    // TODO(marco): do we need more than one ds?
    for ( u32 i = 0; i < k_num_passes; ++i ) {
        gpu->destroy_descriptor_set( descriptor_set[ i ] );

        descriptors.reset();
        descriptors.dynamic_buffers.push( { 40, sizeof( SVGFGpuConstants ) } );
        descriptors.name = "svgf_wavelet_ds";

        descriptor_set[ i ] = renderer->create_descriptor_set( descriptors, reflection_info, pipeline.pipeline, 0, render_blackboard );
    }
}

} // namespace raptor
