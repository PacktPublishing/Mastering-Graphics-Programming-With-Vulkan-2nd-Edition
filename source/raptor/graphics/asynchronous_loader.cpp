#include "graphics/asynchronous_loader.hpp"
#include "graphics/renderer.hpp"

#include "foundation/time.hpp"

#define STBI_FAILURE_USERMSG
#define STB_IMAGE_IMPLEMENTATION
#include "external/stb_image.h"

#include "external/tracy/tracy/Tracy.hpp"

namespace raptor
{
// AsynchronousLoaderV2 ////////////////////////////////////////////////////
void AsynchronousLoader::init( Renderer* renderer_, Allocator* resident_allocator ) {

    renderer = renderer_;

    file_load_requests.init( resident_allocator, 32 );
    upload_requests.init( resident_allocator, 32 );
    inflight_images.init( resident_allocator, 32 );
    inflight_buffers.init( resident_allocator, 32 );

    // Create a persistently-mapped staging buffer
    staging_buffer_offset = 0;
    staging_buffer_size = rmega( 64 );
    staging_buffer_handle = renderer->gpu->create_buffer( {
        .size = staging_buffer_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | 
                            VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .name = "staging_buffer"});

    // Create command pool and buffer
    {
        VkCommandPoolCreateInfo cmd_pool_info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr };
        cmd_pool_info.queueFamilyIndex = renderer->gpu->vulkan_transfer_queue_family;
        cmd_pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

        vkCreateCommandPool( renderer->gpu->vulkan_device, &cmd_pool_info,
                             renderer->gpu->vulkan_allocation_callbacks, &command_pool );

        VkCommandBufferAllocateInfo cmd = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr };
        cmd.commandPool = command_pool;
        cmd.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd.commandBufferCount = 1;

