#include "graphics/render_passes/lighting_pass.hpp"
#include "graphics/render_scene.hpp"
#include "graphics/render_blackboard.hpp"

#include "foundation/numerics.hpp"

namespace raptor {

//
// LightingPass ///////////////////////////////////////////////////////////

static FrameGraphResource* get_output_texture( FrameGraph* frame_graph, FrameGraphResourceHandle input ) {
    FrameGraphResource* input_resource = frame_graph->access_resource( input );

    FrameGraphResource* output_resource = frame_graph->access_resource( input_resource->output_handle );
    RASSERT( output_resource != nullptr );

    return output_resource;
}

//
//
struct LightPassConstants {
    u32             albedo_index;
    u32             rmo_index;
    u32             normal_index;
    u32             depth_index;

    u32             output_index;
    u32             output_width;
    u32             output_height;
    u32             emissive;

    u32             gi_index;
    u32             reflection_index;
    u32             pad[2];
}; // struct LightingConstants

void LightingPass::declare_frame_graph_node( FrameGraphResourceContext& context ) {
    FrameGraphBuilder& builder = *context.frame_graph->builder;

    ArenaAllocator* temp_allocator = MemoryService::instance()->get_thread_allocator();
    ArenaScope temp_scope( temp_allocator );

    Array<FrameGraphResourceCreation_v2> inputs;
    inputs.init( temp_allocator, 16 );

    inputs.push( { .type = FrameGraphResourceType_Texture,
                   .handle = builder.get_output_handle( "gbuffer_pass_late", "gbuffer_colour" ) } );
    inputs.push( { .type = FrameGraphResourceType_Texture,
                   .handle = builder.get_output_handle( "gbuffer_pass_late", "gbuffer_normals" ) } );
    inputs.push( { .type = FrameGraphResourceType_Texture,
                   .handle = builder.get_output_handle( "gbuffer_pass_late", "gbuffer_metallic_roughness_occlusion" ) } );
    inputs.push( { .type = FrameGraphResourceType_Texture,
                   .handle = builder.get_output_handle( "gbuffer_pass_late", "gbuffer_emissive" ) } );
    inputs.push( { .type = FrameGraphResourceType_Texture,
                   .handle = builder.get_output_handle( "gbuffer_pass_late", "depth" ) } );

    if ( !context.render_config->shadows.disable_shadows ) {
        inputs.push( { .type = FrameGraphResourceType_Texture,
                       .handle = builder.get_output_handle( "point_shadows_pass", "point_shadows_depth" ) } );
    }
    
    if ( context.render_config->restirgi.enabled ) {
        inputs.push( { .type = FrameGraphResourceType_Texture,
                       .handle = builder.get_output_handle( "svgf_wavelet_pass", "restirgi_denoised_output" ) } );
    }

    if ( context.render_config->raytraced_reflections.enabled ) {
        inputs.push( { .type = FrameGraphResourceType_Texture,
                       .handle = builder.get_output_handle( "svgf_wavelet_pass", "reflections_denoised_output" ) } );
    }
    
    context.frame_graph->add_node_v2( {
        .inputs = inputs.as_cspan(),
        .outputs = {
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Attachment,
                .resource_info{
                    .texture = {
                        .scale_width = 1.0f,
                        .scale_height = 1.0f,
                        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                        .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
                        .compute = true,
                        .persistent = true
                    }
                },
                .name = "final",
            } ),
        },
        .scheduling = { CommandQueueType::Graphics, 1 },
        .enabled = true,
        .compute = true,
        .name = "lighting_pass" } );
}

void LightingPass::update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) {

    Renderer* renderer = context.renderer;

    if ( phase == PipelineUpdatePhase::Destroy ) {
        renderer->destroy_compute_pipeline_state( compute_pipeline );

        return;
    }

    ShaderCompilationCreation scc = {
        .stages = {
            {
                .source_file_path = "glsl/pbr.glsl",
                .type = VK_SHADER_STAGE_COMPUTE_BIT,
            },
        },
        .name = "deferred_lighting_compute",
        .slang_input = 0,
    };

    if ( context.render_config->raytraced_shadows.enabled ) {
        scc.stages[ 0 ].defines.push( "RAYTRACED_SHADOWS" );
    }

    PipelineCreation pipeline_creation = {
        .name = "deferred_lighting_compute",
        .render_pass_name = "lighting_pass",
    };

    ComputePipelineTransaction transaction( renderer );

    ComputePipelineState& pipeline = transaction.add( compute_pipeline );

    renderer->create_compute_pipeline_state( scc, pipeline_creation,
                                             "compute_lighting", context.frame_graph, pipeline );

    transaction.commit_or_rollback();
}

