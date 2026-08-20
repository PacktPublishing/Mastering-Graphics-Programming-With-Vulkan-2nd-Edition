#include "graphics/render_passes/hdr_color_pass.hpp"
#include "graphics/render_scene.hpp"
#include "graphics/render_blackboard.hpp"

namespace raptor {

void HDRColorCopyPass::declare_frame_graph_node( FrameGraphResourceContext& context ) {
    FrameGraphBuilder& builder = *context.frame_graph->builder;

    context.frame_graph->add_node_v2( {
        .inputs = {
            /*{
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "transparent_pass", "final" )
            },*/
        },
        .outputs = {
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Attachment,
                .resource_info{
                    .texture = {
                        .scale_width = 1.0f,
                        .scale_height = 1.0f,
                        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                        .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
                        .disable_memory_aliasing = true
                    }
                },
                .name = "hdr_color_copy",
            } ),
        },
        .scheduling = { CommandQueueType::Compute, 0 },
        .enabled = true,
        .compute = true,
        .name = k_name } );
}

void HDRColorCopyPass::post_render( FrameGraphRenderContext& context ) {

    // Avoid copying first frame as source image is not ready
    Renderer* renderer = context.renderer;
    GpuDevice* gpu = renderer->gpu;
    if ( gpu->absolute_frame == 0 ) {
        return;
    }

    CommandBuffer* cb = context.gpu_commands;
    RenderScene* render_scene = context.render_view->scene;

    // Get previous frame final image
    FrameGraphResource* hdr_lighting_resource = context.frame_graph->get_resource( "final" );
    RASSERT( hdr_lighting_resource );
    ImageHandle hdr_lighting_image = hdr_lighting_resource->resource_info.texture.previous_image;
    //Image* hdr_image_data = gpu.get_image( hdr_lighting_resource->resource_info.texture.image );

    FrameGraphResource* hdr_copy_resource = context.frame_graph->get_resource( "hdr_color_copy" );
    RASSERT( hdr_copy_resource );
    ImageHandle hdr_copy_image = hdr_copy_resource->resource_info.texture.image;
    //Image* hdr_copy_image_data = gpu.get_image( hdr_copy_resource->resource_info.texture.image );

    // If we have different queue families, acquire ownership
    if ( gpu->vulkan_compute_queue_family != gpu->vulkan_main_queue_family ) {
        cb->acquire_image_ownership( hdr_lighting_image,
                                     range_color_full(),
                                     gpu->vulkan_main_queue_family, gpu->vulkan_compute_queue_family,
                                     { VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                       VK_ACCESS_2_TRANSFER_READ_BIT,
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL } );
        cb->flush_barriers();
    }

    cb->copy_image( hdr_lighting_image, hdr_copy_image,
        ImageSyncState{
            .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .access = VK_ACCESS_2_SHADER_READ_BIT,
            .layout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
        }
    );

    // Done with the texture, release the ownership
    if ( gpu->vulkan_compute_queue_family != gpu->vulkan_main_queue_family ) {
        cb->release_image_ownership( hdr_lighting_image, range_color_full(),
                                     gpu->vulkan_main_queue_family );
        cb->flush_barriers();
    }
    //gpu_commands->copy_image( hdr_lighting_resource->resource_info.texture.image,
    //                          hdr_copy_resource->resource_info.texture.image, RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
}

} // namespace raptor