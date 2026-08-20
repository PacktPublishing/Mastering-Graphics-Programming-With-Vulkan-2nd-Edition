#pragma once

#if defined(_MSC_VER)

#include "foundation/windows_declarations.h"

#endif // _MSC_VER

#include "external/volk.h"

VK_DEFINE_HANDLE( VmaAllocator )

#include "graphics/gpu_resources.hpp"

#include "foundation/data_structures.hpp"
#include "foundation/pool.hpp"
#include "foundation/string.hpp"
#include "foundation/service.hpp"
#include "foundation/array.hpp"

namespace raptor {

struct Allocator;

// Forward-declarations //////////////////////////////////////////////////
struct CommandBuffer;
struct CommandBufferManager;
struct DeviceRenderFrame;
struct GpuProfiler;
struct GpuDevice;
struct GPUTimeQuery;
struct GpuTimeQueryTree;
struct GpuPipelineStatistics;

//
//
struct GpuDescriptorPoolCreation {

    u16         samplers                = 256;
    u16         combined_image_samplers = 256;
    u16         sampled_image           = 256;
    u16         storage_image           = 256;
    u16         uniform_texel_buffers   = 256;
    u16         storage_texel_buffers   = 256;
    u16         uniform_buffer          = 256;
    u16         storage_buffer          = 256;
    u16         uniform_buffer_dynamic  = 256;
    u16         storage_buffer_dynamic  = 256;
    u16         input_attachments       = 256;
    u16         tlas                    = 8;

}; // struct GpuDescriptorPoolCreation

//
//
struct GpuResourcePoolCreation {

    u16         buffers                 = 256;
    u16         images                  = 256;
    u16         image_views             = 256;
    u16         pipelines               = 256;
    u16         samplers                = 256;
    u16         descriptor_set_layouts  = 256;
    u16         descriptor_sets         = 256;
    u16         command_buffers         = 256;
    u16         shaders                 = 256;
    u16         page_pools              = 64;
    u16         pipeline_layouts        = 256;
    u16         blases                  = 128;
    u16         tlases                  = 8;
}; // struct GpuResourcePoolCreation

//
//
struct VulkanDebugOptions {

    bool        enable_validation_layers   = false;    // VK_LAYER_KHRONOS_validation
    bool        enable_debug_utils         = false;    // VK_EXT_debug_utils + debug messenger
    bool        enable_sync_validation     = false;    // VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
    bool        enable_gpu_assisted        = false;    // VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT
    bool        enable_best_practices      = false;    // VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT

    bool        break_on_validation_error  = true;
    bool        break_on_validation_warning = false;

    void        set_none();
    void        set_default();              // Debug utiles e marker, no validation
    void        set_validation();           // Core validation + syncrhonization + best practices.
    void        set_gpu_assisted();         // GPU-AV without core checks, synch and best practices.

}; // struct VulkanDebugOptions

//
//
struct GpuDeviceCreation {

    GpuDescriptorPoolCreation       descriptor_pool_creation;
    GpuResourcePoolCreation         resource_pool_creation;
    VulkanDebugOptions              debug_options;

    Allocator*                      allocator       = nullptr;
    void*                           window          = nullptr; // Pointer to API-specific window: SDL_Window, GLFWWindow
    u16                             width           = 1;
    u16                             height          = 1;

    u16                             gpu_time_queries_per_frame  = 48;
    u16                             num_threads                 = 1;

    u32                             enable_gpu_time_queries     : 1 = 0;
    u32                             enable_pipeline_statistics  : 1 = 1;
    u32                             debug                       : 1 = 0;
    u32                             enable_bindless             : 1 = 0;
    u32                             enable_vrs                  : 1 = 0;
    u32                             enable_ray_tracing          : 1 = 0;
    u32                             enable_pipeline_binary      : 1 = 0;
    u32                             force_disable_mesh_shaders  : 1 = 0;
    u32                             enable_unified_layouts      : 1 = 0;
    u32                             prefer_dedicated_compute_family : 1 = 0;
    u32                             reserved                    : 23 = 0;