void LightingPass::render( FrameGraphRenderContext& context ) {

    if ( !enabled )
        return;

    RenderScene* render_scene = context.render_view->scene;
    u32 current_frame_index = context.current_frame_index;
    CommandBuffer* gpu_commands = context.gpu_commands;
    Renderer* renderer = context.renderer;
    RenderBlackboard& render_blackboard = *context.render_blackboard;

    if ( use_compute ) {

        if ( gi_texture ) {
            gpu_commands->add_image_barrier( gi_texture->resource_info.texture.image, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS ),
                                             { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                               VK_ACCESS_2_SHADER_READ_BIT,
                                               VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL } );
        }
        
        if ( reflections_texture ) {
            gpu_commands->add_image_barrier( reflections_texture->resource_info.texture.image, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS ),
                                             { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                               VK_ACCESS_2_SHADER_READ_BIT,
                                               VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL } );
        }
        
        gpu_commands->flush_barriers();

        gpu_commands->bind_pipeline( compute_pipeline.pipeline );

        gpu_commands->bind_descriptor_set( { renderer->gpu->bindless_descriptor_set, lighting_descriptor_set[ current_frame_index ] },
                                            { render_blackboard.scene_cb_offset, constants_offset, render_blackboard.lighting.lighting_constants_cb_offset } );

        gpu_commands->dispatch( ceilu32( render_blackboard.render_width * 1.f / 8 ), ceilu32( render_blackboard.render_height * 1.f / 8 ), 1 );
    } else {
        /*PipelineHandle pipeline = renderer->get_pipeline( material, 0 );

        gpu_commands->bind_pipeline( pipeline );
        gpu_commands->bind_vertex_buffer( renderer->gpu->get_fullscreen_vertex_buffer(), 0, 0 );

        gpu_commands->bind_descriptor_set( { renderer->gpu->bindless_descriptor_set, lighting_descriptor_set[ current_frame_index ] },
                                            { render_blackboard.scene_cb_offset, constants_offset, render_blackboard.lighting_constants_cb_offset } );

        gpu_commands->draw( TopologyType::Triangle, 0, 3, 0, 1 );*/
    }
}
void LightingPass::on_resize( FrameGraphResourceContext& context, u32 new_width, u32 new_height ) {
    if ( !enabled )
        return;

    Renderer* renderer = context.renderer;
    FrameGraph* frame_graph = context.frame_graph;
    GpuDevice& gpu = *renderer->gpu;

    FrameGraphResource* resource = frame_graph->get_resource( "shading_rate_image" );
    if ( resource ) {
        u32 adjusted_width = ( new_width + gpu.min_fragment_shading_rate_texel_size.width - 1 ) / gpu.min_fragment_shading_rate_texel_size.width;
        u32 adjusted_height = ( new_height + gpu.min_fragment_shading_rate_texel_size.height - 1 ) / gpu.min_fragment_shading_rate_texel_size.height;
        gpu.resize_image( resource->resource_info.texture.image, adjusted_width, adjusted_height );
        gpu.recreate_image_view( resource->resource_info.texture.image_view );
        gpu.add_image_view_to_bindless( resource->resource_info.texture.image_view );

        resource->resource_info.texture.width = adjusted_width;
        resource->resource_info.texture.height = adjusted_height;
    }

    create_descriptors( context );
}

void LightingPass::post_render( FrameGraphRenderContext& context ) {

    RenderScene* render_scene = context.render_view->scene;
    u32 current_frame_index = context.current_frame_index;
    CommandBuffer* gpu_commands = context.gpu_commands;

    //if ( gpu_commands->gpu_device->fragment_shading_rate_present && !use_compute ) {
    //    Image* attachment_image = renderer->gpu->get_image( output_texture->resource_info.texture.image );
    //    Image* frs_image = renderer->gpu->get_image( render_blackboard.fragment_shading_rate_image );

    //    //util_add_image_barrier( renderer->gpu, gpu_commands->vk_command_buffer, attachment_image,
    //                            //RESOURCE_STATE_SHADER_RESOURCE, 0, 1, false );

    //   // util_add_image_barrier( renderer->gpu, gpu_commands->vk_command_buffer, frs_image,
    //    //                        RESOURCE_STATE_UNORDERED_ACCESS, 0, 1, false );

    //    u32 filter_size = 16;
    //    u32 workgroup_x = ( attachment_image->width + ( filter_size - 1 ) ) / filter_size;
    //    u32 workgroup_y = ( attachment_image->height + ( filter_size - 1 ) ) / filter_size;

    //    PipelineHandle pipeline = renderer->get_pipeline( material, 2 );
    //    gpu_commands->bind_pipeline( pipeline );
    //    gpu_commands->bind_descriptor_set(
    //        { renderer->gpu->bindless_descriptor_set, fragment_rate_descriptor_set[ current_frame_index ] },
    //        { render_blackboard.scene_cb_offset } );

    //    gpu_commands->dispatch( workgroup_x, workgroup_y, 1 );

    //    //util_add_image_barrier( renderer->gpu, gpu_commands->vk_command_buffer, frs_image,
    //    //                        RESOURCE_STATE_SHADING_RATE_SOURCE, 0, 1, false );
    //}
}

