#include "graphics/command_buffer.hpp"
#include "graphics/gpu_device.hpp"
#include "graphics/gpu_profiler.hpp"

#include "foundation/memory.hpp"
#include "foundation/numerics.hpp"

namespace raptor {

#if defined(RAPTOR_BARRIER_DEBUG)
void debug_dump_barriers( const CommandBuffer& cb );
#endif

void CommandBuffer::reset() {

    is_recording = false;
    current_pipeline = nullptr;
    current_command = 0;

    inside_pass = false;
    frame_buffer_width = 0;
    frame_buffer_height = 0;

    queue_type = CommandQueueType::Count;
    cb_used_index = u16_max;
    frame_index = u32_max;
    thread_index = u32_max;

    global_barriers.clear();
    buffer_barriers.clear();
    image_barriers.clear();
}

static const u32 k_descriptor_sets_pool_size = 4096;

void CommandBuffer::init( GpuDevice* gpu ) {

    gpu_device = gpu;

    // Create Descriptor Pools
    static const u32 k_global_pool_elements = 128;
    VkDescriptorPoolSize pool_sizes[] =
    {
        { VK_DESCRIPTOR_TYPE_SAMPLER, k_global_pool_elements },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, k_global_pool_elements },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, k_global_pool_elements },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, k_global_pool_elements },
        { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, k_global_pool_elements },
        { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, k_global_pool_elements },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, k_global_pool_elements },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, k_global_pool_elements },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, k_global_pool_elements },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, k_global_pool_elements },
        { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, k_global_pool_elements}
    };
    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = k_descriptor_sets_pool_size;
    pool_info.poolSizeCount = ( u32 )ArraySize( pool_sizes );
    pool_info.pPoolSizes = pool_sizes;

    reset();
}

void CommandBuffer::shutdown() {

    is_recording = false;

    reset();

}

void CommandBuffer::begin() {

    if ( !is_recording ) {
        VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        vkBeginCommandBuffer( vk_command_buffer, &beginInfo );

        is_recording = true;

#if defined (RAPTOR_CB_MARKER_DEBUG)
        markers.clear();
#endif
    }
}

void CommandBuffer::end() {

    if ( is_recording ) {

        vkEndCommandBuffer( vk_command_buffer );

        is_recording = false;

#if defined (RAPTOR_CB_MARKER_DEBUG)
        RASSERT( markers.size == 0 );
#endif
    }
}

void CommandBuffer::begin_render_pass( Span<const ImageViewHandle> render_targets,
                                       Span<const VkAttachmentLoadOp> load_operations,
                                       Span<const VkClearValue> clear_values,
                                       ImageViewHandle depth,
                                       VkAttachmentLoadOp depth_load_operation,
                                       VkClearValue depth_stencil_clear,
                                       ImageViewHandle shading_rate_attachment,
                                       u32 layer_count, u32 view_mask ) {

    Array<VkRenderingAttachmentInfoKHR> color_attachments_info;

    ArenaAllocator* temp_allocator = MemoryService::instance()->get_thread_allocator();
    u64 marker = temp_allocator->get_marker();
    color_attachments_info.init( temp_allocator, ( u32 )render_targets.size, ( u32 )render_targets.size );
    memset( color_attachments_info.data, 0, sizeof( VkRenderingAttachmentInfoKHR ) * render_targets.size );

    frame_buffer_width = 0;
    frame_buffer_height = 0;

    inside_pass = true;

    for ( u32 a = 0; a < ( u32 )render_targets.size; ++a ) {
        ImageView* image_view = gpu_device->get_image_view( render_targets[ a ] );
        Image* image = gpu_device->get_image( image_view->parent_image );

        if ( a == 0 ) {
            frame_buffer_width = image->width;
            frame_buffer_height = image->height;
        } else {
            RASSERT( frame_buffer_width == image->width );
            RASSERT( frame_buffer_height == image->height );
        }

        VkAttachmentLoadOp load_op = load_operations[ a ];
        VkRenderingAttachmentInfoKHR& color_attachment_info = color_attachments_info[ a ];
        color_attachment_info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
        color_attachment_info.imageView = image_view->vk_image_view;
        color_attachment_info.imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL_KHR;// gpu_device->synchronization2_extension_present ? VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL_KHR : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment_info.resolveMode = VK_RESOLVE_MODE_NONE;
        color_attachment_info.loadOp = load_operations[ a ];
        color_attachment_info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        if ( load_op == VK_ATTACHMENT_LOAD_OP_CLEAR ) {
            color_attachment_info.clearValue = clear_values[ a ];
            color_attachment_info.clearValue = clear_values[ a ];
            color_attachment_info.clearValue = clear_values[ a ];
            color_attachment_info.clearValue = clear_values[ a ];
        } else {
            color_attachment_info.clearValue = {};
        }
    }

    VkRenderingAttachmentInfoKHR depth_attachment_info{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR };

    if ( depth.is_valid() ) {
        ImageView* image_view = gpu_device->get_image_view( depth );
        Image* image = gpu_device->get_image( image_view->parent_image );

        // Get width and height for depth-only passes
        if ( render_targets.size == 0 ) {
            frame_buffer_width = image->width;
            frame_buffer_height = image->height;
        }
        else {
            RASSERT( frame_buffer_width == image->width );
            RASSERT( frame_buffer_height == image->height );
        }

        VkAttachmentLoadOp load_op = depth_load_operation;
        depth_attachment_info.imageView = image_view->vk_image_view;
        depth_attachment_info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL_KHR;
        depth_attachment_info.resolveMode = VK_RESOLVE_MODE_NONE;
        depth_attachment_info.loadOp = load_op;
        depth_attachment_info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth_attachment_info.clearValue = depth_stencil_clear;
    }

    VkRenderingInfoKHR rendering_info{ VK_STRUCTURE_TYPE_RENDERING_INFO_KHR };
    rendering_info.flags = 0;
    rendering_info.layerCount = layer_count;
    rendering_info.viewMask = view_mask;
    rendering_info.colorAttachmentCount = ( u32 )render_targets.size;
    rendering_info.pColorAttachments = color_attachments_info.data;
    rendering_info.renderArea = { 0,0, frame_buffer_width, frame_buffer_height };
    rendering_info.pDepthAttachment = depth.is_valid() ? &depth_attachment_info : nullptr;
    rendering_info.pStencilAttachment = nullptr;

    VkRenderingFragmentShadingRateAttachmentInfoKHR shading_rate_info{ VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR };
    if ( shading_rate_attachment.is_valid() ) {
        ImageView* image_view = gpu_device->get_image_view( shading_rate_attachment );

        shading_rate_info.imageView = image_view->vk_image_view;
        shading_rate_info.imageLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
        shading_rate_info.shadingRateAttachmentTexelSize = gpu_device->min_fragment_shading_rate_texel_size;

        rendering_info.pNext = ( void* )&shading_rate_info;
    }

    vkCmdBeginRendering( vk_command_buffer, &rendering_info );

    temp_allocator->free_marker( marker );
}

void CommandBuffer::end_render_pass() {

    vkCmdEndRendering( vk_command_buffer );

    inside_pass = false;
    frame_buffer_width = 0;
    frame_buffer_height = 0;
}

void CommandBuffer::bind_pipeline( PipelineHandle handle_ ) {

    Pipeline* pipeline = gpu_device->get_pipeline( handle_ );
    vkCmdBindPipeline( vk_command_buffer, pipeline->vk_bind_point, pipeline->vk_pipeline );

    // Cache pipeline
    current_pipeline = pipeline;
}