    GpuDeviceCreation&              set_window( u32 width, u32 height, void* handle );
    GpuDeviceCreation&              set_allocator( Allocator* allocator );
    GpuDeviceCreation&              set_num_threads( u32 value );

}; // struct GpuDeviceCreation

namespace GlobalSamplers {
    enum Enum {
        LinearClamp = 0,
        LinearRepeat,
        NearestClamp,
        NearestRepeat,
        Count
    }; // enum Enum
} // namespace GlobalSamplers

//
//
struct GpuSubmitSync {
    StaticArray<VkSemaphoreSubmitInfoKHR, 8> waits;
    StaticArray<VkSemaphoreSubmitInfoKHR, 8> signals;
}; // struct GpuSubmitSync

//
//
struct GpuDevice : public Service {

    // Init/Shutdown methods
    void                            init( const GpuDeviceCreation& creation );
    void                            shutdown();

    // Creation/Destruction of resources /////////////////////////////////
    BufferHandle                    create_buffer( const BufferCreation& creation );
    ImageHandle                     create_image( const ImageCreation& creation );
    ImageViewHandle                 create_image_view( const ImageViewCreation& creation );
    PipelineHandle                  create_pipeline( const PipelineCreation& creation, const char* cache_path = nullptr );
    SamplerHandle                   create_sampler( const SamplerCreation& creation );
    DescriptorSetLayoutHandle       create_descriptor_set_layout( const DescriptorSetLayoutCreation& creation );
    DescriptorSetHandle             create_descriptor_set( const DescriptorSetCreation& creation );
    ShaderStateHandle               create_shader_state( const ShaderStateCreation& creation );
    PipelineLayoutHandle            create_pipeline_layout( const PipelineLayoutCreation& creation );
    BLASHandle                      create_blas( const BLASCreation& creation );
    TLASHandle                      create_tlas( const TLASCreation& creation );

    void                            generate_pipeline_binaries( void* pipeline_info, Pipeline* pipeline, cstring cache_path, ArenaScope& temp_stack );
    bool                            restore_pipeline_binaries( cstring cache_path, ArenaScope& temp_stack, Array<VkPipelineBinaryKHR>& pipeline_binaries, VkPipelineBinaryInfoKHR& pipeline_binary_create_info );

    void                            destroy_buffer( BufferHandle buffer );
    void                            destroy_image( ImageHandle image );
    void                            destroy_image_view( ImageViewHandle view );
    void                            destroy_pipeline( PipelineHandle pipeline );
    void                            destroy_sampler( SamplerHandle sampler );
    void                            destroy_descriptor_set_layout( DescriptorSetLayoutHandle layout );
    void                            destroy_descriptor_set( DescriptorSetHandle set );
    void                            destroy_shader_state( ShaderStateHandle shader );
    void                            destroy_pipeline_layout( PipelineLayoutHandle layout );
    void                            destroy_blas( BLASHandle blas );
    void                            destroy_tlas( TLASHandle tlas );

    // Update/Reload resources ///////////////////////////////////////////
    void                            add_image_view_to_bindless( ImageViewHandle image_view );
    void                            remove_image_view_from_bindless( ImageViewHandle image_view );
    void                            add_buffer_to_bindless( BufferHandle buffer );
    void                            remove_buffer_from_bindless( BufferHandle buffer );

    void                            resize_buffer( BufferHandle buffer, u32 size );
    void                            resize_image( ImageHandle image, u32 width, u32 height );
    void                            resize_image( ImageHandle image, u32 width, u32 height, u32 mip_levels );
    void                            resize_image_3d( ImageHandle image, u32 width, u32 height, u32 depth );
    void                            resize_image_3d( ImageHandle image, u32 width, u32 height, u32 depth, u32 mip_levels );
    void                            recreate_image_view( ImageViewHandle image_view );

