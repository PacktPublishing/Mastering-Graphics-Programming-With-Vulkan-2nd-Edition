#pragma once

#include "graphics/gpu_device.hpp"

namespace raptor {

static const u32 k_secondary_command_buffers_count = 2;

//#define RAPTOR_CB_MARKER_DEBUG

//
//
struct GpuThreadFrameCommandPools {

    VkCommandPool           vulkan_command_pool = nullptr;

}; // struct

//
//
struct CommandBuffer {

    void                    init( GpuDevice* gpu );
    void                    shutdown();

    //
    // Commands interface
    //

    void                    begin();
    void                    end();

    void                    begin_render_pass( Span<const ImageViewHandle> render_targets, Span<const VkAttachmentLoadOp> load_operations,
                                               Span<const VkClearValue> clear_values, ImageViewHandle depth,
                                               VkAttachmentLoadOp depth_load_operation, VkClearValue depth_stencil_clear,
                                               ImageViewHandle shading_rate_attachment = ImageViewHandle(),
                                               u32 layer_count = 1, u32 view_mask = 0 );
    void                    end_render_pass();

    void                    bind_pipeline( PipelineHandle handle );
    void                    bind_vertex_buffer( BufferHandle handle, u32 binding, u32 offset );
    void                    bind_vertex_buffers( BufferHandle* handles, u32 first_binding, u32 binding_count, u32* offsets );
    void                    bind_index_buffer( BufferHandle handle, u32 offset, VkIndexType index_type );

    void                    bind_descriptor_set( Span<const DescriptorSetHandle> handles, Span<const u32> offsets );

    void                    set_fullscreen_viewport();
    void                    set_viewport( const VkViewport& viewport );
    void                    set_fullscreen_scissor();
    void                    set_scissor( const VkRect2D& scissor );
    void                    set_shading_rate( const u32 width, const u32 height );
    void                    set_depth_bias_enabled( bool enabled );
    void                    set_depth_bias( f32 constant_factor, f32 clamp, f32 slope_factor );

    void                    clear( f32 red, f32 green, f32 blue, f32 alpha, u32 attachment_index );
    void                    clear_depth_stencil( f32 depth, u8 stencil );

    void                    push_constants( PipelineHandle pipeline, u32 offset, u32 size, void* data );

    void                    draw( TopologyType::Enum topology, u32 first_vertex, u32 vertex_count, u32 first_instance, u32 instance_count );
    void                    draw_indexed( TopologyType::Enum topology, u32 index_count, u32 instance_count, u32 first_index, i32 vertex_offset, u32 first_instance );
    void                    draw_indirect( BufferHandle handle, u32 draw_count, u32 offset, u32 stride );
    void                    draw_indirect_count( BufferHandle argument_buffer, u32 argument_offset, BufferHandle count_buffer, u32 count_offset, u32 max_draws, u32 stride );
    void                    draw_indexed_indirect( BufferHandle handle, u32 draw_count, u32 offset, u32 stride );

    void                    draw_mesh_task( u32 task_count );
    void                    draw_mesh_task_indirect( BufferHandle argument_buffer, u32 argument_offset, u32 command_count, u32 stride );
    void                    draw_mesh_task_indirect_count( BufferHandle argument_buffer, u32 argument_offset, BufferHandle count_buffer, u32 count_offset, u32 max_draws, u32 stride );

    void                    dispatch( u32 group_x, u32 group_y, u32 group_z );
    void                    dispatch_indirect( BufferHandle handle, u32 offset );

    void                    trace_rays( PipelineHandle pipeline, u32 width, u32 height, u32 depth );

    void                    barrier_instant_compute_write_to_compute_read();
    void                    global_debug_barrier(); // Use only to debug barrier-related problems

    // Barriers
    void                    add_image_barrier( ImageHandle image, VkImageSubresourceRange range,
                                               const ImageSyncState& dst );
    // Used for mip where source state is pointing to mip 0 only
    void                    add_image_barrier( ImageHandle image, VkImageSubresourceRange range,
                                               const ImageSyncState& src, const ImageSyncState& dst );
    void                    add_buffer_barrier( BufferHandle buffer, VkDeviceSize offset, VkDeviceSize size, const BufferSyncState& dst );
    void                    add_memory_barrier( VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                                                VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_acces );

    // Release ownership from the current queue family to dst_queue_family.
    // Records an image barrier with queue family transfer and keeps the current layout.
    void                    release_image_ownership( ImageHandle image, VkImageSubresourceRange range,
                                                     u32 dst_queue_family );