void CommandBuffer::bind_vertex_buffer( BufferHandle handle_, u32 binding, u32 offset ) {

    Buffer* buffer = gpu_device->get_buffer( handle_ );
    VkDeviceSize offsets[] = { offset };

    VkBuffer vk_buffer = buffer->vk_buffer;
    
    vkCmdBindVertexBuffers( vk_command_buffer, binding, 1, &vk_buffer, offsets );
}

void CommandBuffer::bind_vertex_buffers( BufferHandle* handles, u32 first_binding, u32 binding_count, u32* offsets_ ) {
    VkBuffer vk_buffers[ 8 ];
    VkDeviceSize offsets[ 8 ];

    for ( u32 i = 0; i < binding_count; ++i ) {
        Buffer* buffer = gpu_device->get_buffer( handles[i] );

        VkBuffer vk_buffer = buffer->vk_buffer;
        offsets[ i ] = offsets_[ i ];
        vk_buffers[ i ] = vk_buffer;
    }

    vkCmdBindVertexBuffers( vk_command_buffer, first_binding, binding_count, vk_buffers, offsets );
}

void CommandBuffer::bind_index_buffer( BufferHandle handle_, u32 offset_, VkIndexType index_type ) {

    Buffer* buffer = gpu_device->get_buffer( handle_ );

    VkBuffer vk_buffer = buffer->vk_buffer;
    VkDeviceSize offset = offset_;

    vkCmdBindIndexBuffer( vk_command_buffer, vk_buffer, offset, index_type );
}

void CommandBuffer::bind_descriptor_set( Span<const DescriptorSetHandle> handles, Span<const u32> offsets ) {
    constexpr u32 k_first_set = 0;

    VkDescriptorSet vk_descriptor_sets[ 4 ];
    for ( u32 i = 0; i < handles.size; ++i ) {
        DescriptorSet* ds = gpu_device->get_descriptor_set( handles[ i ] );
        vk_descriptor_sets[ i ] = ds->vk_descriptor_set;
    }

    vkCmdBindDescriptorSets( vk_command_buffer, current_pipeline->vk_bind_point, current_pipeline->cached_vk_layout, k_first_set,
                             ( u32 )handles.size, vk_descriptor_sets, ( u32 )offsets.size, offsets.data );
}

void CommandBuffer::set_fullscreen_viewport() {
    RASSERTM( inside_pass, "Set fullscreen viewport should be called inside a begin/end pass pair!" );

    VkViewport vk_viewport;
    vk_viewport.x = 0.f;
    vk_viewport.width = frame_buffer_width * 1.f;
    // Invert Y with negative height and proper offset - Vulkan has unique Clipping Y.
    vk_viewport.y = frame_buffer_height * 1.f;
    vk_viewport.height = frame_buffer_height * -1.f;
    vk_viewport.minDepth = 0.0f;
    vk_viewport.maxDepth = 1.0f;

    vkCmdSetViewport( vk_command_buffer, 0, 1, &vk_viewport );
}

void CommandBuffer::set_viewport( const VkViewport& viewport ) {
    RASSERTM( inside_pass, "Set fullscreen viewport should be called inside a begin/end pass pair!" );
    // Copy and modify viewport to invert the Y axis.
    VkViewport vk_viewport = viewport;

    vk_viewport.y = viewport.height - viewport.y;
    vk_viewport.height = -viewport.height;

    vkCmdSetViewport( vk_command_buffer, 0, 1, &vk_viewport);
}

void CommandBuffer::set_fullscreen_scissor() {
    RASSERTM( inside_pass, "Set fullscreen scissor should be called inside a begin/end pass pair!" );

    VkRect2D vk_scissor;
    vk_scissor.offset.x = 0;
    vk_scissor.offset.y = 0;
    vk_scissor.extent.width = frame_buffer_width;
    vk_scissor.extent.height = frame_buffer_height;

    vkCmdSetScissor( vk_command_buffer, 0, 1, &vk_scissor );
}

void CommandBuffer::set_scissor( const VkRect2D& scissor ) {
    RASSERTM( inside_pass, "Set fullscreen scissor should be called inside a begin/end pass pair!" );

    vkCmdSetScissor( vk_command_buffer, 0, 1, &scissor );
}

void CommandBuffer::set_shading_rate( const u32 width, const u32 height ) {
    VkExtent2D extent{
        .width = width,
        .height = height
    };

    VkFragmentShadingRateCombinerOpKHR combiner_ops[2] = {
        VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,
        VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR,
    };

    vkCmdSetFragmentShadingRateKHR( vk_command_buffer, &extent, combiner_ops );
}

void CommandBuffer::set_depth_bias_enabled( bool enabled ) {
    vkCmdSetDepthBiasEnable( vk_command_buffer, enabled ? VK_TRUE : VK_FALSE );
}

void CommandBuffer::set_depth_bias( f32 constant_factor, f32 clamp, f32 slope_factor ) {
    vkCmdSetDepthBias( vk_command_buffer, constant_factor, clamp, slope_factor );
}

void CommandBuffer::clear( f32 red, f32 green, f32 blue, f32 alpha, u32 attachment_index ) {
    clear_values[ attachment_index ].color = { red, green, blue, alpha };
}

void CommandBuffer::clear_depth_stencil( f32 depth, u8 value ) {
    clear_values[ k_depth_stencil_clear_index ].depthStencil.depth = depth;
    clear_values[ k_depth_stencil_clear_index ].depthStencil.stencil = value;
}

void CommandBuffer::push_constants( PipelineHandle pipeline, u32 offset, u32 size, void* data ) {
    Pipeline* pipeline_ = gpu_device->get_pipeline( pipeline );
    vkCmdPushConstants( vk_command_buffer, pipeline_->cached_vk_layout, VK_SHADER_STAGE_ALL, offset, size, data );
}

void CommandBuffer::draw( TopologyType::Enum topology, u32 first_vertex, u32 vertex_count, u32 first_instance, u32 instance_count ) {
    vkCmdDraw( vk_command_buffer, vertex_count, instance_count, first_vertex, first_instance );
}

void CommandBuffer::draw_indexed( TopologyType::Enum topology, u32 index_count, u32 instance_count, u32 first_index, i32 vertex_offset, u32 first_instance ) {
    vkCmdDrawIndexed( vk_command_buffer, index_count, instance_count, first_index, vertex_offset, first_instance );
}

void CommandBuffer::dispatch( u32 group_x, u32 group_y, u32 group_z ) {
    vkCmdDispatch( vk_command_buffer, group_x, group_y, group_z );
}

void CommandBuffer::draw_indirect( BufferHandle buffer_handle, u32 draw_count, u32 offset, u32 stride ) {

    Buffer* buffer = gpu_device->get_buffer( buffer_handle );

    VkBuffer vk_buffer = buffer->vk_buffer;
    VkDeviceSize vk_offset = offset;

    vkCmdDrawIndirect( vk_command_buffer, vk_buffer, vk_offset, draw_count, stride );
}

void CommandBuffer::draw_indirect_count( BufferHandle argument_buffer, u32 argument_offset, BufferHandle count_buffer, u32 count_offset, u32 max_draws, u32 stride ) {
    Buffer* argument_buffer_ = gpu_device->get_buffer( argument_buffer );
    Buffer* count_buffer_ = gpu_device->get_buffer( count_buffer );

    vkCmdDrawIndirectCount( vk_command_buffer, argument_buffer_->vk_buffer, argument_offset, count_buffer_->vk_buffer, count_offset, max_draws, stride );
}