    PagePoolHandle                  allocate_image_pool( ImageHandle image_handle, u32 pool_size );
    void                            destroy_page_pool( PagePoolHandle pool_handle );

    void                            reset_pool( PagePoolHandle pool_handle );
    void                            bind_image_pages( PagePoolHandle pool_handle, ImageHandle handle, u32 x, u32 y, u32 width, u32 height, u32 layer );

    void                            update_descriptor_set( DescriptorSetHandle set );

    // Misc //////////////////////////////////////////////////////////////
    void                            link_image_sampler( ImageHandle image, SamplerHandle sampler );   // TODO: for now specify a sampler for an image or use the default one.
    void                            set_initial_image_owner( ImageHandle image, QueueType::Enum queue_type ); // Used to change initial ownership for intra-queue images

    void                            set_present_mode( PresentMode::Enum mode );

    void                            frame_counters_advance();

    VkDeviceAddress                 get_buffer_device_address( BufferHandle handle );

    // Swapchain //////////////////////////////////////////////////////////
    void                            create_swapchain();
    void                            destroy_swapchain();
    void                            resize_swapchain();

    // Map/Unmap /////////////////////////////////////////////////////////
    void*                           map_buffer( const MapBufferParameters& parameters );
    void                            unmap_buffer( const MapBufferParameters& parameters );
    void                            flush_buffer( BufferHandle buffer, u32 offset, u32 size );

    void*                           dynamic_allocate( u32 size );

    void*                           dynamic_buffer_allocate( u32 size, u32 alignment, u32* dynamic_offset );

    template<typename T>
    T*                              dynamic_buffer_allocate( u32* dynamic_offset );


    // Command Buffers ///////////////////////////////////////////////////
    CommandBuffer*                  allocate_command_buffer( u32 thread_index, u32 frame_index, CommandQueueType queue_type );
    CommandBuffer*                  get_secondary_command_buffer( u32 thread_index, u32 frame_index );

    // Rendering /////////////////////////////////////////////////////////
    void                            wait_for_previous_frame();
    VkResult                        acquire_next_swapchain_image();
    void                            update_descriptors();
    void                            reset_pools();

    void                            present();
    void                            queue_submit( CommandQueueType queue_type, Span<CommandBuffer*> cbs,
                                                  Span<VkSemaphoreSubmitInfoKHR> waits, Span<VkSemaphoreSubmitInfoKHR> signals );
    void                            update_bindless_resources();
    bool                            update_sparse_resources();  // Returns true if need to wait on bind semaphore
    CommandBuffer*                  flush_acceleration_structure_builds();

    void                            resolve_timestamps();
    void                            process_pending_resource_deletion();
    void                            resize( u16 width, u16 height );

    //void                            fill_barrier( FramebufferHandle render_pass, ExecutionBarrier& out_barrier );

    bool                            buffer_ready( BufferHandle buffer );

    BufferHandle                    get_fullscreen_vertex_buffer() const;           // Returns a vertex buffer usable for fullscreen shaders that uses no vertices.
    ImageHandle                     get_current_swapchain_image() const;
    ImageViewHandle                 get_current_swapchain_image_view() const;
    ImageHandle                     get_current_swapchain_depth_image() const;
    ImageViewHandle                 get_current_swapchain_depth_image_view() const;

    BufferHandle                    get_dummy_constant_buffer() const;
    const RenderPassOutput&         get_swapchain_output() const                    { return swapchain_output; }

    VkSemaphore                     get_current_image_acquired_semaphore() const { return vulkan_image_acquired_binary_semaphore[ current_frame ]; }
    VkSemaphore                     get_current_render_complete_semaphore() const { return vulkan_render_complete_binary_semaphore[ vulkan_image_index ]; }

    // Helpers ////////////////////////////////////////////////////////////
    u64                             compute_frame_limiter_wait_value() const;

