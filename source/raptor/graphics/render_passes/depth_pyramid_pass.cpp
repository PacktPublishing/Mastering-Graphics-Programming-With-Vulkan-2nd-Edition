#include "graphics/render_passes/depth_pyramid_pass.hpp"
#include "graphics/render_scene.hpp"
#include "graphics/render_blackboard.hpp"

namespace raptor {

//
// DepthPyramidPass ///////////////////////////////////////////////////////
void DepthPyramidPass::render( FrameGraphRenderContext& context ) {
    if ( !enabled )
        return;

    update_depth_pyramid = context.render_config->gpu_culling.freeze_occlusion_camera == false;
}

void DepthPyramidPass::post_render( FrameGraphRenderContext& context ) {
    if ( !enabled )
        return;

    GpuDevice* gpu = context.renderer->gpu;
    CommandBuffer* cb = context.gpu_commands;
    FrameGraph* frame_graph = context.frame_graph;

    Image* depth_pyramid_texture = gpu->get_image( depth_pyramid_image );

    if ( update_depth_pyramid ) {
        cb->bind_pipeline( pipeline.pipeline );

        u32 width = depth_pyramid_texture->width;
        u32 height = depth_pyramid_texture->height;

        FrameGraphResource* depth_resource = ( FrameGraphResource* )frame_graph->get_resource( "depth" );
        ImageHandle depth_handle = depth_resource->resource_info.texture.image;
        Image* depth_image = gpu->get_image( depth_handle );

        //util_add_image_barrier( gpu, cb->vk_command_buffer, depth_texture, RESOURCE_STATE_SHADER_RESOURCE, 0, 1, true );
        cb->add_image_barrier( depth_handle, range_aspect( VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 ),
                               { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                 VK_ACCESS_2_SHADER_READ_BIT,
                                 VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL } );

        cb->add_image_barrier( depth_resource->resource_info.texture.previous_image, range_aspect( VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 ),
                               { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                 VK_ACCESS_2_SHADER_READ_BIT,
                                 VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL } );

        cb->flush_barriers();

        for ( u32 mip_index = 0; mip_index < depth_pyramid_texture->mip_level_count; ++mip_index ) {
            //util_add_image_barrier( gpu, cb->vk_command_buffer, depth_pyramid_texture->vk_image, RESOURCE_STATE_UNDEFINED, RESOURCE_STATE_UNORDERED_ACCESS, mip_index, 1, false );
            cb->add_image_barrier( depth_pyramid_image, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, mip_index, 1, 0, 1 ),
                                   { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                     VK_ACCESS_2_SHADER_WRITE_BIT,
                                     VK_IMAGE_LAYOUT_UNDEFINED },
                                   { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                     VK_ACCESS_2_SHADER_WRITE_BIT,
                                     VK_IMAGE_LAYOUT_GENERAL } );
            cb->flush_barriers();

            cb->bind_descriptor_set( { gpu->bindless_descriptor_set, depth_hierarchy_descriptor_set[ mip_index ] }, {} );

            // NOTE(marco): local workgroup is 8 x 8
            u32 group_x = ( width + 7 ) / 8;
            u32 group_y = ( height + 7 ) / 8;

            cb->dispatch( group_x, group_y, 1 );

            //util_add_image_barrier( gpu, cb->vk_command_buffer, depth_pyramid_texture->vk_image, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_SHADER_RESOURCE, mip_index, 1, false );
            cb->add_image_barrier( depth_pyramid_image, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, mip_index, 1, 0, 1 ),
                                   { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                     VK_ACCESS_2_SHADER_WRITE_BIT,
                                     VK_IMAGE_LAYOUT_GENERAL },
                                   { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                     VK_ACCESS_2_SHADER_READ_BIT,
                                     VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL } );
            cb->flush_barriers();

            width /= 2;
            height /= 2;
        }