void CommandBuffer::draw_indexed_indirect( BufferHandle buffer_handle, u32 draw_count, u32 offset, u32 stride ) {
    Buffer* buffer = gpu_device->get_buffer( buffer_handle );

    VkBuffer vk_buffer = buffer->vk_buffer;
    VkDeviceSize vk_offset = offset;

    vkCmdDrawIndexedIndirect( vk_command_buffer, vk_buffer, vk_offset, draw_count, stride );
}

void CommandBuffer::draw_mesh_task( u32 task_count ) {

    vkCmdDrawMeshTasksEXT( vk_command_buffer, task_count, 1, 1 );
}

void CommandBuffer::draw_mesh_task_indirect( BufferHandle argument_buffer, u32 argument_offset, u32 command_count, u32 stride ) {
    Buffer* argument_buffer_ = gpu_device->get_buffer( argument_buffer );

    vkCmdDrawMeshTasksIndirectEXT( vk_command_buffer, argument_buffer_->vk_buffer, argument_offset, command_count, stride );
}

void CommandBuffer::draw_mesh_task_indirect_count( BufferHandle argument_buffer, u32 argument_offset, BufferHandle count_buffer, u32 count_offset, u32 max_draws, u32 stride ) {
    Buffer* argument_buffer_ = gpu_device->get_buffer( argument_buffer );
    Buffer* count_buffer_ = gpu_device->get_buffer( count_buffer );

    vkCmdDrawMeshTasksIndirectCountEXT( vk_command_buffer, argument_buffer_->vk_buffer, argument_offset, count_buffer_->vk_buffer, count_offset, max_draws, stride );
}

void CommandBuffer::dispatch_indirect( BufferHandle buffer_handle, u32 offset ) {
    Buffer* buffer = gpu_device->get_buffer( buffer_handle );

    VkBuffer vk_buffer = buffer->vk_buffer;
    VkDeviceSize vk_offset = offset;

    vkCmdDispatchIndirect( vk_command_buffer, vk_buffer, vk_offset );
}

void CommandBuffer::trace_rays( PipelineHandle pipeline_, u32 width, u32 height, u32 depth ) {
    Pipeline* pipeline = gpu_device->get_pipeline( pipeline_ );

    vkCmdTraceRaysKHR( vk_command_buffer, &pipeline->sbt_raygen_region, &pipeline->sbt_miss_region,
                       &pipeline->sbt_hit_region, &pipeline->sbt_callable_region, width, height, depth );
}

//
static const char* stage_flags_to_string( VkPipelineStageFlags2 stage ) {
    switch ( stage ) {
        case VK_PIPELINE_STAGE_2_NONE: return "NONE";
        case VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT: return "COLOR_ATTACHMENT_OUTPUT";
        case VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT: return "COMPUTE_SHADER";
        case VK_PIPELINE_STAGE_2_TRANSFER_BIT: return "TRANSFER";
        case VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT: return "FRAGMENT_SHADER";
        case VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT: return "ALL_GRAPHICS";
        default: return "MULTI / OTHER";
    }
}

static const char* access_flags_to_string( VkAccessFlags2 access ) {
    switch ( access ) {
        case 0: return "NONE";
        case VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT: return "COLOR_ATTACHMENT_WRITE";
        case VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT: return "COLOR_ATTACHMENT_READ";
        case VK_ACCESS_2_SHADER_READ_BIT: return "SHADER_READ";
        case VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT: return "SHADER_STORAGE_WRITE";
        case VK_ACCESS_2_TRANSFER_READ_BIT: return "TRANSFER_READ";
        case VK_ACCESS_2_TRANSFER_WRITE_BIT: return "TRANSFER_WRITE";
        default: return "MULTI / OTHER";
    }
}

static const char* layout_to_string( VkImageLayout layout ) {
    switch ( layout ) {
        case VK_IMAGE_LAYOUT_UNDEFINED: return "UNDEFINED";
        case VK_IMAGE_LAYOUT_GENERAL: return "GENERAL";
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return "COLOR_ATTACHMENT_OPTIMAL";
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return "SHADER_READ_ONLY_OPTIMAL";
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return "TRANSFER_SRC_OPTIMAL";
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return "TRANSFER_DST_OPTIMAL";
        case VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL: return "ATTACHMENT_OPTIMAL";
        case VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL:  return "READ_ONLY_OPTIMAL";
        default: return "UNKNOWN_LAYOUT";
    }
}

//#define RAPTOR_BARRIER_DEBUG

static void debug_print_image_barrier( cstring label, cstring name,
                                       VkCommandBuffer cb,
                                       const VkImageMemoryBarrier2& b,
                                       const ImageSyncState& tracked_state,
                                       VkImage image, u32 frame_index ) {

    rprint(
        "\n=== IMAGE BARRIER [%s] ===\n"
        "Frame: %u, Name: %s\n"
        "Command Buffer: %p\n"
        "VkImage: %p\n"
        "Subresource: aspect=0x%x mip[%u..%u] layer[%u..%u]\n"
        "QUEUE: %u -> %u\n"
        "SRC: stage=%s access=%s layout=%s\n"
        "DST: stage=%s access=%s layout=%s\n"
        "Tracked BEFORE: owner=%u stage=%s access=%s layout=%s\n",
        label,
        frame_index,
        name,
        cb,
        image,
        b.subresourceRange.aspectMask,
        b.subresourceRange.baseMipLevel,
        b.subresourceRange.baseMipLevel + b.subresourceRange.levelCount - 1,
        b.subresourceRange.baseArrayLayer,
        b.subresourceRange.baseArrayLayer + b.subresourceRange.layerCount - 1,
        b.srcQueueFamilyIndex,
        b.dstQueueFamilyIndex,
        stage_flags_to_string( b.srcStageMask ),
        access_flags_to_string( b.srcAccessMask ),
        layout_to_string( b.oldLayout ),
        stage_flags_to_string( b.dstStageMask ),
        access_flags_to_string( b.dstAccessMask ),
        layout_to_string( b.newLayout ),
        tracked_state.owner_queue_family,
        stage_flags_to_string( tracked_state.stage ),
        access_flags_to_string( tracked_state.access ),
        layout_to_string( tracked_state.layout )
    );
}

void CommandBuffer::barrier_instant_compute_write_to_compute_read() {
    VkMemoryBarrier2KHR barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2_KHR };

    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT_KHR;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT_KHR;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT_KHR;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT_KHR;

    VkDependencyInfoKHR dependency_info{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR };
    dependency_info.memoryBarrierCount = 1;
    dependency_info.pMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2( vk_command_buffer, &dependency_info );
}

void CommandBuffer::global_debug_barrier() {

    VkMemoryBarrier2KHR barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2_KHR };

    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT_KHR;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT_KHR | VK_ACCESS_2_MEMORY_WRITE_BIT_KHR;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT_KHR;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT_KHR | VK_ACCESS_2_MEMORY_WRITE_BIT_KHR;

    VkDependencyInfoKHR dependency_info{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR };
    dependency_info.memoryBarrierCount = 1;
    dependency_info.pMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2( vk_command_buffer, &dependency_info );
}