void LightingPass::create_gpu_resources( FrameGraphResourceContext& context ) {

    FrameGraph* frame_graph = context.frame_graph;
    RenderScene* scene = context.render_scene;

    FrameGraphNode* node = frame_graph->get_node( "lighting_pass" );
    if ( node == nullptr ) {
        enabled = false;

        return;
    }

    enabled = node->enabled;
    if ( !enabled )
        return;

    use_compute = node->compute;

    Renderer* renderer = context.renderer;
    const u64 hashed_name = hash_calculate( "pbr_lighting" );
    
    color_texture = get_output_texture( frame_graph, node->inputs[ 0 ] );
    normal_texture = get_output_texture( frame_graph, node->inputs[ 1 ] );
    roughness_texture = get_output_texture( frame_graph, node->inputs[ 2 ] );
    emissive_texture = get_output_texture( frame_graph, node->inputs[ 3 ] );
    depth_texture = get_output_texture( frame_graph, node->inputs[ 4 ] );

    if ( context.render_config->restirgi.enabled ) {
        gi_texture = frame_graph->get_resource( "restirgi_denoised_output" );
    }
    else {
        gi_texture = nullptr;
    }
    
    if ( context.render_config->raytraced_shadows.enabled ) {
        reflections_texture = frame_graph->get_resource( "reflections_denoised_output" );
    }
    else {
        reflections_texture = nullptr;
    }

    output_texture = frame_graph->access_resource( node->outputs[ 0 ] );

    // Create debug texture
    ImageCreation texture_creation;
    texture_creation.set_size( 1280, 800, 1 ).set_layers( 1 ).set_mips( 1 ).set_format_type( VK_FORMAT_R16G16B16A16_SFLOAT, TextureType::Texture2D )
        .set_flags( TextureFlags::RenderTarget_mask | TextureFlags::Compute_mask ).set_name( "lighting_debug_texture" );

    lighting_debug_texture = renderer->gpu->create_image( texture_creation );
    lighting_debug_image_view = renderer->gpu->create_image_view( {
        .parent_image = lighting_debug_texture,
        .view_type = VK_IMAGE_VIEW_TYPE_2D,
        .sub_resource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        .name = "lighting_debug_image_view" });

    renderer->gpu->add_image_view_to_bindless( lighting_debug_image_view );

    context.render_blackboard->lighting_debug_texture_index = lighting_debug_image_view.index();

    for ( u32 f = 0; f < k_max_frames; ++f ) {
        fragment_rate_descriptor_set[ f ] = {};
        fragment_rate_texture_index[ f ] = {};
    }

    //if ( renderer->gpu->fragment_shading_rate_present && !use_compute ) {

    //    // TODO (gabriel): this needs to be implemented.
    //    //Texture* colour_texture = renderer->gpu->access_texture( color_texture->resource_info.texture.handle );

    //    u32 frs_pass_index = main_technique->get_pass_index( "edge_detection" );
    //    GpuTechniquePass& pass = main_technique->passes[ frs_pass_index ];

    //    //BufferCreation buffer_creation{ };
    //    //buffer_creation.set_name( "fragment_rate_texture_index" ).set(  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, ResourceUsageType::Dynamic, sizeof( u32 ) * 2 );

    //    DescriptorSetBinder descriptors;

    //    for ( u32 f = 0; f < k_max_frames; ++f ) {
    //        fragment_rate_texture_index[ f ] = renderer->gpu->create_buffer( {
    //            .type_flags = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
    //            .usage = ResourceUsageType::Dynamic, .size = sizeof( u32 ) * 2 } );

    //        DescriptorSetHandle ds_handle = fragment_rate_descriptor_set[ f ];

    //        renderer->gpu->destroy_descriptor_set( ds_handle );

    //        DescriptorSetLayoutHandle frs_layout = renderer->gpu->get_descriptor_set_layout( pass.pipeline, k_material_descriptor_set_index );

    //        descriptors.reset();
    //        descriptors.ssbos.push( { fragment_rate_texture_index[ f ], 2 } );
    //        //ds_creation.buffer( light_pass_constants, 1 );

    //        fragment_rate_descriptor_set[ f ] = scene->create_descriptor_set( descriptors, pass, frs_layout, f );
    //    }
    //}

    create_descriptors( context );
}

