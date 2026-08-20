#include "graphics/render_passes/bloom_pass.hpp"
#include "graphics/render_scene.hpp"
#include "graphics/render_blackboard.hpp"

namespace raptor {

void BloomPass::declare_frame_graph_node( FrameGraphResourceContext& context ) {
    FrameGraphBuilder& builder = *context.frame_graph->builder;

    context.frame_graph->add_node_v2( {
        .inputs = {
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "hdr_color_copy_pass", "hdr_color_copy" )
            },
        },
        .outputs = {
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Attachment,
                .resource_info{
                    .external = true
                },
                .name = "bloom",
            } ),
        },
        .scheduling = { CommandQueueType::Compute, 0 },
        .enabled = true,
        .compute = true,
        .name = k_name } );
}

void BloomPass::add_ui() {

}

void BloomPass::update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) {

    Renderer* renderer = context.renderer;

    if ( phase == PipelineUpdatePhase::Destroy ) {
        renderer->destroy_compute_pipeline_state( downsample_pipeline );
        renderer->destroy_compute_pipeline_state( upsample_pipeline );

        return;
    }

    ComputePipelineTransaction transaction( renderer );

    ComputePipelineState& new_downsample_pipeline = transaction.add( downsample_pipeline );
    ComputePipelineState& new_upsample_pipeline = transaction.add( upsample_pipeline );

    renderer->create_compute_pipeline_state( {
        .stages = {
            {
                .source_file_path = "glsl/chapter5/post_bloom.glsl",
                .type = VK_SHADER_STAGE_COMPUTE_BIT,
            }
        },
        .name = "bloom_downsample" },
        {
            .name = "bloom_downsample",
            .render_pass_name = k_name,
        },
        "bloom_downsample", context.frame_graph, new_downsample_pipeline );

    renderer->create_compute_pipeline_state( {
        .stages = {
            {
                .source_file_path = "glsl/chapter5/post_bloom.glsl",
                .type = VK_SHADER_STAGE_COMPUTE_BIT,
            }
        },
        .name = "bloom_upsample" },
        {
            .name = "bloom_downsample",
            .render_pass_name = k_name,
        },
        "bloom_upsample", context.frame_graph, new_upsample_pipeline );

    transaction.commit_or_rollback();
}

struct GpuBloomConstants {
    u32         source_texture_index;
    u32         destination_texture_index;

    f32         filter_radius;
    u32         pad111;

    glm::vec2   rcp_source_texture_size;
    glm::vec2   rcp_destination_texture_size;

};