void CommandBuffer::add_image_barrier( ImageHandle image, VkImageSubresourceRange range,
                                       const ImageSyncState& dst ) {

    RASSERT( vk_command_buffer != VK_NULL_HANDLE );

    // Basic subresource sanity.
    RASSERT( range.aspectMask != 0 );
    if ( range.levelCount != VK_REMAINING_MIP_LEVELS ) {
        RASSERT( range.levelCount > 0 );
    }
    if ( range.layerCount != VK_REMAINING_ARRAY_LAYERS ) {
        RASSERT( range.layerCount > 0 );
    }

    // Destination sync must be explicit.
    RASSERT( dst.stage != 0 );

#if defined(RAPTOR_BARRIER_DEBUG)
    // Destination layout should almost never be UNDEFINED.
    RASSERT( dst.layout != VK_IMAGE_LAYOUT_UNDEFINED && "Destination layout must not be VK_IMAGE_LAYOUT_UNDEFINED." );

    // Heuristic checks to catch common mistakes.
    if ( dst.layout == VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL ) {
        RASSERT( ( dst.access & VK_ACCESS_2_SHADER_WRITE_BIT ) == 0 && "READ_ONLY layout with SHADER_WRITE access is suspicious." );
    }

    if ( dst.layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ) {
        RASSERT( ( dst.access & VK_ACCESS_2_TRANSFER_READ_BIT ) == 0 && "TRANSFER_DST layout with TRANSFER_READ access is suspicious." );
    }

    if ( dst.layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ) {
        RASSERT( ( dst.access & VK_ACCESS_2_TRANSFER_WRITE_BIT ) == 0 && "TRANSFER_SRC layout with TRANSFER_WRITE access is suspicious." );
    }

    if ( dst.layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ) {
        // Present is a read-only usage from the presentation engine, but uses VK_ACCESS_2_MEMORY_NONE.
       // RASSERT( ( dst.access & VK_ACCESS_2_MEMORY_READ_BIT ) != 0 && "PRESENT layout should include VK_ACCESS_2_MEMORY_READ_BIT." );
        RASSERT( ( dst.access & VK_ACCESS_2_MEMORY_WRITE_BIT ) == 0 && "PRESENT layout must not include writes." );
    }
#endif

    Image* img = gpu_device->get_image( image );
    RASSERT( img && img->vk_image != VK_NULL_HANDLE );

    ImageSyncState& src = img->sync_state;

    // Queue ownership transfer ONLY when explicitly requested.
    const bool do_ownership_transfer = ( dst.owner_queue_family != VK_QUEUE_FAMILY_IGNORED );

#if defined(RAPTOR_BARRIER_DEBUG)
    if ( do_ownership_transfer ) {
        RASSERT( src.owner_queue_family != VK_QUEUE_FAMILY_IGNORED && "Ownership transfer requested but source owner is IGNORED." );
        RASSERT( dst.owner_queue_family != src.owner_queue_family && "Ownership transfer requested but dst family equals src family (likely a bug/no-op)." );
    }
#endif

    VkImageMemoryBarrier2 vk_b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    vk_b.srcStageMask = src.stage;
    vk_b.srcAccessMask = src.access;
    vk_b.oldLayout = src.layout;

    vk_b.dstStageMask = dst.stage;
    vk_b.dstAccessMask = dst.access;
    vk_b.newLayout = dst.layout;

    if ( do_ownership_transfer ) {
        // If you request an ownership transfer, src must have a valid family (not IGNORED).
        RASSERT( src.owner_queue_family != VK_QUEUE_FAMILY_IGNORED );
        vk_b.srcQueueFamilyIndex = src.owner_queue_family;
        vk_b.dstQueueFamilyIndex = dst.owner_queue_family;
    } else {
        vk_b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vk_b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    }

    vk_b.image = img->vk_image;
    vk_b.subresourceRange = range;

    image_barriers.push( vk_b );

#if defined(RAPTOR_BARRIER_DEBUG)
    debug_print_image_barrier( "IMG BARRIER", img->name ? img->name : "None", vk_command_buffer, vk_b, img->sync_state, img->vk_image, gpu_device->absolute_frame);
#endif

    // Update tracking immediately so subsequent barriers in the same CB chain correctly.
    src.access = dst.access;
    src.layout = dst.layout;
    src.stage = dst.stage;

    if ( do_ownership_transfer ) {
        src.owner_queue_family = dst.owner_queue_family;
    }
}

void CommandBuffer::add_image_barrier( ImageHandle image, VkImageSubresourceRange range,
                                       const ImageSyncState& src, const ImageSyncState& dst ) {

    RASSERT( vk_command_buffer != VK_NULL_HANDLE );
    RASSERT( range.aspectMask != 0 );

    Image* img = gpu_device->get_image( image );
    RASSERT( img && img->vk_image != VK_NULL_HANDLE );

    VkImageMemoryBarrier2 vk_b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };

    vk_b.srcStageMask = src.stage;
    vk_b.srcAccessMask = src.access;
    vk_b.oldLayout = src.layout;
    vk_b.srcQueueFamilyIndex = src.owner_queue_family;

    vk_b.dstStageMask = dst.stage;
    vk_b.dstAccessMask = dst.access;
    vk_b.newLayout = dst.layout;
    vk_b.dstQueueFamilyIndex = dst.owner_queue_family;

    vk_b.image = img->vk_image;
    vk_b.subresourceRange = range;

    image_barriers.push( vk_b );

#if defined(RAPTOR_BARRIER_DEBUG)
    debug_print_image_barrier( "IMG BARRIER", img->name, vk_command_buffer, vk_b, img->sync_state, img->vk_image, gpu_device->absolute_frame );
#endif

    // Optional: keep coarse tracking updated to dst (ok for whole-image state, not per-mip).
    img->sync_state = dst;
    if ( dst.owner_queue_family != VK_QUEUE_FAMILY_IGNORED ) {
        img->sync_state.owner_queue_family = dst.owner_queue_family;
    }
}


void CommandBuffer::add_buffer_barrier( BufferHandle buffer, VkDeviceSize offset, VkDeviceSize size,
                                        const BufferSyncState& dst ) {

    RASSERT( vk_command_buffer != VK_NULL_HANDLE );
    RASSERT( dst.stage != 0 );
    RASSERT( size != 0 );

    Buffer* buf = gpu_device->get_buffer( buffer );
    RASSERT( buf && buf->vk_buffer != VK_NULL_HANDLE );

#if defined(RAPTOR_BARRIER_DEBUG)
    // Validate offset/size against the actual buffer size.
    // VK_WHOLE_SIZE means "from offset to end of buffer".
    RASSERT( offset < buf->size && "Buffer barrier offset is out of bounds." );

    const VkDeviceSize effective_size = ( size == VK_WHOLE_SIZE ) ? ( buf->size - offset ) : size;
    RASSERT( effective_size > 0 && "Buffer barrier effective size must be > 0." );
    RASSERT( ( offset + effective_size ) <= buf->size && "Buffer barrier range exceeds buffer size." );

    // Heuristic: vertex/index reads should be in VERTEX_INPUT stage.
    if ( ( dst.access & ( VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT ) ) != 0 ) {
        RASSERT( ( dst.stage & VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT ) != 0 && "INDEX/VERTEX_ATTRIBUTE access without VERTEX_INPUT stage is suspicious." );
    }
#endif

    BufferSyncState& src = buf->sync_state;
    const bool do_ownership_transfer = false;// ( dst.owner_queue_family != VK_QUEUE_FAMILY_IGNORED );

#if defined(RAPTOR_BARRIER_DEBUG)
    if ( do_ownership_transfer ) {
        RASSERT( src.owner_queue_family != VK_QUEUE_FAMILY_IGNORED && "Ownership transfer requested but source owner is IGNORED." );
        RASSERT( dst.owner_queue_family != src.owner_queue_family && "Ownership transfer requested but dst family equals src family (likely a bug/no-op)." );
    }
#endif

    VkBufferMemoryBarrier2 vk_b{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
    vk_b.srcStageMask = src.stage;
    vk_b.srcAccessMask = src.access;

    vk_b.dstStageMask = dst.stage;
    vk_b.dstAccessMask = dst.access;

    if ( do_ownership_transfer ) {
        vk_b.srcQueueFamilyIndex = src.owner_queue_family;
        vk_b.dstQueueFamilyIndex = dst.owner_queue_family;
    } else {
        vk_b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vk_b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    }

    vk_b.buffer = buf->vk_buffer;
    vk_b.offset = offset;
    vk_b.size = size;

    buffer_barriers.push( vk_b );

    // Update tracking immediately for chaining.
    src.access = dst.access;
    src.stage = dst.stage;

    if ( do_ownership_transfer ) {
        src.owner_queue_family = dst.owner_queue_family;
    }
}

void CommandBuffer::add_memory_barrier( VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                                        VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access ) {
    VkMemoryBarrier2KHR barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2_KHR };

    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;

    global_barriers.push( barrier );
}

