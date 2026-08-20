
#include "graphics/render_passes/shadow_visibility_pass.hpp"
#include "graphics/render_scene.hpp"
#include "graphics/render_blackboard.hpp"

#include "foundation/numerics.hpp"

#include "../shaders/shared_structs.h"

namespace raptor {

// ShadowVisbilityPass ///////////////////////////////////////////////////
void ShadowVisibilityPass::declare_frame_graph_node( FrameGraphResourceContext& context ) {

    FrameGraphBuilder& builder = *context.frame_graph->builder;

    context.frame_graph->add_node_v2( {
        .inputs = {
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "gbuffer_pass_late", "depth" )
            },
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "motion_vector_pass", "visibility_motion_vectors" )
            }
        },
        .outputs = {
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Attachment,
                .resource_info{
                    .texture = {
                        .scale_width = 1.0f,
                        .scale_height = 1.0f,
                        .format = VK_FORMAT_R16_SFLOAT,
                        .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
                        .compute = true
                    }
                },
                .name = "shadow_visibility",
            } )
        },
        .scheduling = { CommandQueueType::Graphics, 0 },
        .enabled = true,
        .compute = true,
        .name = "shadow_visibility_pass" } );
}

ShaderCompilationCreation ssc_visibility_variance = {
    .stages = {
        {
            .source_file_path = "glsl/raytraced_shadows.glsl",
            .type = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    },
    .name = "shadow_visibility_variance",
    .slang_input = 0,
};

ShaderCompilationCreation ssc_visibility = {
    .stages = {
        {
            .source_file_path = "glsl/raytraced_shadows.glsl",
            .type = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    },
    .name = "shadow_visibility",
    .slang_input = 0,
};

ShaderCompilationCreation ssc_visibility_filtering = {
    .stages = {
        {
            .source_file_path = "glsl/raytraced_shadows.glsl",
            .type = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    },
    .name = "shadow_visibility_filtering",
    .slang_input = 0,
};

ShaderCompilationCreation ssc_cache_reprojection = {
    .stages = {
        {
            .source_file_path = "glsl/raytraced_shadows.glsl",
            .type = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    },
    .name = "shadow_cache_reprojection",
    .slang_input = 0,
};

ShaderCompilationCreation ssc_upscaling = {
    .stages = {
        {
            .source_file_path = "glsl/raytraced_shadows.glsl",
            .type = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    },
    .name = "shadow_visibility_upscaling",
    .slang_input = 0,
};

void ShadowVisibilityPass::update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) {

    Renderer* renderer = context.renderer;

    if ( phase == PipelineUpdatePhase::Destroy ) {
        renderer->destroy_compute_pipeline_state( variance_pipeline );
        renderer->destroy_compute_pipeline_state( visibility_pipeline );
        renderer->destroy_compute_pipeline_state( visibility_filtering_pipeline );
        renderer->destroy_compute_pipeline_state( cache_reprojection_pipeline );
        renderer->destroy_compute_pipeline_state( upscaling_pipeline );

        return;
    }

    ComputePipelineTransaction transaction( renderer );

    PipelineCreation pipeline_creation = {
        .name = "shadow_visibility_variance",
        .render_pass_name = "shadow_visibility_pass",
    };

    ComputePipelineState& new_variance_pipeline = transaction.add( variance_pipeline );
    ComputePipelineState& new_visibility_pipeline = transaction.add( visibility_pipeline );
    ComputePipelineState& new_visibility_filtering_pipeline = transaction.add( visibility_filtering_pipeline );
    ComputePipelineState& new_cache_reprojection_pipeline = transaction.add( cache_reprojection_pipeline );
    ComputePipelineState& new_upscaling_pipeline = transaction.add( upscaling_pipeline );

    renderer->create_compute_pipeline_state( ssc_visibility_variance, pipeline_creation,
                                             pipeline_creation.name, context.frame_graph, new_variance_pipeline );

    pipeline_creation.name = "shadow_visibility";
    renderer->create_compute_pipeline_state( ssc_visibility, pipeline_creation,
                                             pipeline_creation.name, context.frame_graph, new_visibility_pipeline );

    pipeline_creation.name = "shadow_visibility_filtering";
    renderer->create_compute_pipeline_state( ssc_visibility_filtering, pipeline_creation,
                                             pipeline_creation.name, context.frame_graph, new_visibility_filtering_pipeline );

    pipeline_creation.name = "shadow_cache_reprojection";
    renderer->create_compute_pipeline_state( ssc_cache_reprojection, pipeline_creation,
                                             pipeline_creation.name, context.frame_graph, new_cache_reprojection_pipeline );

    pipeline_creation.name = "shadow_visibility_upscaling";
    renderer->create_compute_pipeline_state( ssc_upscaling, pipeline_creation,
                                             pipeline_creation.name, context.frame_graph, new_upscaling_pipeline );

    transaction.commit_or_rollback();
}

void ShadowVisibilityPass::render( FrameGraphRenderContext& context ) {
    if ( !enabled ) {
        return;
    }

    RenderScene* render_scene = context.render_view->scene;
    u32 current_frame_index = context.current_frame_index;
    CommandBuffer* cb = context.gpu_commands;
    RenderBlackboard& render_blackboard = *context.render_blackboard;

    if ( render_scene->active_lights != last_active_lights_count ) {
        GpuDevice& gpu = *context.renderer->gpu;
        recreate_textures( gpu, render_scene->active_lights, render_blackboard.render_width, render_blackboard.render_height );
    }

    if ( clear_resources ) {
        VkClearColorValue clear_value{ };

        cb->clear_color_image( variation_image, clear_value );        
        cb->clear_color_image( filtered_visibility_image, clear_value );
        cb->clear_color_image( filtered_variation_image, clear_value );

        for ( u32 i = 0; i < 2; ++i ) {

            cb->clear_color_image( visibility_cache_image[ i ], clear_value );
            cb->clear_color_image( variation_cache_image[ i ], clear_value );
            cb->clear_color_image( samples_count_cache_image[ i ], clear_value );
        }

        clear_resources = false;
    }

    // Wait for valid TLAS before creating the descriptors
    if ( render_blackboard.tlas.is_invalid() ) {
        return;
    }
    
    if ( !descriptors_created ) {
        create_descriptors( context.renderer, context.render_blackboard );
        descriptors_created = true;
    }

    // Double buffering of cache textures
    const u32 current_cache_index = context.renderer->gpu->absolute_frame & 1;
    const u32 previous_cache_index = current_cache_index ^ 1;

    const ImageHandle previous_visibility_cache = visibility_cache_image[ previous_cache_index ];
    const ImageHandle current_visibility_cache = visibility_cache_image[ current_cache_index ];
    const ImageHandle previous_variation_cache = variation_cache_image[ previous_cache_index ];
    const ImageHandle current_variation_cache = variation_cache_image[ current_cache_index ];
    const ImageHandle previous_samples_count_cache = samples_count_cache_image[ previous_cache_index ];
    const ImageHandle current_samples_count_cache = samples_count_cache_image[ current_cache_index ];

    VkImageSubresourceRange range = range_aspect( VK_IMAGE_ASPECT_COLOR_BIT,
                                                  0, VK_REMAINING_MIP_LEVELS,
                                                  0, VK_REMAINING_ARRAY_LAYERS );

    u32 dispatch_x = ( ceilu32( render_blackboard.render_width * texture_scale ) + 7 ) / 8;
    u32 dispatch_y = ( ceilu32( render_blackboard.render_height * texture_scale ) + 7 ) / 8;

    // Reprojection pass
    cb->add_image_barrier( previous_visibility_cache, range,
        {
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL
        }
    );

    cb->add_image_barrier( previous_variation_cache, range,
        {
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL
        }
    );

    cb->add_image_barrier( previous_samples_count_cache, range,
        {
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL
        }
    );

    cb->add_image_barrier( current_visibility_cache, range,
        {
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_IMAGE_LAYOUT_GENERAL
        }
    );

    cb->add_image_barrier( current_variation_cache, range,
        {
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_IMAGE_LAYOUT_GENERAL
        }
    );

    cb->add_image_barrier( current_samples_count_cache, range,
        {
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_IMAGE_LAYOUT_GENERAL
        }
    );

    cb->flush_barriers();

    cb->bind_pipeline( cache_reprojection_pipeline.pipeline );

    cb->bind_descriptor_set(
        { context.renderer->gpu->bindless_descriptor_set, descriptor_set[ current_frame_index ] },
        { render_blackboard.scene_cb_offset, render_blackboard.lighting.lighting_constants_cb_offset, constants_offset } );

    cb->dispatch( dispatch_x, dispatch_y, 1 );


    cb->add_image_barrier( current_visibility_cache, range,
                           {
                               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                               VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL
                           } );

    cb->add_image_barrier( variation_image, range,
                           {
                               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                               VK_IMAGE_LAYOUT_GENERAL
                           } );

    cb->flush_barriers();

    // Variance pass
    cb->bind_pipeline( variance_pipeline.pipeline );

    cb->dispatch( dispatch_x, dispatch_y, 1 );

    cb->add_image_barrier( variation_image, range,
        {
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL
        } );

    cb->add_image_barrier( current_visibility_cache, range,
        {
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_IMAGE_LAYOUT_GENERAL
        } );

    cb->add_image_barrier( current_variation_cache, range,
        {
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_IMAGE_LAYOUT_GENERAL
        } );

    cb->add_image_barrier( current_samples_count_cache, range,
        {
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_IMAGE_LAYOUT_GENERAL
        } );

    cb->add_image_barrier( filtered_variation_image, range,
        {
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_IMAGE_LAYOUT_GENERAL
        } );

    cb->flush_barriers();

    // Visiblity pass
    cb->bind_pipeline( visibility_pipeline.pipeline );

    cb->dispatch( dispatch_x, dispatch_y, 1 );

    cb->add_image_barrier( current_visibility_cache, range,
        {
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL
        } );

    cb->add_image_barrier( filtered_visibility_image, range,
        {
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_IMAGE_LAYOUT_GENERAL
        } );

    cb->flush_barriers();

    // Visiblity filtering pass
    cb->bind_pipeline( visibility_filtering_pipeline.pipeline );

    cb->dispatch( dispatch_x, dispatch_y, 1 );

    cb->add_image_barrier( filtered_visibility_image, range,
                           {
                               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                               VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL
                           } );

    cb->add_image_barrier( shadow_visibility_resource->resource_info.texture.image, range,
                           {
                               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                               VK_IMAGE_LAYOUT_GENERAL
                           } );

    cb->flush_barriers();

    // Upscaling pass (to full resolution)
    dispatch_x = ( ceilu32( render_blackboard.render_width * 1.f ) + 7 ) / 8;
    dispatch_y = ( ceilu32( render_blackboard.render_height * 1.f ) + 7 ) / 8;
    cb->bind_pipeline( upscaling_pipeline.pipeline );

    cb->dispatch( dispatch_x, dispatch_y, 1 );
}

void ShadowVisibilityPass::on_resize( FrameGraphResourceContext& context, u32 new_width, u32 new_height ) {
    if ( !enabled ) {
        return;
    }

    GpuDevice& gpu = *context.renderer->gpu;

    const u32 adjusted_width = ceilu32( new_width * texture_scale );
    const u32 adjusted_height = ceilu32( new_height * texture_scale );

    for ( u32 i = 0; i < 2; ++i ) {
        gpu.resize_image_3d( visibility_cache_image[ i ], adjusted_width, adjusted_height, last_active_lights_count );
        gpu.recreate_image_view( visibility_cache_image_view[ i ] );
        gpu.resize_image_3d( variation_cache_image[ i ], adjusted_width, adjusted_height, last_active_lights_count );
        gpu.recreate_image_view( variation_cache_image_view[ i ] );
        gpu.resize_image_3d( samples_count_cache_image[ i ], adjusted_width, adjusted_height, last_active_lights_count );
        gpu.recreate_image_view( samples_count_cache_image_view[ i ] );

        gpu.link_image_sampler( samples_count_cache_image[ i ], gpu.global_samplers[ GlobalSamplers::NearestClamp ] );
    }

    gpu.resize_image_3d( variation_image, adjusted_width, adjusted_height, last_active_lights_count );
    gpu.recreate_image_view( variation_image_view );
    gpu.resize_image_3d( filtered_visibility_image, adjusted_width, adjusted_height, last_active_lights_count );
    gpu.recreate_image_view( filtered_visibility_image_view );
    gpu.resize_image_3d( filtered_variation_image, adjusted_width, adjusted_height, last_active_lights_count );
    gpu.recreate_image_view( filtered_variation_image_view );

    gpu.add_image_view_to_bindless( variation_image_view );
    gpu.add_image_view_to_bindless( filtered_visibility_image_view );
    gpu.add_image_view_to_bindless( filtered_variation_image_view );

    for ( u32 i = 0; i < 2; ++i ) {
        gpu.add_image_view_to_bindless( variation_cache_image_view[ i ] );
        gpu.add_image_view_to_bindless( visibility_cache_image_view[ i ] );
        gpu.add_image_view_to_bindless( samples_count_cache_image_view[ i ] );
    }

    clear_resources = true;
}

void ShadowVisibilityPass::recreate_textures( GpuDevice& gpu, u32 lights_count, u32 width, u32 height ) {
    if ( last_active_lights_count != 0 ) {
        gpu.destroy_image( variation_image );
        gpu.destroy_image( filtered_visibility_image );
        gpu.destroy_image( filtered_variation_image );

        gpu.destroy_image_view( variation_image_view );
        gpu.destroy_image_view( filtered_visibility_image_view );
        gpu.destroy_image_view( filtered_variation_image_view );

        for ( u32 i = 0; i < 2; ++i ) {

            gpu.destroy_image( visibility_cache_image[ i ] );
            gpu.destroy_image( variation_cache_image[ i ] );
            gpu.destroy_image( samples_count_cache_image[ i ] );

            gpu.destroy_image_view( variation_cache_image_view[ i ] );
            gpu.destroy_image_view( visibility_cache_image_view[ i ] );
            gpu.destroy_image_view( samples_count_cache_image_view[ i ] );
        }
    }

    const u32 adjusted_width = ceilu32( width * texture_scale );
    const u32 adjusted_height = ceilu32( height * texture_scale );

    ImageCreation texture_creation{ };
    texture_creation.set_flags( TextureFlags::Compute_mask ).set_name( "visibility_cache" )
        .set_format_type( VK_FORMAT_R16G16B16A16_SFLOAT, TextureType::Texture_2D_Array )
        .set_size( adjusted_width, adjusted_height, 1 )
        .set_mips( 1 ).set_layers( 1 );

    ImageViewCreation image_view_creation{ .parent_image = ImageHandle(),
            .view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
            .sub_resource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };

    // Last 4 frames visibility values per light
    // Double buffered
    for ( u32 i = 0; i < 2; ++i ) {
        visibility_cache_image[ i ] = gpu.create_image(texture_creation);

        image_view_creation.parent_image = visibility_cache_image[ i ];
        image_view_creation.name = texture_creation.name;
        visibility_cache_image_view[ i ] = gpu.create_image_view( image_view_creation );

        // NOTE(marco): last 4 frames visibility variation per light
        texture_creation.set_name( "variation_cache" );
        variation_cache_image[ i ] = gpu.create_image( texture_creation );

        image_view_creation.parent_image = variation_cache_image[ i ];
        image_view_creation.name = texture_creation.name;
        variation_cache_image_view[ i ] = gpu.create_image_view( image_view_creation );
    }

    // Visibility delta
    texture_creation.set_name( "variation" ).set_format_type( VK_FORMAT_R16_SFLOAT, TextureType::Texture_2D_Array );
    variation_image = gpu.create_image( texture_creation );

    image_view_creation.parent_image = variation_image;
    image_view_creation.name = texture_creation.name;
    variation_image_view = gpu.create_image_view( image_view_creation );

    texture_creation.set_name( "filtered_variation" );
    filtered_variation_image = gpu.create_image( texture_creation );

    image_view_creation.parent_image = filtered_variation_image;
    image_view_creation.name = texture_creation.name;
    filtered_variation_image_view = gpu.create_image_view( image_view_creation );

    // Store visibility + used depth
    texture_creation.set_name( "filtered_visibility" );
    texture_creation.format = VK_FORMAT_R16G16_SFLOAT;
    filtered_visibility_image = gpu.create_image( texture_creation );

    image_view_creation.parent_image = filtered_visibility_image;
    image_view_creation.name = texture_creation.name;
    filtered_visibility_image_view = gpu.create_image_view( image_view_creation );

    // Last 4 frames samples count per light
    for ( u32 i = 0; i < 2; ++i ) {
        texture_creation.set_name( "samples_count_cache" ).set_format_type( VK_FORMAT_R8G8B8A8_UINT, TextureType::Texture_2D_Array );
        samples_count_cache_image[ i ] = gpu.create_image( texture_creation );

        image_view_creation.parent_image = samples_count_cache_image[ i ];
        image_view_creation.name = texture_creation.name;
        samples_count_cache_image_view[ i ] = gpu.create_image_view( image_view_creation );

        gpu.link_image_sampler( samples_count_cache_image[ i ], gpu.global_samplers[GlobalSamplers::NearestClamp] );
    }

    for ( u32 i = 0; i < 2; ++i ) {
        gpu.add_image_view_to_bindless( variation_cache_image_view[ i ] );
        gpu.add_image_view_to_bindless( visibility_cache_image_view[ i ] );
        gpu.add_image_view_to_bindless( samples_count_cache_image_view[ i ] );
    }

    gpu.add_image_view_to_bindless( variation_image_view );
    gpu.add_image_view_to_bindless( filtered_visibility_image_view );
    gpu.add_image_view_to_bindless( filtered_variation_image_view );

    clear_resources = true;
    last_active_lights_count = lights_count;
}

void ShadowVisibilityPass::create_gpu_resources( FrameGraphResourceContext& context ) {

    FrameGraph* frame_graph = context.frame_graph;
    RenderScene* scene = context.render_scene;

    FrameGraphNode* node = frame_graph->get_node( "shadow_visibility_pass" );
    if ( node == nullptr ) {
        enabled = false;

        return;
    }

    enabled = node->enabled;

    GpuDevice& gpu = *context.renderer->gpu;

    // Use half resolution textures
    texture_scale = 0.5f;

    RenderBlackboard& blackboard = *context.render_blackboard;
    recreate_textures( gpu, scene->active_lights, blackboard.render_width, blackboard.render_height );

    cstring shadow_visibility_resource_name = "shadow_visibility";
    
    shadow_visibility_resource = frame_graph->get_resource( shadow_visibility_resource_name );
    RASSERT( shadow_visibility_resource != nullptr );

    FrameGraphResource* resource = frame_graph->get_resource( "gbuffer_normals" );
    RASSERT( resource != nullptr );
    cached_normals_image_view = resource->resource_info.texture.image_view;
}

void ShadowVisibilityPass::upload_gpu_data( FrameGraphResourceContext& context ) {
    if ( !enabled ) {
        return;
    }

    RenderBlackboard& render_blackboard = *context.render_blackboard;
    Renderer* renderer = context.renderer;

    GpuShadowVisibilityConstants* constants = renderer->gpu->dynamic_buffer_allocate<GpuShadowVisibilityConstants>( &constants_offset );
    if ( constants != nullptr ) {
        const u32 current_cache_index = renderer->gpu->absolute_frame & 1;
        const u32 previous_cache_index = current_cache_index ^ 1;

        constants->previous_visibility_cache_texture_index = visibility_cache_image_view[ previous_cache_index ].index();
        constants->current_visibility_cache_texture_index = visibility_cache_image_view[ current_cache_index ].index();
        constants->previous_variation_cache_texture_index = variation_cache_image_view[ previous_cache_index ].index();
        constants->current_variation_cache_texture_index = variation_cache_image_view[ current_cache_index ].index();
        constants->previous_samples_count_cache_texture_index = samples_count_cache_image_view[ previous_cache_index ].index();
        constants->current_samples_count_cache_texture_index = samples_count_cache_image_view[ current_cache_index ].index();
        constants->variation_texture_index = variation_image_view.index();        
        constants->motion_vectors_texture_index = render_blackboard.visibility_motion_vector_image_view.index();
        constants->normals_texture_index = cached_normals_image_view.index();
        constants->filtered_visibility_texture = filtered_visibility_image_view.index();
        constants->filtered_variation_texture = filtered_variation_image_view.index();
        constants->frame_index = renderer->gpu->absolute_frame % 4;
        constants->resolution_scale = texture_scale;
        constants->resolution_scale_rcp = 1.0f / texture_scale;

        FrameGraphResource* depth_resource = context.frame_graph->get_resource( "depth" );
        RASSERT( depth_resource );
        constants->current_depth_texture_index = depth_resource->resource_info.texture.image_view.index();
        constants->previous_depth_texture_index = depth_resource->resource_info.texture.previous_image_view.index();

        RaytracedShadowsConfig& rt_shadows_config = context.render_config->raytraced_shadows;

        constants->upscaled_visibility_texture = shadow_visibility_resource->resource_info.texture.image_view.index();
        constants->disable_history = rt_shadows_config.disable_history ? 1 : 0;
        constants->max_samples = rt_shadows_config.max_samples;
        constants->disable_spatial = rt_shadows_config.disable_spatial ? 1 : 0;
    }
}

void ShadowVisibilityPass::destroy_gpu_resources( FrameGraphResourceContext& context ) {
    if ( !enabled ) {
        return;
    }

    GpuDevice& gpu = *context.renderer->gpu;

    for ( u32 i = 0; i < k_max_frames; ++i ) {
        gpu.destroy_descriptor_set( descriptor_set[ i ] );
    }

    for ( u32 i = 0; i < 2; ++i ) {
        gpu.destroy_image( visibility_cache_image[ i ] );
        gpu.destroy_image( variation_cache_image[ i ] );
        gpu.destroy_image( samples_count_cache_image[ i ] );

        gpu.destroy_image_view( variation_cache_image_view[ i ] );
        gpu.destroy_image_view( visibility_cache_image_view[ i ] );
        gpu.destroy_image_view( samples_count_cache_image_view[ i ] );
    }

    gpu.destroy_image( variation_image );
    gpu.destroy_image( filtered_visibility_image );
    gpu.destroy_image( filtered_variation_image );

    gpu.destroy_image_view( variation_image_view );
    gpu.destroy_image_view( filtered_visibility_image_view );
    gpu.destroy_image_view( filtered_variation_image_view );
}

void ShadowVisibilityPass::create_descriptors( Renderer* renderer, RenderBlackboard* render_blackboard ) {
    ShaderReflectionInfo* reflection_info = renderer->get_shader_reflection( variance_pipeline.pipeline );

    DescriptorSetBinder descriptors;

    for ( u32 i = 0; i < k_max_frames; ++i ) {
        renderer->gpu->destroy_descriptor_set( descriptor_set[ i ] );

        descriptors.reset();
        descriptors.name = "shadow_visibility_pass_descriptor_set";
        descriptors.dynamic_buffers.push( { 30, sizeof( GpuShadowVisibilityConstants ) } );

        descriptor_set[ i ] = renderer->create_descriptor_set( descriptors, reflection_info, variance_pipeline.pipeline, i, *render_blackboard );
    }
}

} // namespace raptor