void LightingPass::update_dependent_resources( FrameGraphResourceContext& context ) {

    if ( !enabled )
        return;

    create_descriptors( context );
}

void LightingPass::create_descriptors( FrameGraphResourceContext& context ) {

    Renderer* renderer = context.renderer;
    GpuDevice* gpu = renderer->gpu;
    RenderBlackboard& render_blackboard = *context.render_blackboard;

    DescriptorSetBinder descriptors;

    PipelineHandle pipeline = renderer->resource_cache.pipelines.get( hash_calculate( "deferred_lighting_compute" ) );
    ShaderReflectionInfo* shader_reflection = renderer->get_shader_reflection( pipeline );

    for ( u32 i = 0; i < k_max_frames; ++i ) {
        gpu->destroy_descriptor_set( lighting_descriptor_set[ i ] );

        descriptors.reset();
        descriptors.dynamic_buffers.push( { 1, sizeof( LightPassConstants ) } );
        descriptors.name = "light_compute_ds";

        lighting_descriptor_set[ i ] = renderer->create_descriptor_set( descriptors, shader_reflection, pipeline, i, render_blackboard );
    }
}

void LightingPass::upload_gpu_data( FrameGraphResourceContext& context ) {
    if ( !enabled )
        return;

    Renderer* renderer = context.renderer;
    u32 current_frame_index = renderer->gpu->current_frame;
    RenderBlackboard& render_blackboard = *context.render_blackboard;

    LightPassConstants* lighting_data = renderer->gpu->dynamic_buffer_allocate<LightPassConstants>( &constants_offset );
    if ( lighting_data ) {
        lighting_data->albedo_index = color_texture->resource_info.texture.image_view.index();
        lighting_data->rmo_index = roughness_texture->resource_info.texture.image_view.index();
        lighting_data->normal_index = normal_texture->resource_info.texture.image_view.index();
        lighting_data->depth_index = depth_texture->resource_info.texture.image_view.index();
        lighting_data->output_index = output_texture->resource_info.texture.image_view.index();
        lighting_data->output_width = render_blackboard.render_width;
        lighting_data->output_height = render_blackboard.render_height;
        lighting_data->emissive = emissive_texture->resource_info.texture.image_view.index();
        lighting_data->gi_index = gi_texture ? gi_texture->resource_info.texture.image_view.index() : k_invalid_texture_index;
        lighting_data->reflection_index = reflections_texture ? reflections_texture->resource_info.texture.image_view.index() : k_invalid_texture_index;
    }

    if ( renderer->gpu->fragment_shading_rate_present ) {

        for ( u32 f = 0; f < k_max_frames; ++f ) {

            MapBufferParameters cb_map = { fragment_rate_texture_index[ f ], 0, 0 };
            u32* frs_texture_indices = ( u32* )renderer->gpu->map_buffer( cb_map );

            if ( frs_texture_indices != nullptr ) {
                frs_texture_indices[ 0 ] = output_texture->resource_info.texture.image_view.index();
                frs_texture_indices[ 1 ] = render_blackboard.fragment_shading_rate_image_view.index();

                renderer->gpu->unmap_buffer( cb_map );
            }
        }
    }
}

void LightingPass::destroy_gpu_resources( FrameGraphResourceContext& context ) {
    if ( !enabled )
        return;

    Renderer* renderer = context.renderer;
    GpuDevice& gpu = *renderer->gpu;

    gpu.destroy_image( lighting_debug_texture );
    gpu.destroy_image_view( lighting_debug_image_view );

    for ( u32 f = 0; f < k_max_frames; ++f ) {
        gpu.destroy_buffer( fragment_rate_texture_index[ f ] );
        gpu.destroy_descriptor_set( fragment_rate_descriptor_set[ f ] );
        gpu.destroy_descriptor_set( lighting_descriptor_set[ f ] );
    }

    // TODO(marco): destroy scene.fragment_shading_rate_image
}

} // namespace raptor