void CommandBuffer::release_image_ownership( ImageHandle image, VkImageSubresourceRange range,
                                             u32 dst_queue_family ) {

    RASSERT( vk_command_buffer != VK_NULL_HANDLE );
    RASSERT( range.aspectMask != 0 );
    RASSERT( dst_queue_family != VK_QUEUE_FAMILY_IGNORED );

    Image* img = gpu_device->get_image( image );
    RASSERT( img && img->vk_image != VK_NULL_HANDLE );

    ImageSyncState& current_use = img->sync_state;

#if defined(RAPTOR_BARRIER_DEBUG)
    // A release barrier must know the current owner.
    RASSERT( current_use.owner_queue_family != VK_QUEUE_FAMILY_IGNORED );
    RASSERT( current_use.layout != VK_IMAGE_LAYOUT_UNDEFINED );
#endif

    //rprint( "Release Image fam %u, %u\n", current_use.owner_queue_family, dst_queue_family );

    const u32 src_queue_family = current_use.owner_queue_family;
    RASSERT( src_queue_family != dst_queue_family );

    // Release: keep current layout, no destination scope on this queue.
    // The receiving queue will do an acquire.
    VkImageMemoryBarrier2 vk_b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    vk_b.srcStageMask = current_use.stage;
    vk_b.srcAccessMask = current_use.access;
    vk_b.oldLayout = current_use.layout;

    vk_b.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
    vk_b.dstAccessMask = 0;
    vk_b.newLayout = current_use.layout;

    vk_b.srcQueueFamilyIndex = src_queue_family;
    vk_b.dstQueueFamilyIndex = dst_queue_family;

    vk_b.image = img->vk_image;
    vk_b.subresourceRange = range;

    image_barriers.push( vk_b );

#if defined(RAPTOR_BARRIER_DEBUG)
    debug_print_image_barrier( "RELEASE", img->name, vk_command_buffer, vk_b, current_use, img->vk_image, gpu_device->absolute_frame );
#endif

    // Update tracking: ownership moved, but layout/usage not changed yet.
    current_use.stage = VK_PIPELINE_STAGE_2_NONE;
    current_use.access = 0;
    current_use.owner_queue_family = dst_queue_family;
}

void CommandBuffer::acquire_image_ownership( ImageHandle image, VkImageSubresourceRange range,
                                             u32 src_queue_family, u32 dst_queue_family,
                                             const ImageSyncState& next_use ) {

    RASSERT( vk_command_buffer != VK_NULL_HANDLE );
    RASSERT( range.aspectMask != 0 );
    RASSERT( src_queue_family != VK_QUEUE_FAMILY_IGNORED );
    RASSERT( dst_queue_family != VK_QUEUE_FAMILY_IGNORED );
    RASSERT( next_use.stage != 0 );

#if defined(RAPTOR_BARRIER_DEBUG)
    RASSERT( next_use.layout != VK_IMAGE_LAYOUT_UNDEFINED && "Acquire next_use layout must not be UNDEFINED." );
#endif

    Image* img = gpu_device->get_image( image );
    RASSERT( img && img->vk_image != VK_NULL_HANDLE );

    ImageSyncState& current_use = img->sync_state;

#if defined(RAPTOR_BARRIER_DEBUG)
    RASSERT( current_use.owner_queue_family != VK_QUEUE_FAMILY_IGNORED );
    // If you update ownership during release, it should already match dst_queue_family here.
    RASSERT( current_use.owner_queue_family == dst_queue_family && "Acquire dst_queue_family does not match tracked owner (missing release or tracking mismatch)." );
    RASSERT( current_use.layout != VK_IMAGE_LAYOUT_UNDEFINED && "Acquire oldLayout must not be UNDEFINED." );
#endif

    VkImageMemoryBarrier2 vk_b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };

    // Acquire: no source scope on this queue. The release on the other queue established availability.
    vk_b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    vk_b.srcAccessMask = 0;
    // expected to match the layout at release time
    vk_b.oldLayout = current_use.layout;

    // Destination: the next intended use on this queue.
    vk_b.dstStageMask = next_use.stage;
    vk_b.dstAccessMask = next_use.access;
    vk_b.newLayout = next_use.layout;

    vk_b.srcQueueFamilyIndex = src_queue_family;
    vk_b.dstQueueFamilyIndex = dst_queue_family;

    //rprint( "Acquire Image fam %u, %u\n", src_queue_family, dst_queue_family );

    vk_b.image = img->vk_image;
    vk_b.subresourceRange = range;

    image_barriers.push( vk_b );

#if defined(RAPTOR_BARRIER_DEBUG)
    debug_print_image_barrier( "ACQUIRE", img->name, vk_command_buffer, vk_b, current_use, img->vk_image, gpu_device->absolute_frame );
#endif

    // Update tracking to next use.
    current_use.stage = next_use.stage;
    current_use.access = next_use.access;
    current_use.layout = next_use.layout;
    current_use.owner_queue_family = dst_queue_family;
}


void CommandBuffer::flush_barriers() {
    RASSERT( vk_command_buffer != VK_NULL_HANDLE );

    if ( global_barriers.size == 0 && buffer_barriers.size == 0 && image_barriers.size == 0 ) {
        return;
    }

    VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.dependencyFlags = 0;

    dep.memoryBarrierCount = global_barriers.size;
    dep.pMemoryBarriers = ( global_barriers.size > 0 ) ? global_barriers.data : nullptr;

    dep.bufferMemoryBarrierCount = buffer_barriers.size;
    dep.pBufferMemoryBarriers = ( buffer_barriers.size > 0 ) ? buffer_barriers.data : nullptr;

    dep.imageMemoryBarrierCount = image_barriers.size;
    dep.pImageMemoryBarriers = ( image_barriers.size > 0 ) ? image_barriers.data : nullptr;

    vkCmdPipelineBarrier2( vk_command_buffer, &dep );

    global_barriers.clear();
    buffer_barriers.clear();
    image_barriers.clear();
}

void CommandBuffer::clear_color_image( ImageHandle image, VkClearColorValue clear_color ) {
    Image* vk_image = gpu_device->get_image( image );

    VkImageSubresourceRange range{ };
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseArrayLayer= 0;
    range.layerCount = VK_REMAINING_ARRAY_LAYERS;
    range.baseMipLevel = 0;
    range.levelCount = VK_REMAINING_MIP_LEVELS;

    //ResourceState old_state = vk_image->state;

    //util_add_image_barrier( gpu_device, vk_command_buffer, vk_image, ResourceState::RESOURCE_STATE_COPY_DEST, 0, VK_REMAINING_MIP_LEVELS, false );
    add_image_barrier( image, range, { VK_PIPELINE_STAGE_2_TRANSFER_BIT,  VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL } );
    flush_barriers();
    vkCmdClearColorImage( vk_command_buffer, vk_image->vk_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_color, 1, &range );
}