    GpuSubmitSync                   build_present_sync( VkPipelineStageFlags2 acquired_stage,
                                                       VkPipelineStageFlags2 present_stage ) const;
    GpuSubmitSync                   build_frame_limiter_sync() const;
    GpuSubmitSync                   build_compute_sync( u64 compute_signal_value, VkPipelineStageFlags2 signal_stage ) const;

    void                            dump_device_fault();

    // Fill a VkSemaphoreSubmitInfoKHR structure.
    static inline VkSemaphoreSubmitInfoKHR build_semaphore_submit( VkSemaphore semaphore, u64 value, VkPipelineStageFlags2 stage_mask, uint32_t device_index = 0 ) {
        VkSemaphoreSubmitInfoKHR info{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR };
        info.semaphore = semaphore;
        info.value = value;
        info.stageMask = stage_mask;
        info.deviceIndex = device_index;
        return info;
    }

    // Compute ///////////////////////////////////////////////////////////
    //void                            submit_compute_load( CommandBuffer* command_buffer );
    //void                            submit_immediate( CommandBuffer* command_buffer );

    // Names /////////////////////////////////////////////////////////////
    void                            set_resource_name( VkObjectType object_type, u64 handle, const char* name );

    // GPU Timings ///////////////////////////////////////////////////////
    void                            set_gpu_timestamps_enable( bool value )         { timestamps_enabled = value; }

    // Instant methods ///////////////////////////////////////////////////
    void                            destroy_buffer_instant( ResourceHandle buffer );
    void                            destroy_image_instant( ResourceHandle image );
    void                            destroy_image_view_instant( ResourceHandle view );
    void                            destroy_pipeline_instant( ResourceHandle pipeline );
    void                            destroy_sampler_instant( ResourceHandle sampler );
    void                            destroy_descriptor_set_layout_instant( ResourceHandle layout );
    void                            destroy_descriptor_set_instant( ResourceHandle set );
    void                            destroy_shader_state_instant( ResourceHandle shader );
    void                            destroy_page_pool_instant( ResourceHandle handle );
    void                            destroy_pipeline_layout_instant( ResourceHandle handle );
    void                            destroy_blas_instant( ResourceHandle handle );
    void                            destroy_tlas_instant( ResourceHandle handle );

    void                            update_descriptor_set_instant( const DescriptorSetUpdate& update );

    // Memory Statistics //////////////////////////////////////////////////
    cstring                         get_gpu_name() const                { return vulkan_physical_properties.deviceName; }
    u32                             get_memory_heap_count();

    Pool<Buffer, BufferHandle>      buffers_pool;
    Pool<DescriptorSet, DescriptorSetHandle> descriptor_sets_pool;
    Pool<DescriptorSetLayout, DescriptorSetLayoutHandle> descriptor_set_layouts_pool;
    Pool<Image, ImageHandle>        images;
    Pool<ImageView, ImageViewHandle> image_views;
    Pool<PagePool, PagePoolHandle>  page_pools_pool;
    Pool<Pipeline, PipelineHandle>  pipelines;
    Pool<PipelineLayout, PipelineLayoutHandle> pipeline_layouts;
    Pool<Sampler, SamplerHandle>    samplers;
    Pool<ShaderState, ShaderStateHandle> shader_states;
    Pool<BLAS, BLASHandle>          blases;
    Pool<TLAS, TLASHandle>          tlases;

    // Primitive resources
    BufferHandle                    fullscreen_vertex_buffer;
    SamplerHandle                   global_samplers[ GlobalSamplers::Count ];
    // Dummy resources
    ImageHandle                     dummy_image;
    ImageViewHandle                 dummy_image_view;
    BufferHandle                    dummy_constant_buffer;
    BufferHandle                    dummy_storage_buffer;
    BufferHandle                    dummy_vertex_buffer;

    RenderPassOutput                swapchain_output;

    StringBuffer                    string_buffer;

    Allocator*                      allocator;