void BloomPass::post_render( FrameGraphRenderContext& context ) {

    // Avoid copying first frame as source image is not ready
    Renderer* renderer = context.renderer;
    GpuDevice* gpu = renderer->gpu;
    if ( gpu->absolute_frame == 0 ) {
        return;
    }

    CommandBuffer* gpu_commands = context.gpu_commands;
    RenderScene* render_scene = context.render_view->scene;

    FrameGraphResource* lighting_resource = context.frame_graph->get_resource( "hdr_color_copy" );
    Image* bloom_image_data = gpu->get_image( bloom_image );

    u32 width = bloom_image_data->width;
    u32 height = bloom_image_data->height;

    // Downsample
    gpu_commands->bind_pipeline( downsample_pipeline.pipeline );

    for ( u32 i = 0; i < bloom_image_views.size; i++ ) {

        u32 mip_w = width >> i;
        u32 mip_h = height >> i;

        //util_add_image_barrier( &gpu, gpu_commands->vk_command_buffer, bloom_image_data->vk_image, RESOURCE_STATE_UNDEFINED, RESOURCE_STATE_UNORDERED_ACCESS, i, 1, false );

        u32 cb_offset = 0;
        GpuBloomConstants* gpu_data = renderer->gpu->dynamic_buffer_allocate<GpuBloomConstants>( &cb_offset );
        if ( gpu_data ) {
            gpu_data->source_texture_index = ( i == 0 ) ? lighting_resource->resource_info.texture.image_view.index() : bloom_image_views[ i - 1 ].index();
            gpu_data->destination_texture_index = bloom_image_views[ i ].index();
            gpu_data->rcp_destination_texture_size = glm::vec2{ 1.f / mip_w, 1.f / mip_h };
            gpu_data->rcp_source_texture_size = glm::vec2{ 1.f / ( mip_w * 2 ), 1.f / ( mip_h * 2 ) };
            //rprint( "index %d %d, offset %d\n", gpu_data->source_texture_index, gpu_data->destination_texture_index, cb_offset );
        }

        gpu_commands->bind_descriptor_set( { renderer->gpu->bindless_descriptor_set, descriptor_set },
                                            { cb_offset } );

        u32 group_x = ( mip_w + 7 ) / 8;
        u32 group_y = ( mip_h + 7 ) / 8;

        gpu_commands->dispatch( group_x, group_y, 1 );

        gpu_commands->barrier_instant_compute_write_to_compute_read();
    }

    // Upsample
    gpu_commands->bind_pipeline( upsample_pipeline.pipeline );

    for ( i32 i = bloom_image_views.size - 1; i > 0; i-- ) {

        //util_add_image_barrier( &gpu, gpu_commands->vk_command_buffer, bloom_image_data->vk_image, RESOURCE_STATE_UNDEFINED, RESOURCE_STATE_UNORDERED_ACCESS, i, 1, false );

        // Mip i
        const u32 src_w = bloom_image_data->width >> i;
        const u32 src_h = bloom_image_data->height >> i;

        // Mip i-1
        const u32 dst_w = bloom_image_data->width >> ( i - 1 );
        const u32 dst_h = bloom_image_data->height >> ( i - 1 );

        u32 cb_offset = 0;
        GpuBloomConstants* gpu_data = renderer->gpu->dynamic_buffer_allocate<GpuBloomConstants>( &cb_offset );
        if ( gpu_data ) {
            gpu_data->source_texture_index = bloom_image_views[ i ].index();
            gpu_data->destination_texture_index = bloom_image_views[ i - 1 ].index();
            gpu_data->rcp_source_texture_size = { 1.f / src_w, 1.f / src_h };
            gpu_data->rcp_destination_texture_size = { 1.f / dst_w, 1.f / dst_h };
            gpu_data->filter_radius = 0.005f;
        }

        gpu_commands->bind_descriptor_set( { renderer->gpu->bindless_descriptor_set, descriptor_set },
                                            { cb_offset } );

        u32 group_x = ( dst_w + 7 ) / 8;
        u32 group_y = ( dst_h + 7 ) / 8;

        gpu_commands->dispatch( group_x, group_y, 1 );

        gpu_commands->barrier_instant_compute_write_to_compute_read();
    }
}

// Utility methods ///////////////////////////////////////////////
u32 calculate_mip_levels( u32 width, u32 height ) {
    u32 mip_levels = 0;
    while ( width >= 16 && height >= 16 ) {
        mip_levels++;
        width /= 2;
        height /= 2;
    }
    return mip_levels;
}

void BloomPass::create_image_views_for_mipmaps( GpuDevice& gpu, FrameGraph* frame_graph, u32 mip_levels ) {
    // Create views for each mipmap
    ImageViewCreation image_view_creation{
        .parent_image = bloom_image,
        .view_type = VK_IMAGE_VIEW_TYPE_2D, };
    for ( u32 i = 0; i < mip_levels; i++ ) {
        image_view_creation.sub_resource = { VK_IMAGE_ASPECT_COLOR_BIT, i, 1, 0, 1 };
        bloom_image_views.push( gpu.create_image_view( image_view_creation ) );
        gpu.add_image_view_to_bindless( bloom_image_views[ i ] );
    }
}