void CommandBuffer::fill_buffer( BufferHandle buffer, u32 offset, u32 size, u32 data ) {
    Buffer* vk_buffer = gpu_device->get_buffer( buffer );

    vkCmdFillBuffer( vk_command_buffer, vk_buffer->vk_buffer, VkDeviceSize( offset ), size ? VkDeviceSize( size ) : VkDeviceSize( vk_buffer->size ), data);
}

void CommandBuffer::push_marker( const char* name ) {

    if ( !gpu_device->debug_utils_extension_present ) {
        return;
    }

    VkDebugUtilsLabelEXT label = { VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT };
    label.pLabelName = name;
    label.color[ 0 ] = 1.0f;
    label.color[ 1 ] = 1.0f;
    label.color[ 2 ] = 1.0f;
    label.color[ 3 ] = 1.0f;
    vkCmdBeginDebugUtilsLabelEXT( vk_command_buffer, &label );

    gpu_device->gpu_profiler->push_timestamp( this, name );

#if defined (RAPTOR_CB_MARKER_DEBUG)
    markers.push( name );
    rprint( "push %s\n", name );
#endif
}

void CommandBuffer::pop_marker() {

    if ( !gpu_device->debug_utils_extension_present ) {
        return;
    }

    vkCmdEndDebugUtilsLabelEXT( vk_command_buffer );

    gpu_device->gpu_profiler->pop_timestamp( this );

#if defined (RAPTOR_CB_MARKER_DEBUG)
    rprint( "pop %s\n", markers.back() );
    markers.pop();
#endif
}

u32 CommandBuffer::get_subgroup_sized( u32 group ) {

    return raptor::ceilu32( group * 1.f / gpu_device->subgroup_size );
}

void CommandBuffer::copy_buffer_to_image( ImageHandle image_handle, BufferHandle buffer_handle, sizet buffer_offset ) {

    Image* image = gpu_device->get_image( image_handle );
    Buffer* staging_buffer = gpu_device->get_buffer( buffer_handle );
    u32 image_size = image->width * image->height * 4;

    VkBufferImageCopy region = {};
    region.bufferOffset = buffer_offset;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;

    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;

    region.imageOffset = { 0, 0, 0 };
    region.imageExtent = { image->width, image->height, image->depth };

    const VkImageSubresourceRange range = raptor::range_color( 0, 1, 0, 1 );

    // Pre copy memory barrier to perform layout transition
    //util_add_image_barrier( gpu_device, vk_command_buffer, image, RESOURCE_STATE_COPY_DEST, 0, 1, false );
    add_image_barrier( image_handle, range, { VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL } );
    flush_barriers();
    // Copy from the staging buffer to the image
    vkCmdCopyBufferToImage( vk_command_buffer, staging_buffer->vk_buffer, image->vk_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region );

    // NOTE: This is moved outside for correctness.
    // Post copy memory barrier
    /*add_image_barrier( image_handle, range, { VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       gpu_device->vulkan_main_queue_family } );*/

    /*util_add_image_barrier_ext( gpu_device, vk_command_buffer, image, RESOURCE_STATE_COPY_SOURCE,
                                0, 1, 0, 1, false, gpu_device->vulkan_transfer_queue_family, gpu_device->vulkan_main_queue_family,
                                QueueType::CopyTransfer, QueueType::Graphics );*/
}

void CommandBuffer::upload_texture_data( ImageHandle texture_handle, void* texture_data, BufferHandle staging_buffer_handle, sizet staging_buffer_offset ) {

    Image* texture = gpu_device->get_image( texture_handle );
    Buffer* staging_buffer = gpu_device->get_buffer( staging_buffer_handle );
    u32 image_size = texture->width * texture->height * 4;

    // Copy buffer_data to staging buffer
    memcpy( staging_buffer->mapped_data + staging_buffer_offset, texture_data, static_cast< size_t >( image_size ) );

    VkBufferImageCopy region = {};
    region.bufferOffset = staging_buffer_offset;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;

    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;

    region.imageOffset = { 0, 0, 0 };
    region.imageExtent = { texture->width, texture->height, texture->depth };

    const VkImageSubresourceRange range = raptor::range_color( 0, 1, 0, 1 );
    // Pre copy memory barrier to perform layout transition
    //util_add_image_barrier( gpu_device, vk_command_buffer, texture, RESOURCE_STATE_COPY_DEST, 0, 1, false );
    add_image_barrier( texture_handle, range, { VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL } );
    flush_barriers();
    // Copy from the staging buffer to the image
    vkCmdCopyBufferToImage( vk_command_buffer, staging_buffer->vk_buffer, texture->vk_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region );

    // Post copy memory barrier
    add_image_barrier( texture_handle, range, { VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       gpu_device->vulkan_main_queue_family } );
    flush_barriers();
    /*util_add_image_barrier_ext( gpu_device,vk_command_buffer, texture, RESOURCE_STATE_COPY_SOURCE,
                                0, 1, 0, 1, false, gpu_device->vulkan_transfer_queue_family, gpu_device->vulkan_main_queue_family,
                                QueueType::CopyTransfer, QueueType::Graphics );*/
}
void CommandBuffer::copy_image( ImageHandle src_h, ImageHandle dst_h, const ImageSyncState& final_dst ) {

    copy_image( src_h, ImageSubResource{  }, dst_h, ImageSubResource{ }, final_dst );
}

void CommandBuffer::copy_image_mip0( ImageHandle src_h, ImageSubResource src_sub,
                                     ImageHandle dst_h, ImageSubResource dst_sub ) {
    RASSERT( vk_command_buffer != VK_NULL_HANDLE );

    Image* src = gpu_device->get_image( src_h );
    Image* dst = gpu_device->get_image( dst_h );
    RASSERT( src && src->vk_image != VK_NULL_HANDLE );
    RASSERT( dst && dst->vk_image != VK_NULL_HANDLE );

    const bool src_is_depth = TextureFormat::is_depth_only( src->vk_format );
    const bool dst_is_depth = TextureFormat::is_depth_only( dst->vk_format );

    const VkImageAspectFlags src_aspect =
        src_is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;

    const VkImageAspectFlags dst_aspect =
        dst_is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;

    // Subresource ranges for the copy.
    const VkImageSubresourceRange src_range =
        raptor::range_aspect( src_aspect,
                              src_sub.mip_base_level, 1,
                              src_sub.array_base_layer, src_sub.array_layer_count );

    const VkImageSubresourceRange dst_range =
        raptor::range_aspect( dst_aspect,
                              dst_sub.mip_base_level, 1,
                              dst_sub.array_base_layer, dst_sub.array_layer_count );

    // Pre-copy transitions
    ImageSyncState src_to_transfer_src{};
    src_to_transfer_src.stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    src_to_transfer_src.access = VK_ACCESS_2_TRANSFER_READ_BIT;
    src_to_transfer_src.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    ImageSyncState dst_to_transfer_dst{};
    dst_to_transfer_dst.stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    dst_to_transfer_dst.access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    dst_to_transfer_dst.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    add_image_barrier( src_h, src_range, src_to_transfer_src );
    add_image_barrier( dst_h, dst_range, dst_to_transfer_dst );

    flush_barriers();

    // Copy
    VkImageCopy region{};
    region.srcSubresource.aspectMask = src_aspect;
    region.srcSubresource.mipLevel = src_sub.mip_base_level;
    region.srcSubresource.baseArrayLayer = src_sub.array_base_layer;
    region.srcSubresource.layerCount = src_sub.array_layer_count;

    region.dstSubresource.aspectMask = dst_aspect;
    region.dstSubresource.mipLevel = dst_sub.mip_base_level;
    region.dstSubresource.baseArrayLayer = dst_sub.array_base_layer;
    region.dstSubresource.layerCount = dst_sub.array_layer_count;

    region.dstOffset = { 0, 0, 0 };
    region.extent = { src->width, src->height, src->depth };

    vkCmdCopyImage( vk_command_buffer,
                    src->vk_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    dst->vk_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1, &region );
}