    u32                             dynamic_max_per_frame_size;
    BufferHandle                    dynamic_buffer;
    u8*                             dynamic_mapped_memory;
    u32                             dynamic_allocated_size;
    u32                             dynamic_per_frame_size;

    PresentMode::Enum               present_mode                        = PresentMode::VSync;
    u32                             current_frame;
    u32                             previous_frame;

    u64                             absolute_frame;

    u16                             swapchain_width                     = 1;
    u16                             swapchain_height                    = 1;

    GpuProfiler*                    gpu_profiler                        = nullptr;

    bool                            bindless_supported                  = false;
    bool                            timestamps_enabled                  = false;
    bool                            resized                             = false;
    bool                            vertical_sync                       = false;

    bool                            device_fault_extension_present      = false;
    bool                            device_fault_enabled                = false;
    bool                            device_lost                         = false;

    static constexpr cstring        k_name                              = "raptor_gpu_service";


    VkAllocationCallbacks*          vulkan_allocation_callbacks;
    VkInstance                      vulkan_instance;
    VkPhysicalDevice                vulkan_physical_device;
    VkPhysicalDeviceProperties      vulkan_physical_properties;
    VkPhysicalDevicePipelineBinaryPropertiesKHR vulkan_pipeline_binary_properties;
    VkPipelineBinaryKeyKHR          global_binary_key{ };
    VkDevice                        vulkan_device;
    VkQueue                         vulkan_main_queue;
    VkQueue                         vulkan_compute_queue;
    VkQueue                         vulkan_transfer_queue;
    u32                             vulkan_main_queue_family;
    u32                             vulkan_compute_queue_family;
    u32                             vulkan_transfer_queue_family;
    VkDescriptorPool                vulkan_descriptor_pool;

    // [TAG: BINDLESS]
    VkDescriptorPool                vulkan_bindless_descriptor_pool;
    VkDescriptorSet                 vulkan_bindless_descriptor_set_cached;  // Cached but will be removed with its associated DescriptorSet.
    DescriptorSetLayoutHandle       bindless_descriptor_set_layout;
    DescriptorSetHandle             bindless_descriptor_set;

    // Swapchain
    ImageHandle                     vulkan_swapchain_images[ k_max_swapchain_images ];
    ImageViewHandle                 vulkan_swapchain_image_views[ k_max_swapchain_images ];
    ImageHandle                     vulkan_swapchain_depth_images[ k_max_swapchain_images ];
    ImageViewHandle                 vulkan_swapchain_depth_image_views[ k_max_swapchain_images ];

    // Per frame synchronization
    VkSemaphore                     vulkan_render_complete_binary_semaphore[ k_max_swapchain_images ];
    VkSemaphore                     vulkan_image_acquired_binary_semaphore[ k_max_frames ];
    VkSemaphore                     vulkan_graphics_timeline_semaphore;

    VkSemaphore                     vulkan_bind_binary_semaphore;

    VkSemaphore                     vulkan_compute_timeline_semaphore;
    u64                             last_compute_semaphore_value = 0;
    bool                            has_async_work = false;

    VkFence                         vulkan_immediate_fence;

    // Windows specific
    VkSurfaceKHR                    vulkan_window_surface;
    VkSurfaceFormatKHR              vulkan_surface_format;
    VkPresentModeKHR                vulkan_present_mode;
    VkSwapchainKHR                  vulkan_swapchain;
    u32                             vulkan_swapchain_image_count;

    VulkanDebugOptions              debug_options;
    VkDebugReportCallbackEXT        vulkan_debug_callback;
    VkDebugUtilsMessengerEXT        vulkan_debug_utils_messenger;

    u32                             vulkan_image_index;

    VmaAllocator                    vma_allocator;

    Array<VkPhysicalDeviceFragmentShadingRateKHR> fragment_shading_rates;