        // NOTE(marco): this is a bit wasteful, as we really only need to transition the first mip. Not doing this means
        // we should track the layout per mip/layer, which we don't need for now.
        cb->add_image_barrier( spd_depth_image, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS ),
                        { VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                            VK_ACCESS_2_TRANSFER_WRITE_BIT,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL } );

        cb->flush_barriers();

        cb->copy_image_mip0( depth_handle, { }, spd_depth_image, { } );

        cb->add_image_barrier( spd_depth_image, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS ),
            { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
              VK_IMAGE_LAYOUT_GENERAL } );

        cb->flush_barriers();

        u32 spd_group_x = ( depth_image->width + 63 ) / 64;
        u32 spd_group_y = ( depth_image->height + 63 ) / 64;

        u32 constants_offset = 0;
        SpdConstants* spd_constants_data = gpu->dynamic_buffer_allocate<SpdConstants>( &constants_offset );
        spd_constants_data->mips = depth_pyramid_levels;
        spd_constants_data->numWorkGroups =  spd_group_x * spd_group_y;
        spd_constants_data->workGroupOffset[ 0 ] = 0;
        spd_constants_data->workGroupOffset[ 1 ] = 0;

        cb->add_buffer_barrier( spd_atomic_buffer, 0, sizeof( u32 ) * 6,
                                { VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                  VK_ACCESS_2_TRANSFER_WRITE_BIT } );

        cb->flush_barriers();

        cb->fill_buffer( spd_atomic_buffer, 0, sizeof( u32 ) * 6, 0 );

        cb->add_buffer_barrier( spd_atomic_buffer, 0, sizeof( u32 ) * 6,
                                { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                  VK_ACCESS_2_SHADER_READ_BIT |
                                  VK_ACCESS_2_SHADER_WRITE_BIT } );
        cb->flush_barriers();

        cb->bind_pipeline( spd_pipeline.pipeline );
        cb->bind_descriptor_set(
            { gpu->bindless_descriptor_set, spd_descriptor_set }, { constants_offset } );

        cb->dispatch( spd_group_x, spd_group_y, 1 );
    }
}

void DepthPyramidPass::on_resize( FrameGraphResourceContext& context, u32 new_width, u32 new_height ) {

    FrameGraph* frame_graph = context.frame_graph;
    GpuDevice& gpu = *context.renderer->gpu;

    // Destroy old resources
    gpu.destroy_image( depth_pyramid_image );
    gpu.destroy_image( spd_depth_image );
    // Use old depth pyramid levels value
    for ( u32 i = 0; i < depth_pyramid_levels; ++i ) {
        gpu.destroy_descriptor_set( depth_hierarchy_descriptor_set[ i ] );
        gpu.destroy_image_view( depth_pyramid_image_views[ i ] );
    }

    for ( u32 i = 0; i < depth_pyramid_levels + 1; ++i ) {
        gpu.destroy_image_view( spd_depth_image_view_all_mips[ i ] );
    }
    gpu.destroy_descriptor_set( spd_descriptor_set );

    FrameGraphResource* depth_resource = ( FrameGraphResource* )frame_graph->get_resource( "depth" );
    ImageHandle depth_image = depth_resource->resource_info.texture.image;
    ImageViewHandle depht_image_view = depth_resource->resource_info.texture.image_view;

    create_depth_pyramid_resource( context.renderer, depth_image, depht_image_view );

    set_external_framegraph_resource( frame_graph, gpu );
}

void DepthPyramidPass::declare_frame_graph_node( FrameGraphResourceContext& context ) {
    FrameGraphBuilder& builder = *context.frame_graph->builder;

    context.frame_graph->add_node_v2( {
        .inputs = {
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "gbuffer_pass_early", "depth" )
            }
        },
        .outputs = {
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Attachment,
                .resource_info = {
                    .external = true
                },
                .name = "depth_pyramid",
            } ),
        },
        .scheduling = { CommandQueueType::Graphics, 0 },
        .enabled = true,
        .compute = true,
        .name = "depth_pyramid_pass" } );
}