void CommandBuffer::copy_image( ImageHandle src_h, ImageSubResource src_sub,
                                ImageHandle dst_h, ImageSubResource dst_sub,
                                const ImageSyncState& final_dst ) {

    copy_image_mip0( src_h, src_sub, dst_h, dst_sub );

    Image* dst = gpu_device->get_image( dst_h );
    RASSERT( dst && dst->vk_image != VK_NULL_HANDLE );
    const bool dst_is_depth = TextureFormat::is_depth_only( dst->vk_format );

    const VkImageAspectFlags aspect =
        dst_is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;

    // Optional mip generation
    if ( dst->mip_level_count > 1 ) {

        // This path only makes sense for color images and when copying to mip 0.
        RASSERT( !dst_is_depth );
        RASSERT( dst_sub.mip_base_level == 0 );
        RASSERT( dst_sub.array_layer_count == 1 );

        // Prepare mip 0 as TRANSFER_SRC
        ImageSyncState to_transfer_src{};
        to_transfer_src.stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        to_transfer_src.access = VK_ACCESS_2_TRANSFER_READ_BIT;
        to_transfer_src.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

        const VkImageSubresourceRange dst_range =
        raptor::range_aspect( aspect,
                              dst_sub.mip_base_level, 1,
                              dst_sub.array_base_layer, dst_sub.array_layer_count );
        add_image_barrier( dst_h, dst_range, to_transfer_src );
        flush_barriers();

        int32_t w = dst->width;
        int32_t h = dst->height;

        for ( int mip_index = 1; mip_index < (int)dst->mip_level_count; ++mip_index ) {

            const VkImageSubresourceRange mip_range =
                raptor::range_aspect( VK_IMAGE_ASPECT_COLOR_BIT,
                                      (u32)mip_index, 1,
                                      dst_sub.array_base_layer, 1 );

            // Transition current mip to TRANSFER_DST
            ImageSyncState to_transfer_dst{};
            to_transfer_dst.stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            to_transfer_dst.access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            to_transfer_dst.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

            add_image_barrier( dst_h, mip_range, to_transfer_dst );
            flush_barriers();

            VkImageBlit blit{};
            blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.srcSubresource.mipLevel = (u32)( mip_index - 1 );
            blit.srcSubresource.baseArrayLayer = dst_sub.array_base_layer;
            blit.srcSubresource.layerCount = 1;

            blit.srcOffsets[ 0 ] = { 0, 0, 0 };
            blit.srcOffsets[ 1 ] = { w, h, 1 };

            w = ( w > 1 ) ? ( w / 2 ) : 1;
            h = ( h > 1 ) ? ( h / 2 ) : 1;

            blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.dstSubresource.mipLevel = (u32)mip_index;
            blit.dstSubresource.baseArrayLayer = dst_sub.array_base_layer;
            blit.dstSubresource.layerCount = 1;

            blit.dstOffsets[ 0 ] = { 0, 0, 0 };
            blit.dstOffsets[ 1 ] = { w, h, 1 };

            vkCmdBlitImage( vk_command_buffer,
                            dst->vk_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            dst->vk_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            1, &blit, VK_FILTER_LINEAR );

            // Prepare current mip as TRANSFER_SRC for next iteration
            ImageSyncState to_transfer_src_next{};
            to_transfer_src_next.stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            to_transfer_src_next.access = VK_ACCESS_2_TRANSFER_READ_BIT;
            to_transfer_src_next.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

            add_image_barrier( dst_h, mip_range, to_transfer_src_next );

            if ( mip_index + 1 < (int)dst->mip_level_count ) {
                flush_barriers();
            }
        }
    }

    // Final transition
    {
        const VkImageSubresourceRange full_range =
            raptor::range_aspect( aspect,
                                  dst_sub.mip_base_level,
                                  dst->mip_level_count - dst_sub.mip_base_level,
                                  dst_sub.array_base_layer,
                                  dst_sub.array_layer_count );

        add_image_barrier( dst_h, full_range, final_dst );
        flush_barriers();
    }
}


void CommandBuffer::copy_buffer( BufferHandle src, sizet src_offset, BufferHandle dst, sizet dst_offset, sizet size ) {
    Buffer* src_buffer = gpu_device->get_buffer( src );
    Buffer* dst_buffer = gpu_device->get_buffer( dst );

    VkBufferCopy copy_region{ };
    copy_region.srcOffset = src_offset;
    copy_region.dstOffset = dst_offset;
    copy_region.size = size;

    vkCmdCopyBuffer( vk_command_buffer, src_buffer->vk_buffer, dst_buffer->vk_buffer, 1, &copy_region );
}

void CommandBuffer::update_buffer( BufferHandle buffer_handle, sizet offset, sizet size, void* data ) {
    Buffer* buffer = gpu_device->get_buffer( buffer_handle );

    vkCmdUpdateBuffer( vk_command_buffer, buffer->vk_buffer, offset, size, data );
}

void CommandBuffer::upload_buffer_data( BufferHandle buffer_handle, void* buffer_data, BufferHandle staging_buffer_handle, sizet staging_buffer_offset ) {

    RASSERT( false );
    //Buffer* buffer = gpu_device->get_buffer( buffer_handle );
    //Buffer* staging_buffer = gpu_device->get_buffer( staging_buffer_handle );
    //u32 copy_size = buffer->size;

    //// Copy buffer_data to staging buffer
    //memcpy( staging_buffer->mapped_data + staging_buffer_offset, buffer_data, static_cast< size_t >( copy_size ) );

    //VkBufferCopy region{};
    //region.srcOffset = staging_buffer_offset;
    //region.dstOffset = 0;
    //region.size = copy_size;

    //vkCmdCopyBuffer( vk_command_buffer, staging_buffer->vk_buffer, buffer->vk_buffer, 1, &region );

    //util_add_buffer_barrier_ext( gpu_device, vk_command_buffer, buffer->vk_buffer, RESOURCE_STATE_COPY_DEST, RESOURCE_STATE_UNDEFINED,
    //                             copy_size, gpu_device->vulkan_transfer_queue_family, gpu_device->vulkan_main_queue_family,
    //                             QueueType::CopyTransfer, QueueType::Graphics );
}

void CommandBuffer::upload_buffer_data( BufferHandle src_, BufferHandle dst_ ) {
    Buffer* src = gpu_device->get_buffer( src_ );
    Buffer* dst = gpu_device->get_buffer( dst_ );

    RASSERT( src->size == dst->size );

    u32 copy_size = src->size;

    VkBufferCopy region{};
    region.srcOffset = 0;
    region.dstOffset = 0;
    region.size = copy_size;

    vkCmdCopyBuffer( vk_command_buffer, src->vk_buffer, dst->vk_buffer, 1, &region );
}