    // Acquire ownership on the current queue from src_queue_family and optionally transition
    // to the desired next use (stage/access/layout). This is the recommended way to consume
    // resources produced on another queue (e.g. transfer -> graphics).
    void                    acquire_image_ownership( ImageHandle image, VkImageSubresourceRange range,
                                                     u32 src_queue_family, u32 dst_queue_family,
                                                     const ImageSyncState& next_use );

    void                    flush_barriers();

    void                    clear_color_image( ImageHandle texture, VkClearColorValue clear_color );
    void                    fill_buffer( BufferHandle buffer, u32 offset, u32 size, u32 data );

    void                    push_marker( const char* name );
    void                    pop_marker();

    u32                     get_subgroup_sized( u32 group );

    // Non-drawing methods
    void                    copy_buffer_to_image( ImageHandle image, BufferHandle buffer, sizet buffer_offset );

    [[deprecated("Use copy_buffer_to_image method")]]
    void                    upload_texture_data( ImageHandle texture, void* texture_data, BufferHandle staging_buffer, sizet staging_buffer_offset );

    void                    copy_image( ImageHandle src, ImageHandle dst, const ImageSyncState& dst_state );
    void                    copy_image( ImageHandle src, ImageSubResource src_sub, ImageHandle dst, ImageSubResource dst_sub, const ImageSyncState& dst_state );
    void                    copy_image_mip0( ImageHandle src, ImageSubResource src_sub, ImageHandle dst, ImageSubResource dst_sub );

    void                    copy_buffer( BufferHandle src, sizet src_offset, BufferHandle dst, sizet dst_offset, sizet size );

    void                    update_buffer( BufferHandle  buffer, sizet offset, sizet size, void* data );
    void                    upload_buffer_data( BufferHandle buffer, void* buffer_data, BufferHandle staging_buffer, sizet staging_buffer_offset );
    void                    upload_buffer_data( BufferHandle src, BufferHandle dst );

    void                    reset();

    static const u32        k_depth_stencil_clear_index = k_max_image_outputs;

    VkCommandBuffer         vk_command_buffer;

    GpuThreadFrameCommandPools* thread_frame_pool = nullptr;
    GpuDevice*              gpu_device          = nullptr;

    Pipeline*               current_pipeline    = nullptr;
    VkClearValue            clear_values[ k_max_image_outputs + 1 ];    // Clear value for each attachment with depth/stencil at the end.
    bool                    is_recording        = false;

    // Render sizes when inside a pass.
    u32                     frame_buffer_width  = 0;
    u32                     frame_buffer_height = 0;
    bool                    inside_pass         = false;

    u32                     handle              = u32_max;

    u32                     current_command     = 0;
    ResourceHandle          resource_handle     = u32_max;

    // Barrier management
    StaticArray<VkMemoryBarrier2, 4>    global_barriers;
    StaticArray<VkBufferMemoryBarrier2, 8> buffer_barriers;
    StaticArray<VkImageMemoryBarrier2, 8> image_barriers;

#if defined (RAPTOR_CB_MARKER_DEBUG)
    StaticArray<cstring, 64> markers;
#endif // RAPTOR_CB_MARKER_DEBUG

    // Caching indices
    u32                     frame_index         = 0;
    u32                     thread_index        = 0;
    u16                     cb_used_index       = 0; // per frame-thread used index
    CommandQueueType        queue_type          = CommandQueueType::Count;

}; // struct CommandBuffer


struct CommandBufferManager {

    void                    init( GpuDevice* gpu, u32 num_threads );
    void                    shutdown();

    void                    reset_pools( u32 frame_index );

    CommandBuffer*          allocate_command_buffer( u32 frame, u32 thread_index, CommandQueueType queue_type );
    CommandBuffer*          get_secondary_command_buffer( u32 frame, u32 thread_index );

    u16                     pool_from_index( u32 index ) { return (u16)index / num_pools_per_frame; }
    u32                     pool_from_indices( u32 frame_index, u32 thread_index, CommandQueueType queue_type );

    Array<GpuThreadFrameCommandPools> thread_frame_command_pools;
    Array<CommandBuffer>    command_buffers;
    Array<CommandBuffer>    secondary_command_buffers;
    Array<u8>               used_buffers;       // Track how many buffers were used per thread per frame.
    Array<u8>               used_secondary_command_buffers;

    GpuDevice*              gpu                     = nullptr;
    u32                     num_pools_per_frame     = 0;
    static const u32        k_num_cbs_per_thread    = 4;

}; // struct CommandBufferManager


} // namespace raptor