        command_buffer = {};
        command_buffer.gpu_device = renderer->gpu;
        vkAllocateCommandBuffers( renderer->gpu->vulkan_device, &cmd, &command_buffer.vk_command_buffer );
    }

    // Create timeline semaphore
    VkSemaphoreTypeCreateInfo timeline_info{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    timeline_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timeline_info.initialValue = 0;

    VkSemaphoreCreateInfo semaphore_info{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    semaphore_info.pNext = &timeline_info;
    vkCreateSemaphore( renderer->gpu->vulkan_device, &semaphore_info,
                       renderer->gpu->vulkan_allocation_callbacks, &transfer_timeline_semaphore );

    last_signaled_transfer_value = 0;
    last_completed_transfer_value = 0;
    last_transfer_query_frame = u32_max;
}

void AsynchronousLoader::shutdown() {

    renderer->gpu->destroy_buffer( staging_buffer_handle );

    file_load_requests.shutdown();
    upload_requests.shutdown();
    inflight_images.shutdown();
    inflight_buffers.shutdown();

    // Command buffers are destroyed with the pool associated.
    vkDestroyCommandPool( renderer->gpu->vulkan_device, command_pool,
                          renderer->gpu->vulkan_allocation_callbacks );

    vkDestroySemaphore( renderer->gpu->vulkan_device, transfer_timeline_semaphore,
                        renderer->gpu->vulkan_allocation_callbacks );
}

void AsynchronousLoader::update() {
    ZoneScopedN("AsynchronousLoaderV2::update");

    // Process a limited amount of file load requests per update
    {
        FileLoadRequestV2 load_request;
        const u32 k_max_files_per_update = 4;

        for ( u32 i = 0; i < k_max_files_per_update; ++i ) {
            if ( file_load_requests.size == 0 ) {
                break;
            }

            // Get front load request and delete swap
            load_request = file_load_requests.front();
            file_load_requests.delete_swap( 0 );

            // NOTE: check for image size and see if it fits in staging buffer
            int x, y, comp;
            stbi_info( load_request.path.c_str(), &x, &y, &comp );
            const u32 image_size = u32( x ) * u32( y ) * 4u;

            // Load request is already removed from array
            if ( image_size > staging_buffer_size ) {
                rprint( "Image %s is too large to fit in staging buffer (%llu bytes required, %u bytes available)\n",
                        load_request.path.c_str(), image_size, staging_buffer_size );
                RASSERTM( false, "Image too large to fit in staging buffer" );
                continue;
            }

            i64 start_reading_file = time_now();

            // TODO: add streaming allocator

            u8* texture_data = stbi_load( load_request.path.c_str(), &x, &y, &comp, 4);

            if ( texture_data ) {
                rprint( "File %s read in %f ms\n", load_request.path, time_from_milliseconds( start_reading_file ) );

                UploadRequestV2 upload_request{};
                upload_request.type = UploadRequestType::TextureCpuToGpu;
                upload_request.texture = load_request.texture;
                upload_request.cpu_data = texture_data;
                upload_request.size = image_size;

                upload_requests.push( upload_request );
            } else {
                rprint( "Error reading file %s: %s\n", load_request.path, stbi_failure_reason() );
            }
        }
    }

    // Check if previous batch is still in flight using timeline semaphore.
    // For now we keep only one batch in flight to simplify things.
    
    if ( last_completed_transfer_value < last_signaled_transfer_value ) {

        const u32 current_frame = renderer->gpu->current_frame;

        // The asynchronous loader can update multiple times per frame,
        // but querying the semaphore more than once is unnecessary.
        if ( last_transfer_query_frame == current_frame ) {
            return;
        }

        last_transfer_query_frame = current_frame;

        u64 gpu_counter_value = 0;
        VkResult result = vkGetSemaphoreCounterValue( renderer->gpu->vulkan_device, transfer_timeline_semaphore, &gpu_counter_value );

        if ( result != VK_SUCCESS ) {
            return;
        }

        last_completed_transfer_value = gpu_counter_value;

        if ( last_completed_transfer_value < last_signaled_transfer_value ) {
            // Previous batch is still using the staging buffer.
            return;
        }
    }

    // Go through all the inflight images and mark them as ready
    for ( u32 i = 0; i < inflight_images.size; ++i ) {
        renderer->add_image_to_finalize_upload( inflight_images[ i ] );
    }
    inflight_images.clear();

    for ( u32 i = 0; i < inflight_buffers.size; ++i ) {

        InFlighBuffer& inflight_buffer = inflight_buffers[ i ];

        if ( inflight_buffer.timeline_value > last_completed_transfer_value ) {
            continue;
        }

        Buffer* destination = renderer->gpu->get_buffer( inflight_buffer.destination );
        destination->ready = true;

        if ( !inflight_buffer.keep_src ) {
            renderer->gpu->destroy_buffer( inflight_buffer.source );
        }
    }
    inflight_buffers.clear();

    // At this point we finished processing previous requests, reset offset
    staging_buffer_offset = 0;

    if ( upload_requests.size == 0 ) {
        return;
    }

    // Record copy commands until the staging buffer is full or no more requests
    // TODO: multithreaded support
    vkResetCommandBuffer( command_buffer.vk_command_buffer, 0 );
    command_buffer.begin();

    Buffer* staging_buffer = renderer->gpu->get_buffer( staging_buffer_handle );
    GpuDevice* gpu = renderer->gpu;
    bool staging_buffer_full = false;

    // Submit the commands and signal the new timeline value.
    const u64 signal_value = last_signaled_transfer_value + 1;

    for ( u32 i = 0; i < upload_requests.size; i++ ) {

        UploadRequestV2 request = upload_requests[ i ];
        bool request_processed = false;

        switch ( request.type ) {
            case UploadRequestType::TextureCpuToGpu: {
                // Check if we have enough space in staging buffer
                constexpr u32 k_texture_alignment = 4;

                const sizet current_offset = staging_buffer_offset.load();
                const sizet aligned_offset = memory_align( current_offset, k_texture_alignment );
                const sizet end_offset = aligned_offset + request.size;

                if ( end_offset > staging_buffer_size ) {
                    staging_buffer_full = true;
                    break;
                }
                // Copy buffer_data to staging buffer
                memcpy( staging_buffer->mapped_data + aligned_offset, request.cpu_data, request.size );

                // Add to inflight image array
                inflight_images.push( request.texture );

                // Update offset
                staging_buffer_offset = end_offset;

                request_processed = true;

                // Adjust ownership of image to transfer queue, as it was never set.
                // NOTE: this is the first command written to this texture, update the owner
                gpu->set_initial_image_owner( request.texture, QueueType::CopyTransfer );

                command_buffer.copy_buffer_to_image( request.texture, staging_buffer_handle, aligned_offset );

                // Release ownership to main family from now on.
                if ( gpu->vulkan_transfer_queue_family != gpu->vulkan_main_queue_family ) {
                    command_buffer.release_image_ownership( request.texture, range_color( 0, 1, 0, 1 ),
                                                            gpu->vulkan_main_queue_family );
                    command_buffer.flush_barriers();
                }

                // Free CPU data
                stbi_image_free( (void*)request.cpu_data );

                break;
            }

            case UploadRequestType::BufferCpuToGpu: {
                Buffer* src = gpu->get_buffer( request.cpu_buffer );
                Buffer* dst = gpu->get_buffer( request.gpu_buffer );

                RASSERT( src->size == dst->size );

                command_buffer.copy_buffer( request.cpu_buffer, 0, request.gpu_buffer, 0, src->size );
                inflight_buffers.push( { .source = request.cpu_buffer, .destination = request.gpu_buffer, 
                                         .timeline_value = signal_value, .keep_src = request.keep_src } );

                request_processed = true;

                break;
            }

            case UploadRequestType::BufferUpload:
            {
                Buffer* dst = gpu->get_buffer( request.gpu_buffer );

                RASSERT( dst );
                RASSERT( request.cpu_data );
                RASSERT( request.size > 0 );
                RASSERT( request.offset + request.size <= dst->size );

                constexpr VkDeviceSize k_buffer_copy_alignment = 4;

                const VkDeviceSize current_offset = staging_buffer_offset.load();
                const VkDeviceSize aligned_offset = memory_align( current_offset, k_buffer_copy_alignment );
                const VkDeviceSize end_offset = aligned_offset + request.size;

                if ( request.size > staging_buffer_size ) {
                    rprint( "Buffer upload is too large for staging buffer: %llu bytes required, %u available\n", request.size, staging_buffer_size );

                    RASSERTM( false, "Buffer upload is too large for staging buffer" );

                    // The request must be removed, otherwise it will remain queued forever.
                    //release_upload_data( request );
                    request_processed = true;
                    break;
                }

                if ( end_offset > staging_buffer_size ) {
                    staging_buffer_full = true;
                    break;
                }

                memcpy( staging_buffer->mapped_data + aligned_offset, request.cpu_data, request.size );

                command_buffer.copy_buffer( staging_buffer_handle, aligned_offset,
                                            request.gpu_buffer, request.offset, request.size );

                inflight_buffers.push( {
                    .source = staging_buffer_handle,
                    .destination = request.gpu_buffer,
                    .timeline_value = signal_value,
                    .keep_src = true } );

                staging_buffer_offset = end_offset;

                // The temporary CPU copy is no longer necessary after copying it
                // into the persistent staging buffer.
                //release_upload_data( request );

                request_processed = true;
                break;
            }
        }

        if ( request_processed ) {
            // Remove request from the list
            upload_requests.delete_swap( i );
            // adjust index since we removed current request
            --i;
        }

        if ( staging_buffer_full ) {
            break;
        }
    }

    command_buffer.end();

    if ( inflight_images.size == 0 && inflight_buffers.size == 0 ) {
        // No requests were processed
        return;
    }

    // Perform a flush of the staging buffer to ensure that the data is visible to the GPU before submitting the command buffer.
    const VkDeviceSize staging_bytes_written = staging_buffer_offset.load();

    if ( staging_bytes_written > 0 ) {
        vmaFlushAllocation( gpu->vma_allocator, staging_buffer->vma_allocation, 0, staging_bytes_written );
    }

    VkTimelineSemaphoreSubmitInfo timeline_submit{ VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    timeline_submit.signalSemaphoreValueCount = 1;
    timeline_submit.pSignalSemaphoreValues = &signal_value;

    VkSubmitInfo submit_info{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit_info.pNext = &timeline_submit;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer.vk_command_buffer;
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &transfer_timeline_semaphore;

    vkQueueSubmit( renderer->gpu->vulkan_transfer_queue, 1, &submit_info, VK_NULL_HANDLE );

    last_signaled_transfer_value = signal_value;
}

void AsynchronousLoader::request_texture_load_from_file( cstring filename, ImageHandle texture ) {
    // TODO: add multithreaded safety
    FileLoadRequestV2& request = file_load_requests.push_use();
    request.path.set( filename );
    request.texture = texture;
    request.buffer = {};
}

void AsynchronousLoader::request_buffer_copy( BufferHandle src, BufferHandle dst, bool keep_src ) {

    UploadRequestV2& upload_request = upload_requests.push_use();
    memset( &upload_request, 0, sizeof( upload_request ) );
    upload_request.type = UploadRequestType::BufferCpuToGpu;
    upload_request.cpu_buffer = src;
    upload_request.gpu_buffer = dst;
    upload_request.keep_src = keep_src;

    Buffer* buffer = renderer->gpu->get_buffer( dst );
    buffer->ready = false;
}

void AsynchronousLoader::request_buffer_upload( BufferHandle destination, const void* data, VkDeviceSize size, VkDeviceSize offset ) {
    UploadRequestV2& upload_request = upload_requests.push_use();
    upload_request = {};

    upload_request.type = UploadRequestType::BufferUpload;
    upload_request.gpu_buffer = destination;
    upload_request.cpu_data = data;
    upload_request.size = ( u32 )size;
    upload_request.offset = ( u32 )offset;

    Buffer* buffer = renderer->gpu->get_buffer( destination );
    buffer->ready = false;
}

} // namespace raptor
//