// CommandBufferManager ///////////////////////////////////////////////////
void CommandBufferManager::init( GpuDevice* gpu_, u32 num_threads ) {

    gpu = gpu_;
    num_pools_per_frame = num_threads;

    // Create pools: num frames * num threads;
    const u32 total_pools = num_pools_per_frame * k_max_frames * k_command_queue_count;
    // Init per thread-frame used buffers
    thread_frame_command_pools.init( gpu->allocator, total_pools, total_pools );
    used_buffers.init( gpu->allocator, total_pools, total_pools );
    used_secondary_command_buffers.init( gpu->allocator, total_pools, total_pools );

    for ( u32 i = 0; i < total_pools; i++ ) {
        used_buffers[ i ] = 0;
        used_secondary_command_buffers[ i ] = 0;
    }

    for ( u32 i = 0; i < thread_frame_command_pools.size; ++i ) {
        GpuThreadFrameCommandPools& pool = thread_frame_command_pools[ i ];

        // Create command buffer pool.
        VkCommandPoolCreateInfo cmd_pool_info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr };
        cmd_pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

        CommandQueueType queue_type = (CommandQueueType)( i % k_command_queue_count );
        const u32 family_index = ( queue_type == CommandQueueType::Graphics ) ?
                                 gpu->vulkan_main_queue_family : gpu->vulkan_compute_queue_family;

        cmd_pool_info.queueFamilyIndex = family_index;

        vkCreateCommandPool( gpu->vulkan_device, &cmd_pool_info, gpu->vulkan_allocation_callbacks, &pool.vulkan_command_pool );
    }

    // Create command buffers: pools * buffers per pool
    const u32 total_buffers = total_pools * k_num_cbs_per_thread;
    command_buffers.init( gpu->allocator, total_buffers, total_buffers );

    const u32 total_secondary_buffers = total_pools * k_secondary_command_buffers_count;
    secondary_command_buffers.init( gpu->allocator, total_secondary_buffers );

    for ( u32 i = 0; i < total_buffers; i++ ) {
        VkCommandBufferAllocateInfo cmd = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr };
        // Which pool am I populating ?
        const u32 pool_linear = i / k_num_cbs_per_thread;
        const u32 frame_index = pool_linear / ( num_pools_per_frame * k_command_queue_count );
        const u32 rem0 = pool_linear % ( num_pools_per_frame * k_command_queue_count );
        const u32 thread_index = rem0 / k_command_queue_count;
        const CommandQueueType queue_type = (CommandQueueType)( rem0 % k_command_queue_count );

        const u32 pool_index = pool_from_indices( frame_index, thread_index, queue_type );
        //rprint( "Indices i:%u f:%u t:%u p:%u\n", i, frame_index, thread_index, pool_index );
        cmd.commandPool = thread_frame_command_pools[ pool_index ].vulkan_command_pool;
        cmd.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd.commandBufferCount = 1;

        CommandBuffer& current_command_buffer = command_buffers[ i ];
        vkAllocateCommandBuffers( gpu->vulkan_device, &cmd, &current_command_buffer.vk_command_buffer );

        current_command_buffer.handle = i;
        current_command_buffer.thread_frame_pool = &thread_frame_command_pools[ pool_index ];
        current_command_buffer.init( gpu );
    }

    u32 handle = total_buffers;
    for ( u32 pool_index = 0; pool_index < total_pools; ++pool_index ) {
        VkCommandBufferAllocateInfo cmd = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr };

        cmd.commandPool = thread_frame_command_pools[ pool_index ].vulkan_command_pool;
        cmd.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
        cmd.commandBufferCount = k_secondary_command_buffers_count;

        VkCommandBuffer secondary_buffers[ k_secondary_command_buffers_count ];
        vkAllocateCommandBuffers( gpu->vulkan_device, &cmd, secondary_buffers );

        for ( u32 scb_index = 0; scb_index < k_secondary_command_buffers_count; ++scb_index ) {
            CommandBuffer cb{ };
            cb.vk_command_buffer = secondary_buffers[ scb_index ];

            cb.handle = handle++;
            cb.thread_frame_pool = &thread_frame_command_pools[ pool_index ];
            cb.init( gpu );

            // NOTE(marco): access to the descriptor pool has to be synchronized
            // across theads. Don't allow for now
            secondary_command_buffers.push( cb );
        }
    }

    //rprint( "Done\n" );
}

void CommandBufferManager::shutdown() {

    for ( u32 i = 0; i < thread_frame_command_pools.size; ++i ) {
        GpuThreadFrameCommandPools& pool = thread_frame_command_pools[ i ];
        vkDestroyCommandPool( gpu->vulkan_device, pool.vulkan_command_pool, gpu->vulkan_allocation_callbacks );
    }

    for ( u32 i = 0; i < command_buffers.size; i++ ) {
        command_buffers[ i ].shutdown();
    }

    for ( u32 i = 0; i < secondary_command_buffers.size; ++i ) {
        secondary_command_buffers[ i ].shutdown();
    }

    thread_frame_command_pools.shutdown();
    command_buffers.shutdown();
    secondary_command_buffers.shutdown();
    used_buffers.shutdown();
    used_secondary_command_buffers.shutdown();
}

void CommandBufferManager::reset_pools( u32 frame_index ) {

    for ( u32 i = 0; i < num_pools_per_frame; i++ ) {

        for ( u32 q = 0; q < k_command_queue_count; ++q ) {
            const u32 pool_index = pool_from_indices( frame_index, i, (CommandQueueType)q );
            vkResetCommandPool( gpu->vulkan_device, thread_frame_command_pools[ pool_index ].vulkan_command_pool, 0 );
            used_buffers[ pool_index ] = 0;
            used_secondary_command_buffers[ pool_index ] = 0;
        }
    }
}

CommandBuffer* CommandBufferManager::allocate_command_buffer( u32 frame, u32 thread_index, CommandQueueType queue_type ) {
    const u32 pool_index = pool_from_indices( frame, thread_index, queue_type );
    u32 current_used_buffer = used_buffers[ pool_index ]++;
    RASSERT( current_used_buffer < k_num_cbs_per_thread );

    CommandBuffer* cb = &command_buffers[ ( pool_index * k_num_cbs_per_thread ) + current_used_buffer ];

    cb->reset();
    cb->frame_index = frame;
    cb->thread_index = thread_index;
    cb->queue_type = queue_type;
    cb->cb_used_index = current_used_buffer;

    return cb;
}

CommandBuffer* CommandBufferManager::get_secondary_command_buffer( u32 frame, u32 thread_index ) {
    /*const u32 pool_index = pool_from_indices( frame, thread_index );
    u32 current_used_buffer = used_secondary_command_buffers[ pool_index ];
    used_secondary_command_buffers[ pool_index ] = current_used_buffer + 1;

    RASSERT( current_used_buffer < k_secondary_command_buffers_count );

    CommandBuffer* cb = &secondary_command_buffers[ ( pool_index * k_secondary_command_buffers_count ) + current_used_buffer ];
    cb->frame_index = frame;
    cb->thread_index = thread_index;

    return cb;*/
    RASSERT( false );
    return nullptr;
}

u32 CommandBufferManager::pool_from_indices( u32 frame_index, u32 thread_index, CommandQueueType queue_type ) {
    return ((frame_index * num_pools_per_frame) + thread_index) * k_command_queue_count + (u32)queue_type;
}

} // namespace raptor
