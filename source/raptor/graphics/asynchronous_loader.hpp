#pragma once

#include "foundation/array.hpp"
#include "foundation/platform.hpp"

#include "graphics/command_buffer.hpp"
#include "graphics/gpu_device.hpp"
#include "graphics/gpu_resources.hpp"

#include "foundation/static_string.hpp"

#include <atomic>

namespace raptor
{
    struct Allocator;
    struct FrameGraph;
    struct GpuVisualProfiler;
    struct ImGuiService;
    struct Renderer;
    struct ArenaAllocator;

    // V2 //////////////////////////////////////////////////////////////////////

    struct FileLoadRequestV2 {
        StringPath              path;
        ImageHandle             texture = {};
        BufferHandle            buffer  = {};
    }; // struct FileLoadRequest

    enum class UploadRequestType : u8 {
        TextureCpuToGpu,
        BufferCpuToGpu,
        BufferUpload
    };

    struct UploadRequestV2 {
        UploadRequestType       type;

        // For textures
        ImageHandle             texture;
        const void*             cpu_data;

        // For buffers
        BufferHandle            cpu_buffer;   // "source" buffer on GPU or CPU buffer handle
        BufferHandle            gpu_buffer;   // destination GPU buffer handle

        u32                     size;         // size of data when copying from raw CPU memory
        u32                     offset;
        bool                    keep_src;     // if true, don't destroy the src buffer after upload

    }; // struct UploadRequest

    //
    //
    struct AsynchronousLoader {

        struct InFlighBuffer {
            BufferHandle        source;
            BufferHandle        destination;
            u64                 timeline_value;
            bool                keep_src; // Don't destroy the buffer after upload
        };

        void                    init( Renderer* renderer, Allocator* resident_allocator );
        void                    update();
        void                    shutdown();

        void                    request_texture_load_from_file( cstring filename, ImageHandle texture );
        void                    request_buffer_copy( BufferHandle src, BufferHandle dst, bool keep_src = false );
        void                    request_buffer_upload( BufferHandle destination, const void* data, VkDeviceSize size, VkDeviceSize offset = 0 );

        Renderer*               renderer        = nullptr;

        u64                     last_signaled_transfer_value = 0;
        u64                     last_completed_transfer_value = 0;
        u32                     last_transfer_query_frame = u32_max;

        Array<FileLoadRequestV2> file_load_requests;
        Array<UploadRequestV2>  upload_requests;
        Array<ImageHandle>      inflight_images;
        Array<InFlighBuffer>    inflight_buffers;

        BufferHandle            staging_buffer_handle;
        std::atomic_size_t      staging_buffer_offset;
        u32                     staging_buffer_size;

        VkCommandPool           command_pool;
        CommandBuffer           command_buffer;
        VkSemaphore             transfer_timeline_semaphore;

    }; // struct AsynchronousLoader

} // namespace raptor