void BloomPass::set_external_framegraph_resource( FrameGraph* frame_graph, GpuDevice& gpu ) {
    FrameGraphResource* bloom_tex = frame_graph->get_resource( "bloom" );
    RASSERT( bloom_tex );

    bloom_tex->resource_info.set_external_texture_2d(
        gpu.get_image( bloom_image )->width,
        gpu.get_image( bloom_image )->height,
        VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
        bloom_image,
        bloom_image_views[ 0 ] );
}

void BloomPass::on_resize( FrameGraphResourceContext& context, u32 new_width, u32 new_height ) {

    Renderer* renderer = context.renderer;
    FrameGraph* frame_graph = context.frame_graph;
    GpuDevice& gpu = *renderer->gpu;

    FrameGraphNode* node = frame_graph->get_node( k_name );
    if ( node == nullptr ) {
        RASSERT( false );
        return;
    }

    // Remove old image views from bindless and destroy them
    // NOTE: image can be just re-created, but we may have different mip levels and
    // thus different image views.
    for ( u32 i = 0; i < bloom_image_views.size; i++ ) {
        gpu.remove_image_view_from_bindless( bloom_image_views[ i ] );
        gpu.destroy_image_view( bloom_image_views[ i ] );
    }

    bloom_image_views.clear();

    // Resize image
    u32 mip_levels = calculate_mip_levels( new_width / 2, new_height / 2 );
    gpu.resize_image( bloom_image, new_width / 2, new_height / 2, mip_levels );

    create_image_views_for_mipmaps( gpu, frame_graph, mip_levels );
    set_external_framegraph_resource( frame_graph, gpu );
}

void BloomPass::create_gpu_resources( FrameGraphResourceContext& context ) {

    Renderer* renderer = context.renderer;
    GpuDevice& gpu = *renderer->gpu;

    FlatHashMapIterator it = renderer->resource_cache.pipelines.find( hash_calculate( "bloom_downsample" ) );
    RASSERT( it.is_valid() );

    PipelineHandle pipeline = renderer->resource_cache.pipelines.get( it );
    DescriptorSetLayoutHandle layout_handle = gpu.get_descriptor_set_layout( pipeline, k_material_descriptor_set_index );
    ShaderReflectionInfo* reflection_info = renderer->get_shader_reflection( pipeline );

    FrameGraphResource* lighting_resource = context.frame_graph->get_resource( "final" );

    const RenderBlackboard& blackboard = *context.render_blackboard;

    u32 mip_levels = calculate_mip_levels( blackboard.render_width / 2, blackboard.render_height / 2 );

    // Create image
    ImageCreation image_creation{ };
    image_creation.set_format_type( VK_FORMAT_R16G16B16A16_SFLOAT, TextureType::Enum::Texture2D )
        .set_flags( TextureFlags::Compute_mask )
        .set_size( blackboard.render_width / 2, blackboard.render_height / 2, 1 )
        .set_name( "bloom" ).set_mips( mip_levels );

    bloom_image = gpu.create_image( image_creation );

    RASSERT( bloom_image_views.size == 0 );
    create_image_views_for_mipmaps( gpu, context.frame_graph, mip_levels );
    set_external_framegraph_resource( context.frame_graph, gpu );

    descriptor_set = gpu.create_descriptor_set( {
        .dynamic_buffers = { {.binding = renderer->get_binding_index( reflection_info, "bloom_locals" ), .size = sizeof( GpuBloomConstants ) }}, 
        .layout = layout_handle } );
}

void BloomPass::destroy_gpu_resources( FrameGraphResourceContext& context ) {
    Renderer* renderer = context.renderer;

    renderer->gpu->destroy_image( bloom_image );

    for ( u32 i = 0; i < bloom_image_views.size; i++ ) {
        renderer->gpu->destroy_image_view( bloom_image_views[ i ] );
    }

    renderer->gpu->destroy_descriptor_set( descriptor_set );
}

} // namespace raptor