void DepthPyramidPass::update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) {

    Renderer* renderer = context.renderer;

    if ( phase == PipelineUpdatePhase::Destroy ) {
        renderer->destroy_compute_pipeline_state( pipeline );
        renderer->destroy_compute_pipeline_state( spd_pipeline );

        return;
    }

    ShaderCompilationCreation scc = {
    .stages = {
        {
            .source_file_path = "glsl/depth_pyramid.glsl",
            .type = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    },
        .name = "depth_pyramid",
        .slang_input = 0,
    };

    PipelineCreation pipeline_creation = {
        .name = "depth_pyramid",
        .render_pass_name = "depth_pyramid_pass",
    };

    ShaderCompilationCreation spd_scc = {
    .stages = {
        {
            .source_file_path = "glsl/spd/ffx_spd_downsample_pass.glsl",
            .defines = { "FFX_GPU", "FFX_GLSL", "FFX_SPD_OPTION_DOWNSAMPLE_FILTER=2" },
            .type = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    },
        .name = "depth_spd",
        .slang_input = 0,
    };

    PipelineCreation spd_pipeline_creation = {
        .name = "depth_spd",
        .render_pass_name = "depth_pyramid_pass",
    };

    ComputePipelineTransaction transaction( renderer );

    ComputePipelineState& new_pipeline = transaction.add( pipeline );
    ComputePipelineState& new_spd_pipeline = transaction.add( spd_pipeline );

    renderer->create_compute_pipeline_state( scc, pipeline_creation,
                                             "depth_pyramid", context.frame_graph, new_pipeline );

    renderer->create_compute_pipeline_state( spd_scc, spd_pipeline_creation,
                                             "depth_spd", context.frame_graph, new_spd_pipeline );

    transaction.commit_or_rollback();
}

void DepthPyramidPass::create_gpu_resources( FrameGraphResourceContext& context ) {

    FrameGraph* frame_graph = context.frame_graph;

    FrameGraphNode* node = frame_graph->get_node( "depth_pyramid_pass" );
    if ( node == nullptr ) {
        enabled = false;

        return;
    }

    enabled = node->enabled;
    if ( !enabled )
        return;

    GpuDevice& gpu = *context.renderer->gpu;

    FrameGraphResource* depth_resource = ( FrameGraphResource* )frame_graph->get_resource( "depth" );
    ImageHandle depth_image = depth_resource->resource_info.texture.image;
    ImageViewHandle depht_image_view = depth_resource->resource_info.texture.image_view;

    // Sampler does not need to be recreated
    SamplerCreation sc;
    sc.set_address_mode_uvw( VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE )
        .set_min_mag_mip( VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST ).set_reduction_mode( VK_SAMPLER_REDUCTION_MODE_MAX ).set_name( "depth_pyramid_sampler" );
    depth_pyramid_sampler = gpu.create_sampler( sc );

    create_depth_pyramid_resource( context.renderer, depth_image, depht_image_view );

    set_external_framegraph_resource( frame_graph, gpu );
}

void DepthPyramidPass::destroy_gpu_resources( FrameGraphResourceContext& context ) {
    if ( !enabled )
        return;

    Renderer* renderer = context.renderer;
    GpuDevice& gpu = *renderer->gpu;

    gpu.destroy_sampler( depth_pyramid_sampler );
    gpu.destroy_image( depth_pyramid_image );

    for ( u32 i = 0; i < depth_pyramid_levels; ++i ) {
        gpu.destroy_image_view( depth_pyramid_image_views[ i ] );
        gpu.destroy_descriptor_set( depth_hierarchy_descriptor_set[ i ] );
    }

    for ( u32 i = 0; i < depth_pyramid_levels + 1; ++i ) {
        // This includes spd_depth_src_view
        gpu.destroy_image_view( spd_depth_image_view_all_mips[ i ] );
    }

    gpu.destroy_image( spd_depth_image );
    gpu.destroy_image_view( spd_depth_image_view_mid_mip );
    gpu.destroy_buffer( spd_atomic_buffer );
    gpu.destroy_descriptor_set( spd_descriptor_set );
}

void DepthPyramidPass::create_depth_pyramid_resource( Renderer* renderer, ImageHandle depth_image, ImageViewHandle depth_image_view ) {

    Image* depth_texture = renderer->gpu->get_image( depth_image );

    // TODO(marco): this assumes a pot depth resolution
    u32 width = depth_texture->width / 2;
    u32 height = depth_texture->height / 2;

    GpuDevice& gpu = *renderer->gpu;

    depth_pyramid_levels = 0;
    while ( width >= 2 && height >= 2 ) {
        depth_pyramid_levels++;

        width /= 2;
        height /= 2;
    }

    ImageCreation depth_hierarchy_creation{ };
    depth_hierarchy_creation.set_format_type( VK_FORMAT_R32_SFLOAT, TextureType::Enum::Texture2D ).set_flags( TextureFlags::Compute_mask ).set_size( depth_texture->width / 2, depth_texture->height / 2, 1 ).set_name( "depth_hierarchy" ).set_mips( depth_pyramid_levels );

    depth_pyramid_image = gpu.create_image( depth_hierarchy_creation );
    depth_hierarchy_creation.name = "spd_depth";
    depth_hierarchy_creation.set_size( depth_texture->width, depth_texture->height, 1 ).set_mips( depth_pyramid_levels + 1 );

    spd_depth_image = gpu.create_image( depth_hierarchy_creation );

    ImageViewCreation depth_pyramid_view_creation = {
        .parent_image = depth_pyramid_image,
        .view_type = VK_IMAGE_VIEW_TYPE_2D,
        .name = "depth_pyramid_view_creation"
    };

    //GpuTechnique* culling_technique = renderer->resource_cache.techniques.get( hash_calculate( "culling" ) );
    //depth_pyramid_pipeline = culling_technique->passes[ 1 ].pipeline;
    DescriptorSetLayoutHandle depth_pyramid_layout = gpu.get_descriptor_set_layout( pipeline.pipeline, k_material_descriptor_set_index );

    for ( u32 i = 0; i < depth_pyramid_levels; ++i ) {
        depth_pyramid_view_creation.sub_resource = { VK_IMAGE_ASPECT_COLOR_BIT, i, 1, 0, 1 };

        depth_pyramid_image_views[ i ] = gpu.create_image_view( depth_pyramid_view_creation );
        gpu.add_image_view_to_bindless( depth_pyramid_image_views[ i ] );

        if ( i == 0 ) {
            depth_hierarchy_descriptor_set[ i ] = gpu.create_descriptor_set( {
                .textures = { { depth_image_view, 0 } },
                .images = { { depth_pyramid_image_views[ i ], 1 } },
                .layout = depth_pyramid_layout,
                .name = "depth_pyramid0_ds" } );
        } else {
            depth_hierarchy_descriptor_set[ i ] = gpu.create_descriptor_set( {
                .textures = { { depth_pyramid_image_views[ i - 1 ], 0 } },
                .images = { { depth_pyramid_image_views[ i ], 1 } },
                .layout = depth_pyramid_layout,
                .name = "depth_pyramid1.._ds" } );
        }
    }

    if ( spd_atomic_buffer.is_invalid() ) {
        spd_atomic_buffer = gpu.create_buffer( {
            .size = sizeof( u32 ) * 6,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .name = "spd_atomic_buffer" } );
    }

    DescriptorSetLayoutHandle spd_depth_layout = gpu.get_descriptor_set_layout( spd_pipeline.pipeline, k_material_descriptor_set_index );

    ImageViewCreation spd_depth_view_creation = {
        .parent_image = spd_depth_image,
        .view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
        .name = "spd_depth_view_creation"
    };

    for ( u32 i = 0; i < depth_pyramid_levels + 1; ++i ) {
        spd_depth_view_creation.sub_resource = { VK_IMAGE_ASPECT_COLOR_BIT, i, 1, 0, VK_REMAINING_ARRAY_LAYERS };
        spd_depth_image_view_all_mips[ i ] = gpu.create_image_view( spd_depth_view_creation );
    }

    // NOTE(marco): the shader use a static array, so we need to fill all elements even if we won't use all of them
    for ( u32 i = depth_pyramid_levels + 1; i < k_max_depth_pyramid_levels + 1; ++i ) {
        spd_depth_image_view_all_mips[ i ] = spd_depth_image_view_all_mips[ depth_pyramid_levels ]; // point to last mip
    }

    spd_depth_image_view_mid_mip = spd_depth_image_view_all_mips[ 6 ];

    spd_descriptor_set = gpu.create_descriptor_set( {
        .textures = { { spd_depth_image_view_all_mips[ 0 ], 0 } },
        .images = { { spd_depth_image_view_mid_mip, 2001 } },
        .image_arrays = { { spd_depth_image_view_all_mips, 2002, u16( k_max_depth_pyramid_levels + 1 ) } },
        .ssbos = { { spd_atomic_buffer, 2000 } },
        .dynamic_buffers = { { 3000, sizeof( SpdConstants ) }, },
        .layout = spd_depth_layout, .name = "depth_pyramid_spd_ds" } );
}

void DepthPyramidPass::set_external_framegraph_resource( FrameGraph* frame_graph, GpuDevice& gpu ) {

    FrameGraphResource* depth_pyramid_resource = (FrameGraphResource*)frame_graph->get_resource( "depth_pyramid" );
    RASSERT( depth_pyramid_resource );

    depth_pyramid_resource->resource_info.set_external_texture_2d(
        gpu.get_image( spd_depth_image )->width,
        gpu.get_image( spd_depth_image )->height,
        VK_FORMAT_R32_SFLOAT,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
        spd_depth_image,
        spd_depth_image_view_all_mips[ 0 ] );

    gpu.link_image_sampler( spd_depth_image, depth_pyramid_sampler );
}

} // namespace raptor