    // These are dynamic - so that workload can be handled correctly.
    Array<ResourceUpdate>           resource_deletion_queue;
    Array<DescriptorSetUpdate>      descriptor_set_updates;
    // [TAG: BINDLESS]
    Array<ImageViewUpdate>          image_views_to_update_bindless;
    Array<BufferUpdate>             buffers_to_update_bindless;
    Array<BLASBuildInfo>            blas_build_requests;
    Array<TLASBuildInfo>            tlas_build_requests;

    Array<SparseMemoryBindInfo>     pending_sparse_memory_info;
    Array<VkSparseImageMemoryBind>  pending_sparse_queue_binds;

    u32                             num_threads = 1;
    f32                             gpu_timestamp_frequency;

    u32                             debug_utils_extension_present : 1        = false;
    u32                             mesh_shaders_extension_present : 1       = false;
    u32                             fragment_shading_rate_present : 1        = false;
    u32                             ray_tracing_present : 1                  = false;
    u32                             ray_query_present : 1                    = false;
    u32                             unified_layout_present : 1               = false;
    u32                             pipeline_binary_present : 1              = false;
    u32                             feature_flags_padding : 25               = false;

    sizet                           ubo_alignment                   = 256;
    sizet                           ssbo_alignemnt                  = 256;
    u32                             subgroup_size                   = 32;
    u32                             max_framebuffer_layers          = 1;
    VkExtent2D                      min_fragment_shading_rate_texel_size;
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR   ray_tracing_pipeline_features;
    VkPhysicalDeviceRayQueryFeaturesKHR             ray_query_features;
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR ray_tracing_pipeline_properties;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration_structure_features;

    ShaderState*                    get_shader_state( ShaderStateHandle shader );
    const ShaderState*              get_shader_state( ShaderStateHandle shader ) const;

    Image*                          get_image( ImageHandle texture );
    const Image*                    get_image( ImageHandle texture ) const;

    ImageView*                      get_image_view( ImageViewHandle view );
    const ImageView*                get_image_view( ImageViewHandle view ) const;

    Buffer*                         get_buffer( BufferHandle buffer );
    const Buffer*                   get_buffer( BufferHandle buffer ) const;

    Pipeline*                       get_pipeline( PipelineHandle pipeline );
    const Pipeline*                 get_pipeline( PipelineHandle pipeline ) const;

    Sampler*                        get_sampler( SamplerHandle sampler );
    const Sampler*                  get_sampler( SamplerHandle sampler ) const;

    DescriptorSetLayout*            get_descriptor_set_layout( DescriptorSetLayoutHandle layout );
    const DescriptorSetLayout*      get_descriptor_set_layout( DescriptorSetLayoutHandle layout ) const;

    DescriptorSetLayoutHandle       get_descriptor_set_layout( PipelineHandle pipeline_handle, int layout_index );
    DescriptorSetLayoutHandle       get_descriptor_set_layout( PipelineHandle pipeline_handle, int layout_index ) const;

    DescriptorSet*                  get_descriptor_set( DescriptorSetHandle set );
    const DescriptorSet*            get_descriptor_set( DescriptorSetHandle set ) const;

    PipelineLayout*                 get_pipeline_layout( PipelineLayoutHandle layout );
    const PipelineLayout*           get_pipeline_layout( PipelineLayoutHandle layout ) const;

    PagePool*                       get_page_pool( PagePoolHandle page_pool );
    const PagePool*                 get_page_pool( PagePoolHandle page_pool ) const;

    BLAS*                           get_blas( BLASHandle blas );
    const BLAS*                     get_blas( BLASHandle blas ) const;

    TLAS*                           get_tlas( TLASHandle tlas );
    const TLAS*                     get_tlas( TLASHandle tlas ) const;

}; // struct GpuDevice

template<typename T>
inline T* GpuDevice::dynamic_buffer_allocate( u32* dynamic_offset ) {
    void* allocated_memory = dynamic_buffer_allocate( sizeof( T ), alignof( T ), dynamic_offset );
    return static_cast< T* >( allocated_memory );
}


} // namespace raptor
