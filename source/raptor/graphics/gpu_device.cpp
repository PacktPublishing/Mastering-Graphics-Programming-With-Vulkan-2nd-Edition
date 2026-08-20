#include "graphics/gpu_device.hpp"
#include "graphics/command_buffer.hpp"
#include "graphics/gpu_profiler.hpp"
#include "graphics/shader_compiler.hpp"

#include "foundation/memory.hpp"
#include "foundation/hash_map.hpp"
#include "foundation/file.hpp"
#include "foundation/numerics.hpp"

#include <vulkan/vk_enum_string_helper.h>
#include "external/vk_mem_alloc.h"

template<class T>
constexpr const T& raptor_min( const T& a, const T& b ) {
    return ( a < b ) ? a : b;
}

template<class T>
constexpr const T& raptor_max( const T& a, const T& b ) {
    return ( a < b ) ? b : a;
}

#define VMA_MAX raptor_max
#define VMA_MIN raptor_min
#define VMA_USE_STL_CONTAINERS 0
#define VMA_USE_STL_VECTOR 0
#define VMA_USE_STL_UNORDERED_MAP 0
#define VMA_USE_STL_LIST 0

#if defined (_MSC_VER)
#pragma warning (disable: 4127)
#pragma warning (disable: 4189)
#pragma warning (disable: 4191)
#pragma warning (disable: 4296)
#pragma warning (disable: 4324)
#pragma warning (disable: 4355)
#pragma warning (disable: 4365)
#pragma warning (disable: 4625)
#pragma warning (disable: 4626)
#pragma warning (disable: 4668)
#pragma warning (disable: 5026)
#pragma warning (disable: 5027)
#endif // _MSC_VER

//#define VMA_DEBUG_LOG rprintret

#define VMA_IMPLEMENTATION
#include "external/vk_mem_alloc.h"

// SDL and Vulkan headers
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

namespace raptor {


static void                 check_result( VkResult result );
#define                     check( result ) RASSERTM( result == VK_SUCCESS, "Vulkan assert code %u, '%s'", result, string_VkResult( result ) )

// Device implementation //////////////////////////////////////////////////

// Methods //////////////////////////////////////////////////////////////////////
#define RAPTOR_GPU_DEVICE_RESOURCE_TRACKING

#if defined (RAPTOR_GPU_DEVICE_RESOURCE_TRACKING)
//
//
struct ResourceTracker {

    void init( Allocator* allocator ) {
        resources_to_names.init( allocator, 64 );
    }

    void shutdown() {

        FlatHashMapIterator it = resources_to_names.iterator_begin();
        while ( it.is_valid() ) {
            auto kv = resources_to_names.get_structure( it );
            ResourceUpdateType::Enum type = (ResourceUpdateType::Enum)( kv.key >> 28 );
            u32 index = kv.key & 0xfffffff;
            rprint( "Leaking %s id %u\n", ResourceUpdateType::ToString( type ), index );
            resources_to_names.iterator_advance( it );
        }

        resources_to_names.shutdown();
    }

    u32 calculate_resource_id( ResourceUpdateType::Enum type, u32 index ) {
        return ( (u32)type << 28 ) | ( index & 0xfffffff );
    }

    void track_create_resource( ResourceUpdateType::Enum type, u32 index, cstring name ) {
        u32 resource_id = calculate_resource_id( type, index );

        resources_to_names.insert( resource_id, index );

        if ( track_resource && tracked_resource_type == type && ( ( tracked_resource_index == index ) || track_all_indices_per_type ) ) {
            rprint( "Creating resource %s, index %u, name %s\n", ResourceUpdateType::ToString( type ), index, name );
        }
    }

    void track_destroy_resource( ResourceUpdateType::Enum type, u32 index ) {
        u32 resource_id = calculate_resource_id( type, index );

        FlatHashMapIterator it = resources_to_names.find( resource_id );
        resources_to_names.remove( it );

        if ( track_resource && tracked_resource_type == type && ( ( tracked_resource_index == index ) || track_all_indices_per_type ) ) {
            rprint( "Destroying resource %s, index %u\n", ResourceUpdateType::ToString( type ), index );
        }
    }

    raptor::FlatHashMap<u32, u32>   resources_to_names;

    ResourceUpdateType::Enum        tracked_resource_type = ResourceUpdateType::Count;
    u32                             tracked_resource_index = k_invalid_index;
    bool                            track_resource = false;             // Global runtime switch for printing resources
    bool                            track_all_indices_per_type = false; // Set to true to print all resources of a type
    // instead of a single index.
}; // struct ResourceTracker

#else

struct ResourceTracker {

    void init( Allocator* allocator ) {
    }

    void shutdown() {
    }

    void track_create_resource( ResourceUpdateType::Enum type, u32 index, cstring name ) {
    }

    void track_destroy_resource( ResourceUpdateType::Enum type, u32 index ) {
    }

}; // struct ResourceTracker

#endif // RAPTOR_GPU_DEVICE_RESOURCE_TRACKING

// VulkanDebugOptions ////////////////////////////////////////////////////
void VulkanDebugOptions::set_none() {
    enable_validation_layers = false;
    enable_debug_utils = false;
    enable_sync_validation = false;
    enable_gpu_assisted = false;
    enable_best_practices = false;
    break_on_validation_error = false;
    break_on_validation_warning = false;
}

void VulkanDebugOptions::set_default() {
    enable_validation_layers = false;
    enable_debug_utils = true;
    enable_sync_validation = false;
    enable_gpu_assisted = false;
    enable_best_practices = false;
    break_on_validation_error = true;
    break_on_validation_warning = false;
}

void VulkanDebugOptions::set_validation() {
    enable_validation_layers = true;
    enable_debug_utils = true;

    enable_sync_validation = true;
    enable_gpu_assisted = false;
    enable_best_practices = true;

    break_on_validation_error = true;
    break_on_validation_warning = false;
}

void VulkanDebugOptions::set_gpu_assisted() {
    enable_validation_layers = true;
    enable_debug_utils = true;

    enable_sync_validation = false;
    enable_gpu_assisted = true;
    enable_best_practices = false;

    break_on_validation_error = true;
    break_on_validation_warning = false;
}

static const char* s_base_instance_extensions[] = {
    VK_KHR_SURFACE_EXTENSION_NAME,
    // Platform specific extension
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
#elif defined(VK_USE_PLATFORM_MACOS_MVK)
    VK_MVK_MACOS_SURFACE_EXTENSION_NAME,
#elif defined(VK_USE_PLATFORM_IOS_MVK)
    VK_MVK_IOS_SURFACE_EXTENSION_NAME,
#elif defined(VK_USE_PLATFORM_ANDROID_KHR)
    VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
#elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
    VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
#elif defined(VK_USE_PLATFORM_XCB_KHR)
    VK_KHR_XCB_SURFACE_EXTENSION_NAME,
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
    VK_KHR_XLIB_SURFACE_EXTENSION_NAME,
#elif defined(VK_USE_PLATFORM_DISPLAY_KHR)
    VK_KHR_DISPLAY_EXTENSION_NAME,
#endif

};


static VkBool32 debug_utils_callback( VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                      VkDebugUtilsMessageTypeFlagsEXT types,
                                      const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
                                      void* user_data ) {

    GpuDevice* device = (GpuDevice*)user_data;

    const bool is_validation = ( types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT ) != 0;
    const bool is_warning_or_error = ( severity & ( VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ) ) != 0;

    rprint( " MessageID: %s %i\nMessage: %s\n\n",
            callback_data->pMessageIdName, callback_data->messageIdNumber, callback_data->pMessage );

    bool trigger_break = false;
    if ( is_validation && ( severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ) ) {
        trigger_break = device->debug_options.break_on_validation_error;
    } else if ( is_validation && ( severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT ) ) {
        trigger_break = device->debug_options.break_on_validation_warning;
    }

#if defined(_MSC_VER)
    if ( trigger_break ) {
        __debugbreak();
    }
#endif

    return VK_FALSE;
}

static VkDebugUtilsMessengerCreateInfoEXT create_debug_utils_messenger_info( GpuDevice* device ) {
    VkDebugUtilsMessengerCreateInfoEXT info{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
    info.pUserData = device;
    info.pfnUserCallback = debug_utils_callback;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
    return info;
}

static SDL_Window* sdl_window;

static CommandBufferManager command_buffer_manager;
static ResourceTracker resource_tracker;

static const u32        k_bindless_texture_binding = 10;
static const u32        k_bindless_image_binding = 11;
static const u32        k_bindless_sampler_binding = 12;
static const u32        k_bindless_ssbo_binding = 13;
static const u32        k_max_bindless_resources = 1024;
// TODO[gabriel]: how can we have different max resources for samplers ?
//static const u32        k_max_sampler_resources = 1024;

void GpuDevice::init( const GpuDeviceCreation& creation ) {

    rprint( "Gpu Device init\n" );
    // 1. Perform common code
    allocator = creation.allocator;
    debug_options = creation.debug_options;

    ArenaAllocator* temp_allocator = MemoryService::instance()->get_thread_allocator();

    string_buffer.init( 1024 * 1024, creation.allocator );

    const u32 vulkan_api_version = VK_API_VERSION_1_4;

    // Init volk and check support for Vulkan 1.4.
    VkResult result = volkInitialize();
    check( result );
    RASSERT( volkGetInstanceVersion() >= vulkan_api_version );

    if ( debug_options.enable_validation_layers ) {
        sizet marker = temp_allocator->get_marker();

        u32 count = 0;
        vkEnumerateInstanceLayerProperties( &count, nullptr );
        VkLayerProperties* layers = (VkLayerProperties*)ralloca( sizeof( VkLayerProperties ) * count, temp_allocator );
        vkEnumerateInstanceLayerProperties( &count, layers );

        bool has_validation_layer = false;
        for ( u32 i = 0; i < count; ++i ) {
            if ( strcmp( layers[ i ].layerName, "VK_LAYER_KHRONOS_validation" ) == 0 ) {
                has_validation_layer = true;
                break;
            }
        }

        temp_allocator->free_marker( marker );

        if ( !has_validation_layer ) {
            rprint( "VK_LAYER_KHRONOS_validation not found. Disabling validation layers.\n" );
            debug_options.enable_validation_layers = false;
            debug_options.enable_sync_validation = false;
            debug_options.enable_gpu_assisted = false;
            debug_options.enable_best_practices = false;
        }

    } else {
        // Can't enable validation features without validation layers.
        debug_options.enable_sync_validation = false;
        debug_options.enable_gpu_assisted = false;
        debug_options.enable_best_practices = false;
    }

    debug_utils_extension_present = false;

    // Debug utils availability
    if ( debug_options.enable_debug_utils ) {
        sizet marker = temp_allocator->get_marker();

        u32 count = 0;
        vkEnumerateInstanceExtensionProperties( nullptr, &count, nullptr );
        VkExtensionProperties* exts = (VkExtensionProperties*)ralloca( sizeof( VkExtensionProperties ) * count, temp_allocator );
        vkEnumerateInstanceExtensionProperties( nullptr, &count, exts );

        bool has_debug_utils_extension = false;
        for ( u32 i = 0; i < count; ++i ) {
            if ( strcmp( exts[ i ].extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME ) == 0 ) {
                has_debug_utils_extension = true;
                debug_utils_extension_present = true;
                break;
            }
        }

        if ( !has_debug_utils_extension ) {
            debug_options.enable_debug_utils = false;
        }

        temp_allocator->free_marker( marker );
    }

    sizet initial_temp_allocator_marker = temp_allocator->get_marker();

    // Build instance extension and layers arrays
    Array<cstring> instance_extensions, instance_layers;
    instance_extensions.init( temp_allocator, 16 );
    instance_layers.init( temp_allocator, 16 );

    for ( u32 i = 0; i < ArraySize( s_base_instance_extensions ); ++i ) {
        instance_extensions.push( s_base_instance_extensions[ i ] );
    }

    if ( debug_options.enable_debug_utils ) {
        instance_extensions.push( VK_EXT_DEBUG_UTILS_EXTENSION_NAME );
    }

    if ( debug_options.enable_validation_layers ) {
        instance_layers.push( "VK_LAYER_KHRONOS_validation" );
    }

    // Build pNext chain
    void* instance_pnext = nullptr;

    VkDebugUtilsMessengerCreateInfoEXT debug_ci{};
    if ( debug_options.enable_debug_utils ) {
        debug_ci = create_debug_utils_messenger_info( this );
        debug_ci.pNext = instance_pnext;
        instance_pnext = &debug_ci;
    }

    const bool use_gpu_assisted = debug_options.enable_validation_layers &&
                                  debug_options.enable_gpu_assisted;

    const bool use_sync_validation = debug_options.enable_validation_layers && 
                                     debug_options.enable_sync_validation &&
                                     !use_gpu_assisted;

    const bool use_best_practices = debug_options.enable_validation_layers &&
                                    debug_options.enable_best_practices &&
                                    !use_gpu_assisted;

    const bool use_layer_settings = use_gpu_assisted || use_sync_validation ||use_best_practices;

    VkBool32 setting_true = VK_TRUE;
    VkBool32 setting_false = VK_FALSE;

    StaticArray<VkLayerSettingEXT, 8> layer_settings;

    VkLayerSettingsCreateInfoEXT layer_settings_ci{
        VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT
    };

    if ( use_layer_settings ) {
        instance_extensions.push( VK_EXT_LAYER_SETTINGS_EXTENSION_NAME );

        if ( use_gpu_assisted ) {
            layer_settings.push( { "VK_LAYER_KHRONOS_validation", "gpuav_enable", 
                                 VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &setting_true } );

            // GPU-AV without Core Checks
            layer_settings.push( { "VK_LAYER_KHRONOS_validation", "validate_core",
                                 VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &setting_false } );
        }

        if ( use_sync_validation ) {
            layer_settings.push( { "VK_LAYER_KHRONOS_validation", "validate_sync", 
                                 VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &setting_true } );
        }

        if ( use_best_practices ) {
            layer_settings.push( { "VK_LAYER_KHRONOS_validation", "validate_best_practices",
                                 VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &setting_true } );
        }

        layer_settings_ci.settingCount = layer_settings.size;
        layer_settings_ci.pSettings = layer_settings.data;
        layer_settings_ci.pNext = instance_pnext;
        instance_pnext = &layer_settings_ci;
    }

    //////// Init Vulkan instance.
    vulkan_allocation_callbacks = nullptr;

    VkApplicationInfo app_info{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app_info.pApplicationName = "Raptor Graphics Device";
    app_info.applicationVersion = 1;
    app_info.pEngineName = "Raptor";
    app_info.engineVersion = 1;
    app_info.apiVersion = VK_API_VERSION_1_4;

    VkInstanceCreateInfo ci{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo = &app_info;
    ci.enabledExtensionCount = instance_extensions.size;
    ci.ppEnabledExtensionNames = instance_extensions.data;
    ci.enabledLayerCount = instance_layers.size;
    ci.ppEnabledLayerNames = instance_layers.data;
    ci.pNext = instance_pnext;

    //// Create Vulkan Instance
    result = vkCreateInstance( &ci, vulkan_allocation_callbacks, &vulkan_instance );
    check( result );

    volkLoadInstanceOnly( vulkan_instance );

    swapchain_width = creation.width;
    swapchain_height = creation.height;

    //// Create debug
    if ( debug_options.enable_debug_utils ) {
        VkDebugUtilsMessengerCreateInfoEXT messenger_ci = create_debug_utils_messenger_info( this );
        VkResult r = vkCreateDebugUtilsMessengerEXT( vulkan_instance, &messenger_ci,
                                                        vulkan_allocation_callbacks, &vulkan_debug_utils_messenger );
        if ( r != VK_SUCCESS ) {
            rprint( "vkCreateDebugUtilsMessengerEXT failed. Disabling debug messenger.\n" );
            vulkan_debug_utils_messenger = VK_NULL_HANDLE;
        }
    }

    temp_allocator->free_marker( initial_temp_allocator_marker );

    //////// Choose physical device
    u32 num_physical_device;
    result = vkEnumeratePhysicalDevices( vulkan_instance, &num_physical_device, NULL );
    check( result );

    VkPhysicalDevice* gpus = (VkPhysicalDevice*)ralloca( sizeof( VkPhysicalDevice ) * num_physical_device, temp_allocator );
    result = vkEnumeratePhysicalDevices( vulkan_instance, &num_physical_device, gpus );
    check( result );

    VkPhysicalDevice discrete_gpu = VK_NULL_HANDLE;
    VkPhysicalDevice integrated_gpu = VK_NULL_HANDLE;
    for ( u32 i = 0; i < num_physical_device; ++i ) {
        VkPhysicalDevice physical_device = gpus[ i ];
        vkGetPhysicalDeviceProperties( physical_device, &vulkan_physical_properties );

        if ( vulkan_physical_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ) {
            discrete_gpu = physical_device;
            continue;
        }

        if ( vulkan_physical_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ) {
            integrated_gpu = physical_device;
            continue;
        }
    }

    if ( discrete_gpu != VK_NULL_HANDLE ) {
        vulkan_physical_device = discrete_gpu;
    } else if ( integrated_gpu != VK_NULL_HANDLE ) {
        vulkan_physical_device = integrated_gpu;
    } else {
        RASSERTM( false, "Suitable GPU device not found!" );
        return;
    }

    temp_allocator->free_marker( initial_temp_allocator_marker );

    {
        initial_temp_allocator_marker = temp_allocator->get_marker();

        u32 device_extension_count = 0;
        vkEnumerateDeviceExtensionProperties( vulkan_physical_device, nullptr, &device_extension_count, nullptr );
        VkExtensionProperties* extensions = (VkExtensionProperties*)ralloca( sizeof( VkExtensionProperties ) * device_extension_count, temp_allocator );
        vkEnumerateDeviceExtensionProperties( vulkan_physical_device, nullptr, &device_extension_count, extensions );
        for ( size_t i = 0; i < device_extension_count; i++ ) {

            if ( !strcmp( extensions[ i ].extensionName, VK_EXT_MESH_SHADER_EXTENSION_NAME ) ) {
                mesh_shaders_extension_present = true && ( !creation.force_disable_mesh_shaders );
                continue;
            }

            if ( !strcmp( extensions[ i ].extensionName, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME ) ) {
                fragment_shading_rate_present = true && ( creation.enable_vrs == 1 );
                continue;
            }

            if ( !strcmp( extensions[ i ].extensionName, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME ) ) {
                ray_tracing_present = true && ( creation.enable_ray_tracing == 1 );
                continue;
            }

            if ( !strcmp( extensions[ i ].extensionName, VK_KHR_RAY_QUERY_EXTENSION_NAME ) ) {
                ray_query_present = true && ( creation.enable_ray_tracing == 1 );
                continue;
            }

            if ( !strcmp( extensions[ i ].extensionName, VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME ) ) {
                unified_layout_present = true && ( creation.enable_unified_layouts == 1 );
                continue;
            }

            if ( !strcmp( extensions[ i ].extensionName, VK_KHR_PIPELINE_BINARY_EXTENSION_NAME ) ) {
                pipeline_binary_present = true && ( creation.enable_pipeline_binary == 1 );
                continue;
            }

            if ( !strcmp( extensions[ i ].extensionName, VK_EXT_DEVICE_FAULT_EXTENSION_NAME ) ) {
                device_fault_extension_present = true;
                continue;
            }
        }

        temp_allocator->free_marker( initial_temp_allocator_marker );
    }

    vkGetPhysicalDeviceProperties( vulkan_physical_device, &vulkan_physical_properties );
    gpu_timestamp_frequency = vulkan_physical_properties.limits.timestampPeriod / ( 1000 * 1000 );

    rprint( "GPU Used: %s\n", vulkan_physical_properties.deviceName );

    void* physical_device_properties_pnext = nullptr;

    VkPhysicalDeviceSubgroupProperties subgroup_properties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES };
    subgroup_properties.pNext = NULL;

    VkPhysicalDeviceProperties2 physical_device_properties_2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    physical_device_properties_pnext = &subgroup_properties;

    VkPhysicalDeviceFragmentShadingRatePropertiesKHR fragment_shading_rate_properties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR };
    if ( fragment_shading_rate_present ) {
        fragment_shading_rate_properties.pNext = physical_device_properties_pnext;
        physical_device_properties_pnext = &fragment_shading_rate_properties;
    }

    ray_tracing_pipeline_properties = VkPhysicalDeviceRayTracingPipelinePropertiesKHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR };
    if ( ray_tracing_present ) {
        ray_tracing_pipeline_properties.pNext = physical_device_properties_pnext;
        physical_device_properties_pnext = &ray_tracing_pipeline_properties;
    }

    if ( pipeline_binary_present ) {
        vulkan_pipeline_binary_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_PROPERTIES_KHR;
        vulkan_pipeline_binary_properties.pNext = physical_device_properties_pnext;
        physical_device_properties_pnext = &vulkan_pipeline_binary_properties;
    }

    physical_device_properties_2.pNext = physical_device_properties_pnext;

    vkGetPhysicalDeviceProperties2( vulkan_physical_device, &physical_device_properties_2 );

    subgroup_size = subgroup_properties.subgroupSize;

    if ( fragment_shading_rate_present ) {
        min_fragment_shading_rate_texel_size = fragment_shading_rate_properties.minFragmentShadingRateAttachmentTexelSize;
    }

    ubo_alignment = vulkan_physical_properties.limits.minUniformBufferOffsetAlignment;
    ssbo_alignemnt = vulkan_physical_properties.limits.minStorageBufferOffsetAlignment;
    max_framebuffer_layers = vulkan_physical_properties.limits.maxFramebufferLayers;

    //////// Create logical device
    u32 queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties( vulkan_physical_device, &queue_family_count, nullptr );

    VkQueueFamilyProperties* queue_families = (VkQueueFamilyProperties*)ralloca( sizeof( VkQueueFamilyProperties ) * queue_family_count, temp_allocator );
    vkGetPhysicalDeviceQueueFamilyProperties( vulkan_physical_device, &queue_family_count, queue_families );

    {
        u32 main_queue_family_index = u32_max, transfer_queue_family_index = u32_max, compute_queue_family_index = u32_max, present_queue_family_index = u32_max;
        u32 compute_queue_index = u32_max;
        for ( u32 fi = 0; fi < queue_family_count; ++fi ) {
            VkQueueFamilyProperties queue_family = queue_families[ fi ];

            if ( queue_family.queueCount == 0 ) {
                continue;
            }
#if defined(_DEBUG)
            rprint( "Family %u, flags %u queue count %u\n", fi, queue_family.queueFlags, queue_family.queueCount );
#endif // DEBUG

            // Search for main queue that should be able to do all work (graphics, compute and transfer)
            if ( ( queue_family.queueFlags & ( VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT ) ) == ( VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT ) ) {
                main_queue_family_index = fi;

                RASSERT( ( queue_family.queueFlags & VK_QUEUE_SPARSE_BINDING_BIT ) == VK_QUEUE_SPARSE_BINDING_BIT );

                // NOTE: removing this will use a real compute only queue.
                if ( queue_family.queueCount > 1 && !creation.prefer_dedicated_compute_family ) {
                    compute_queue_family_index = fi;
                    compute_queue_index = 1;
                }

                continue;
            }

            // Search for another compute queue if graphics queue exposes only one queue
            if ( ( queue_family.queueFlags & VK_QUEUE_COMPUTE_BIT ) &&
                 ( ( queue_family.queueFlags & VK_QUEUE_GRAPHICS_BIT ) == 0 ) &&
                 ( compute_queue_index == u32_max ) ) {
                compute_queue_family_index = fi;
                compute_queue_index = 0;
            }

            // Search for transfer queue
            if ( ( ( queue_family.queueFlags & VK_QUEUE_COMPUTE_BIT ) == 0 ) &&
                 ( queue_family.queueFlags & VK_QUEUE_TRANSFER_BIT ) &&
                 ( transfer_queue_family_index == u32_max ) ) {
                transfer_queue_family_index = fi;
                continue;
            }
        }

        RASSERT( main_queue_family_index != u32_max );

        // Use main queue family for compute if no dedicated found
        if ( compute_queue_family_index == u32_max ) {
            compute_queue_family_index = main_queue_family_index;
        }

        // Use main queue family for transfer if no dedicated found
        if ( transfer_queue_family_index == u32_max ) {
            transfer_queue_family_index = main_queue_family_index;
        }

        // Cache family indices
        vulkan_main_queue_family = main_queue_family_index;
        vulkan_compute_queue_family = compute_queue_family_index;
        vulkan_transfer_queue_family = transfer_queue_family_index;
    }

    Array<const char*> device_extensions;
    device_extensions.init( temp_allocator, 16 );
    device_extensions.push( VK_KHR_SWAPCHAIN_EXTENSION_NAME );

    if ( mesh_shaders_extension_present ) {
        device_extensions.push( VK_EXT_MESH_SHADER_EXTENSION_NAME );
    }

    if ( fragment_shading_rate_present ) {
        device_extensions.push( VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME );
        device_extensions.push( VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME );
        device_extensions.push( VK_KHR_MAINTENANCE2_EXTENSION_NAME );
    }

    if ( ray_tracing_present ) {
        device_extensions.push( VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME );

        device_extensions.push( VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME );
        device_extensions.push( VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME );
    }

    if ( ray_query_present ) {
        device_extensions.push( VK_KHR_RAY_QUERY_EXTENSION_NAME );
    }

    if ( unified_layout_present ) {
        device_extensions.push( VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME );
    }

    if ( pipeline_binary_present ) {
        device_extensions.push( VK_KHR_PIPELINE_BINARY_EXTENSION_NAME );
        device_extensions.push( VK_KHR_MAINTENANCE_5_EXTENSION_NAME );
    }

    if ( device_fault_extension_present ) {
        device_extensions.push( VK_EXT_DEVICE_FAULT_EXTENSION_NAME );
    }

    const float queue_priority[] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
    Array<VkDeviceQueueCreateInfo> queue_info;
    queue_info.init( temp_allocator, 8 );

    u32 main_family_queue_count = queue_families[ vulkan_main_queue_family ].queueCount;

    // Create queues
    u32 main_queue_count = 1;
    // If main family has more than one queue, request two queues so we can use one for main work and one for async compute
    if ( vulkan_compute_queue_family == vulkan_main_queue_family && main_family_queue_count >= 2 ) {
        ++main_queue_count;
    }

    // If transfer queue is the same as main, and there is still room, request one more for async transfers
    if ( vulkan_transfer_queue_family == vulkan_main_queue_family && main_family_queue_count >= ( main_queue_count + 1 ) ) {
        ++main_queue_count;
    }

    VkDeviceQueueCreateInfo& main_queue = queue_info.push_use();
    main_queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    main_queue.queueFamilyIndex = vulkan_main_queue_family;
    main_queue.queueCount = main_queue_count;
    main_queue.pQueuePriorities = queue_priority;

    bool separate_compute_queue = vulkan_compute_queue_family < queue_family_count &&
        vulkan_compute_queue_family != vulkan_main_queue_family;

    if ( separate_compute_queue ) {
        VkDeviceQueueCreateInfo& compute_queue = queue_info.push_use();
        compute_queue.queueFamilyIndex = vulkan_compute_queue_family;
        compute_queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        compute_queue.queueCount = 1;
        compute_queue.pQueuePriorities = queue_priority;
    }

    bool separate_transfer_queue = vulkan_transfer_queue_family < queue_family_count &&
        vulkan_transfer_queue_family != vulkan_main_queue_family &&
        vulkan_transfer_queue_family != vulkan_compute_queue_family;

    if ( separate_transfer_queue ) {
        VkDeviceQueueCreateInfo& transfer_queue_info = queue_info.push_use();
        transfer_queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        transfer_queue_info.queueFamilyIndex = vulkan_transfer_queue_family;
        transfer_queue_info.queueCount = 1;
        transfer_queue_info.pQueuePriorities = queue_priority;
    }

    VkPhysicalDeviceFeatures2 physical_features2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    VkPhysicalDeviceVulkan11Features vulkan_11_features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
    // NOTE: we try to use maintenance 8 to avoid a validation error when copying depth to a colour image with same format
    VkPhysicalDeviceMaintenance8FeaturesKHR maintenance_8_features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_8_FEATURES_KHR };
    maintenance_8_features.pNext = &vulkan_11_features;
    void* current_pnext = &maintenance_8_features;

    VkPhysicalDeviceVulkan12Features vulkan_12_features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    vulkan_12_features.pNext = current_pnext;
    current_pnext = &vulkan_12_features;

    VkPhysicalDeviceVulkan13Features vulkan_13_features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    vulkan_13_features.pNext = current_pnext;
    current_pnext = &vulkan_13_features;

    VkPhysicalDeviceMeshShaderFeaturesEXT mesh_shaders_features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT };
    if ( mesh_shaders_extension_present ) {
        mesh_shaders_features.pNext = current_pnext;
        current_pnext = &mesh_shaders_features;
    }

    VkPhysicalDeviceFragmentShadingRateFeaturesKHR shading_rate_features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR };
    if ( fragment_shading_rate_present ) {
        shading_rate_features.pNext = current_pnext;
        current_pnext = &shading_rate_features;
    }

    acceleration_structure_features = VkPhysicalDeviceAccelerationStructureFeaturesKHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
    ray_tracing_pipeline_features = VkPhysicalDeviceRayTracingPipelineFeaturesKHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR };
    if ( ray_tracing_present ) {
        ray_tracing_pipeline_features.pNext = &acceleration_structure_features;
        acceleration_structure_features.pNext = current_pnext;

        current_pnext = &ray_tracing_pipeline_features;
    }

    ray_query_features = VkPhysicalDeviceRayQueryFeaturesKHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
    if ( ray_query_present ) {
        ray_query_features.pNext = current_pnext;
        current_pnext = &ray_query_features;
    }

    VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR unified_layout_features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR };
    if ( unified_layout_present ) {
        unified_layout_features.pNext = current_pnext;
        current_pnext = &unified_layout_features;
    }

    VkPhysicalDeviceFaultFeaturesEXT device_fault_features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT };
    if ( device_fault_extension_present ) {
        device_fault_features.pNext = current_pnext;
        current_pnext = &device_fault_features;
    }

    physical_features2.pNext = current_pnext;

    vkGetPhysicalDeviceFeatures2( vulkan_physical_device, &physical_features2 );

    // For the feature to be correctly working, we need both the possibility to partially bind a descriptor,
    // as some entries in the bindless array will be empty, and SpirV runtime descriptors.
    bindless_supported = creation.enable_bindless && 
                         vulkan_12_features.descriptorBindingPartiallyBound &&
                         vulkan_12_features.runtimeDescriptorArray;

    // Disable if shading rate is disabled
    mesh_shaders_features.primitiveFragmentShadingRateMeshShader =
        ( mesh_shaders_features.primitiveFragmentShadingRateMeshShader == VK_TRUE ) && 
        ( fragment_shading_rate_present == 1 );

    // NOTE(marco): needed for virtual textures
    RASSERT( physical_features2.features.sparseBinding );
    RASSERT( physical_features2.features.sparseResidencyImage3D );
    RASSERT( physical_features2.features.sparseResidencyImage2D );

    // 
    device_fault_enabled = device_fault_extension_present && device_fault_features.deviceFault == VK_TRUE;

    if ( device_fault_extension_present ) {
        device_fault_features.deviceFault = device_fault_enabled ? VK_TRUE : VK_FALSE;

        // Get only textual report
        device_fault_features.deviceFaultVendorBinary = VK_FALSE;
    }

    rprint( "VK_EXT_device_fault: %s\n", device_fault_enabled ? "enabled" : "unavailable" );

    VkDeviceCreateInfo device_create_info{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    device_create_info.queueCreateInfoCount = queue_info.size;
    device_create_info.pQueueCreateInfos = queue_info.data;
    device_create_info.enabledExtensionCount = device_extensions.size;
    device_create_info.ppEnabledExtensionNames = device_extensions.data;
    device_create_info.pNext = &physical_features2;

    result = vkCreateDevice( vulkan_physical_device, &device_create_info, vulkan_allocation_callbacks, &vulkan_device );
    check( result );

    volkLoadDevice( vulkan_device );

    if ( fragment_shading_rate_present ) {

        u32 shading_rates_count = 0;
        vkGetPhysicalDeviceFragmentShadingRatesKHR( vulkan_physical_device, &shading_rates_count, nullptr );

        fragment_shading_rates.init( allocator, shading_rates_count, shading_rates_count );
        memset( fragment_shading_rates.data, 0, sizeof( VkPhysicalDeviceFragmentShadingRateKHR ) * shading_rates_count );

        for ( u32 fsr_index = 0; fsr_index < shading_rates_count; ++fsr_index ) {
            fragment_shading_rates[ fsr_index ].sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR;
        }

        vkGetPhysicalDeviceFragmentShadingRatesKHR( vulkan_physical_device, &shading_rates_count, fragment_shading_rates.data );
    }

    if ( pipeline_binary_present ) {
        global_binary_key.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR;
        vkGetPipelineKeyKHR( vulkan_device, NULL, &global_binary_key );
    }

    // Get main queue
    vkGetDeviceQueue( vulkan_device, vulkan_main_queue_family, 0, &vulkan_main_queue );

    // Get compute queue
    if ( separate_compute_queue ) {
        vkGetDeviceQueue( vulkan_device, vulkan_compute_queue_family, 0, &vulkan_compute_queue );
    }
    else {
        // If compute queue is same as graphics, get second queue if available
        u32 compute_queue_index = ( main_queue_count >= 2 ) ? 1 : 0;
        vkGetDeviceQueue( vulkan_device, vulkan_main_queue_family, compute_queue_index, &vulkan_compute_queue );
    }

    // Get transfer queue if present
    if ( separate_transfer_queue ) {
        vkGetDeviceQueue( vulkan_device, vulkan_transfer_queue_family, 0, &vulkan_transfer_queue );
    }
    else {
        u32 transfer_queue_index = 0;

        if ( main_queue_count >= 3 ) {
            transfer_queue_index = 2;
        } else if ( main_queue_count == 2 ) {
            // Alias with compute queue
            transfer_queue_index = 1;
        } else {
            // Alias with main queue
            transfer_queue_index = 0;
        }

        vkGetDeviceQueue( vulkan_device, vulkan_main_queue_family, transfer_queue_index, &vulkan_transfer_queue );
    }

    //////// Create drawable surface
    // Create surface
    SDL_Window* window = (SDL_Window*)creation.window;
    if ( SDL_Vulkan_CreateSurface( window, vulkan_instance, vulkan_allocation_callbacks, &vulkan_window_surface ) == false ) {
        rprint( "Failed to create Vulkan surface.\n" );
    }

    sdl_window = window;

    // Create Framebuffers
    int window_width, window_height;
    SDL_GetWindowSize( window, &window_width, &window_height );

    //// Select Surface Format
    //const TextureFormat::Enum swapchain_formats[] = { TextureFormat::B8G8R8A8_UNORM, TextureFormat::R8G8B8A8_UNORM, TextureFormat::B8G8R8X8_UNORM, TextureFormat::B8G8R8X8_UNORM };
    const VkFormat surface_image_formats[] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8_UNORM, VK_FORMAT_R8G8B8_UNORM };
    const VkColorSpaceKHR surface_color_space = VK_COLORSPACE_SRGB_NONLINEAR_KHR;

    u32 supported_count;
    vkGetPhysicalDeviceSurfaceFormatsKHR( vulkan_physical_device, vulkan_window_surface, &supported_count, NULL );
    VkSurfaceFormatKHR* supported_formats = (VkSurfaceFormatKHR*)ralloca( sizeof( VkSurfaceFormatKHR ) * supported_count, temp_allocator );
    vkGetPhysicalDeviceSurfaceFormatsKHR( vulkan_physical_device, vulkan_window_surface, &supported_count, supported_formats );

    // Cache render pass output
    swapchain_output.reset();

    //// Check for supported formats
    bool format_found = false;
    const u32 surface_format_count = ArraySize( surface_image_formats );

    for ( int i = 0; i < surface_format_count; i++ ) {
        for ( u32 j = 0; j < supported_count; j++ ) {
            if ( supported_formats[ j ].format == surface_image_formats[ i ] && supported_formats[ j ].colorSpace == surface_color_space ) {
                vulkan_surface_format = supported_formats[ j ];
                format_found = true;
                break;
            }
        }

        if ( format_found )
            break;
    }

    swapchain_output.depth( VK_FORMAT_D32_SFLOAT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL );
    swapchain_output.set_depth_stencil_operations( VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_LOAD_OP_CLEAR );

    // Default to the first format supported.
    if ( !format_found ) {
        vulkan_surface_format = supported_formats[ 0 ];
        RASSERT( false );
    }

    swapchain_output.color( vulkan_surface_format.format, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ATTACHMENT_LOAD_OP_CLEAR );

    set_present_mode( present_mode );

    //////// Create VMA Allocator
    const VmaVulkanFunctions funcs = {
      .vkGetInstanceProcAddr = vkGetInstanceProcAddr,
      .vkGetDeviceProcAddr = vkGetDeviceProcAddr,
      .vkGetPhysicalDeviceProperties = vkGetPhysicalDeviceProperties,
      .vkGetPhysicalDeviceMemoryProperties = vkGetPhysicalDeviceMemoryProperties,
      .vkAllocateMemory = vkAllocateMemory,
      .vkFreeMemory = vkFreeMemory,
      .vkMapMemory = vkMapMemory,
      .vkUnmapMemory = vkUnmapMemory,
      .vkFlushMappedMemoryRanges = vkFlushMappedMemoryRanges,
      .vkInvalidateMappedMemoryRanges = vkInvalidateMappedMemoryRanges,
      .vkBindBufferMemory = vkBindBufferMemory,
      .vkBindImageMemory = vkBindImageMemory,
      .vkGetBufferMemoryRequirements = vkGetBufferMemoryRequirements,
      .vkGetImageMemoryRequirements = vkGetImageMemoryRequirements,
      .vkCreateBuffer = vkCreateBuffer,
      .vkDestroyBuffer = vkDestroyBuffer,
      .vkCreateImage = vkCreateImage,
      .vkDestroyImage = vkDestroyImage,
      .vkCmdCopyBuffer = vkCmdCopyBuffer,
      .vkGetBufferMemoryRequirements2KHR = vkGetBufferMemoryRequirements2,
      .vkGetImageMemoryRequirements2KHR = vkGetImageMemoryRequirements2,
      .vkBindBufferMemory2KHR = vkBindBufferMemory2,
      .vkBindImageMemory2KHR = vkBindImageMemory2,
      .vkGetPhysicalDeviceMemoryProperties2KHR = vkGetPhysicalDeviceMemoryProperties2,
      .vkGetDeviceBufferMemoryRequirements = vkGetDeviceBufferMemoryRequirements,
      .vkGetDeviceImageMemoryRequirements = vkGetDeviceImageMemoryRequirements,
    };

    VmaAllocatorCreateInfo allocatorInfo = {};
    allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    allocatorInfo.physicalDevice = vulkan_physical_device;
    allocatorInfo.pVulkanFunctions = &funcs,
        allocatorInfo.device = vulkan_device;
    allocatorInfo.instance = vulkan_instance;

    result = vmaCreateAllocator( &allocatorInfo, &vma_allocator );
    check( result );

    ////////  Create Descriptor Pools
    //if ( !descriptor_buffer_present ) 
    {
        const GpuDescriptorPoolCreation& pool_creation = creation.descriptor_pool_creation;
        Array<VkDescriptorPoolSize> pool_sizes( temp_allocator,
        {
            { VK_DESCRIPTOR_TYPE_SAMPLER, pool_creation.samplers },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, pool_creation.combined_image_samplers },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, pool_creation.sampled_image },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, pool_creation.storage_image },
            { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, pool_creation.uniform_texel_buffers },
            { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, pool_creation.storage_texel_buffers },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, pool_creation.uniform_buffer },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, pool_creation.storage_buffer },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, pool_creation.uniform_buffer_dynamic },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, pool_creation.storage_buffer_dynamic },
            { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, pool_creation.input_attachments },
        });

        if ( creation.enable_ray_tracing ) {
            pool_sizes.push( { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, pool_creation.tlas } );
        }

        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        // TODO:
        pool_info.maxSets = creation.resource_pool_creation.descriptor_sets;
        pool_info.poolSizeCount = pool_sizes.size;
        pool_info.pPoolSizes = pool_sizes.data;
        result = vkCreateDescriptorPool( vulkan_device, &pool_info, vulkan_allocation_callbacks, &vulkan_descriptor_pool );
        check( result );

        // [TAG: BINDLESS]
        // Create the Descriptor Pool used by bindless, that needs update after bind flag.
        if ( bindless_supported ) {
            VkDescriptorPoolSize pool_sizes_bindless[] =
            {
                { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, k_max_bindless_resources },
                { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, k_max_bindless_resources },
                { VK_DESCRIPTOR_TYPE_SAMPLER, k_max_bindless_resources },
                { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, k_max_bindless_resources },
            };

            // Update after bind is needed here, for each binding and in the descriptor set layout creation.
            pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT_EXT;
            pool_info.maxSets = k_max_bindless_resources * ArraySize( pool_sizes_bindless );
            pool_info.poolSizeCount = (u32)ArraySize( pool_sizes_bindless );
            pool_info.pPoolSizes = pool_sizes_bindless;
            result = vkCreateDescriptorPool( vulkan_device, &pool_info, vulkan_allocation_callbacks, &vulkan_bindless_descriptor_pool );
            check( result );
        }
    }

    // Final use of temp allocator, free all temporary memory created here.
    temp_allocator->free_marker( initial_temp_allocator_marker );

    // Init render frame informations. This includes fences, semaphores, command buffers, ...
    // Create vulkan pools
    const u32 num_pools = creation.num_threads * k_max_frames;
    num_threads = creation.num_threads;

    command_buffer_manager.init( this, creation.num_threads );

    gpu_profiler = rallocat( GpuProfiler, allocator );
    gpu_profiler->init( this, allocator, creation.gpu_time_queries_per_frame, creation.num_threads, k_max_frames );

    //////// Create resource pools
    const GpuResourcePoolCreation& resource_pool_creation = creation.resource_pool_creation;
    buffers_pool.init( allocator, resource_pool_creation.buffers );
    images.init( allocator, resource_pool_creation.images );
    image_views.init( allocator, resource_pool_creation.image_views );
    descriptor_set_layouts_pool.init( allocator, resource_pool_creation.descriptor_set_layouts );
    pipelines.init( allocator, resource_pool_creation.pipelines );
    shader_states.init( allocator, resource_pool_creation.shaders );
    descriptor_sets_pool.init( allocator, resource_pool_creation.descriptor_sets );
    samplers.init( allocator, resource_pool_creation.samplers );
    page_pools_pool.init( allocator, resource_pool_creation.page_pools );
    pipeline_layouts.init( allocator, resource_pool_creation.pipeline_layouts );
    blases.init( allocator, resource_pool_creation.blases );
    tlases.init( allocator, resource_pool_creation.tlases );

    pending_sparse_queue_binds.init( allocator, 1024 );
    pending_sparse_memory_info.init( allocator, 1024 );

    // Create binary semaphores
    // NOTE: swapchain does not support timeline semaphores for image acquisition
    VkSemaphoreCreateInfo semaphore_info{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    vkCreateSemaphore( vulkan_device, &semaphore_info, vulkan_allocation_callbacks, &vulkan_bind_binary_semaphore );

    for ( size_t i = 0; i < k_max_frames; i++ ) {
        vkCreateSemaphore( vulkan_device, &semaphore_info, vulkan_allocation_callbacks, &vulkan_image_acquired_binary_semaphore[ i ] );
    }

    for ( size_t i = 0; i < k_max_swapchain_images; ++i ) {
        vkCreateSemaphore( vulkan_device, &semaphore_info, vulkan_allocation_callbacks, &vulkan_render_complete_binary_semaphore[ i ] );
    }

    // Timeline semaphores
    VkSemaphoreTypeCreateInfo semaphore_type_info{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    semaphore_type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    semaphore_info.pNext = &semaphore_type_info;

    vkCreateSemaphore( vulkan_device, &semaphore_info, vulkan_allocation_callbacks, &vulkan_graphics_timeline_semaphore );
    vkCreateSemaphore( vulkan_device, &semaphore_info, vulkan_allocation_callbacks, &vulkan_compute_timeline_semaphore );

    // Fence for immediate command buffer submission
    VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    vkCreateFence( vulkan_device, &fenceInfo, vulkan_allocation_callbacks, &vulkan_immediate_fence );

    vulkan_image_index = 0;
    current_frame = 0;
    previous_frame = 0;
    absolute_frame = 0;
    timestamps_enabled = false;

    resource_deletion_queue.init( allocator, 32 );
    descriptor_set_updates.init( allocator, 32 );
    image_views_to_update_bindless.init( allocator, 32 );
    buffers_to_update_bindless.init( allocator, 32 );
    blas_build_requests.init( allocator, 32 );
    tlas_build_requests.init( allocator, 4 );

    // Init resource tracker
#if defined (RAPTOR_GPU_DEVICE_RESOURCE_TRACKING)
    resource_tracker.init( allocator );
    resource_tracker.tracked_resource_type = ResourceUpdateType::Image;
    resource_tracker.tracked_resource_index = 45;
    resource_tracker.track_resource = false;
    resource_tracker.track_all_indices_per_type = false;
#endif // RAPTOR_GPU_DEVICE_RESOURCE_TRACKING

    //////// Create swapchain
    create_swapchain();

    //
    // Init primitive resources
    //
    SamplerCreation sc{};
    sc.set_address_mode_uvw( VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE )
        .set_min_mag_mip( VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR ).set_name( "Sampler Linear Clamp" );
    global_samplers[ GlobalSamplers::LinearClamp ] = create_sampler( sc );

    sc.set_address_mode_uvw( VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT ).set_name( "Sampler Linear Repeat" );
    global_samplers[ GlobalSamplers::LinearRepeat ] = create_sampler( sc );

    sc.set_address_mode_uvw( VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE )
        .set_min_mag_mip( VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST ).set_name( "Sampler Nearest Clamp" );

    global_samplers[ GlobalSamplers::NearestClamp ] = create_sampler( sc );

    sc.set_address_mode_uvw( VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT )
        .set_min_mag_mip( VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST ).set_name( "Sampler Nearest Repeat" );

    global_samplers[ GlobalSamplers::NearestRepeat ] = create_sampler( sc );

    {
        const VkDeviceSize fullscreen_size = 3 * 3 * sizeof( f32 );

        fullscreen_vertex_buffer = create_buffer( {
            .size = fullscreen_size,
            .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
            .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .name = "fullscreen_vb" } );

        Buffer* fullscreen_buffer = get_buffer( fullscreen_vertex_buffer );
        RASSERT( fullscreen_buffer && fullscreen_buffer->mapped_data );

        const f32 fullscreen_vertices[] = { 
            -1.0f, -1.0f, 0.0f,
             3.0f, -1.0f, 0.0f,
            -1.0f,  3.0f, 0.0f
        };

        memcpy( fullscreen_buffer->mapped_data, fullscreen_vertices, sizeof( fullscreen_vertices ) );
        flush_buffer( fullscreen_vertex_buffer, 0, sizeof( fullscreen_vertices ) );
    }

    // Init Dummy resources
    ImageCreation dummy_image_creation;
    dummy_image_creation.set_size( 1, 1, 1 ).set_flags( TextureFlags::Mask::RenderTarget_mask | TextureFlags::Mask::Compute_mask ).set_format_type( VK_FORMAT_R8_UNORM, TextureType::Texture2D ).set_name( "Dummy_texture" );
    dummy_image = create_image( dummy_image_creation );

    dummy_image_view = create_image_view( { .parent_image = dummy_image, .view_type = VK_IMAGE_VIEW_TYPE_2D,
                                          .sub_resource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }, .name = dummy_image_creation.name } );
    add_image_view_to_bindless( dummy_image_view );

    {
        constexpr VkDeviceSize dummy_buffer_size = 16;

        dummy_constant_buffer = create_buffer( {
            .size = dummy_buffer_size,
            .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
            .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
            .name = "dummy_cb" } );

        dummy_storage_buffer = create_buffer( {
            .size = dummy_buffer_size,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
            .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
            .name = "dummy_sb" } );

        dummy_vertex_buffer = create_buffer( {
            .size = dummy_buffer_size,
            .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
            .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
            .name = "dummy_vb" } );

        const u32 dummy_data[ 4 ]{};

        {
            MapBufferParameters map{ dummy_constant_buffer, 0, dummy_buffer_size };
            void* data = map_buffer( map );
            RASSERT( data );

            memcpy( data, dummy_data, dummy_buffer_size );
            flush_buffer( dummy_constant_buffer, 0, dummy_buffer_size );
            unmap_buffer( map );
        }

        {
            MapBufferParameters map{ dummy_storage_buffer, 0, dummy_buffer_size };
            void* data = map_buffer( map );
            RASSERT( data );

            memcpy( data, dummy_data, dummy_buffer_size );
            flush_buffer( dummy_storage_buffer, 0, dummy_buffer_size );
            unmap_buffer( map );
        }

        {
            MapBufferParameters map{ dummy_vertex_buffer, 0, dummy_buffer_size };
            void* data = map_buffer( map );
            RASSERT( data );

            memcpy( data, dummy_data, dummy_buffer_size );
            flush_buffer( dummy_vertex_buffer, 0, dummy_buffer_size );
            unmap_buffer( map );
        }
    }

    ShaderCompiler::init( allocator );

    // [TAG: BINDLESS]
    // Bindless resources creation
    if ( bindless_supported ) {
        bindless_descriptor_set_layout = create_descriptor_set_layout( {
            .bindings = { { k_bindless_texture_binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, k_max_bindless_resources },
                          { k_bindless_image_binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, k_max_bindless_resources },
                          { k_bindless_sampler_binding, VK_DESCRIPTOR_TYPE_SAMPLER, k_max_bindless_resources },
                          { k_bindless_ssbo_binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, k_max_bindless_resources } },
            .set_index = 0, .bindless = 1, .dynamic = 0, .name = "BindlessLayout" } );

        bindless_descriptor_set = create_descriptor_set( { .layout = bindless_descriptor_set_layout, .name = "bindless_set" } );

        DescriptorSet* bindless_set = get_descriptor_set( bindless_descriptor_set );
        vulkan_bindless_descriptor_set_cached = bindless_set->vk_descriptor_set;
    }

    // Dynamic buffer handling
    // TODO:
    dynamic_per_frame_size = rmega( 10 );
    VkBufferUsageFlags dynamic_buffer_usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    
    dynamic_buffer = create_buffer( {
        .size = VkDeviceSize( dynamic_per_frame_size ) * k_max_frames,
        .usage = dynamic_buffer_usage,
        .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                            VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .name = "Dynamic_Persistent_Buffer" } );

    Buffer* vk_dynamic_buffer = get_buffer( dynamic_buffer );
    dynamic_mapped_memory = vk_dynamic_buffer->mapped_data;

    if ( bindless_supported ) {
        VkDescriptorImageInfo image_infos[ GlobalSamplers::Count ];
        VkWriteDescriptorSet descriptors_to_modify[ GlobalSamplers::Count ];

        DescriptorSet* descriptor_set = get_descriptor_set( bindless_descriptor_set );

        for ( u32 i = 0; i < GlobalSamplers::Count; ++i ) {
            Sampler* sampler = get_sampler( global_samplers[ i ] );

            VkDescriptorImageInfo& image_info = image_infos[ i ];
            image_info.sampler = sampler->vk_sampler;

            VkWriteDescriptorSet& descriptor_write = descriptors_to_modify[ i ];

            descriptor_write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            descriptor_write.dstSet = descriptor_set->vk_descriptor_set;
            descriptor_write.dstBinding = k_bindless_sampler_binding;
            descriptor_write.dstArrayElement = i;
            descriptor_write.descriptorCount = 1;
            descriptor_write.pImageInfo = &image_info;
        }

        vkUpdateDescriptorSets( vulkan_device, GlobalSamplers::Count, descriptors_to_modify, 0, nullptr );
    }
}

void GpuDevice::shutdown() {

    vkDeviceWaitIdle( vulkan_device );

    command_buffer_manager.shutdown();

    for ( size_t i = 0; i < k_max_frames; i++ ) {
        vkDestroySemaphore( vulkan_device, vulkan_image_acquired_binary_semaphore[ i ], vulkan_allocation_callbacks );
    }

    for ( size_t i = 0; i < k_max_swapchain_images; ++i ) {
        vkDestroySemaphore( vulkan_device, vulkan_render_complete_binary_semaphore[ i ], vulkan_allocation_callbacks );
    }

    vkDestroySemaphore( vulkan_device, vulkan_graphics_timeline_semaphore, vulkan_allocation_callbacks );
    vkDestroySemaphore( vulkan_device, vulkan_compute_timeline_semaphore, vulkan_allocation_callbacks );

    vkDestroyFence( vulkan_device, vulkan_immediate_fence, vulkan_allocation_callbacks );

    vkDestroySemaphore( vulkan_device, vulkan_bind_binary_semaphore, vulkan_allocation_callbacks );

    gpu_profiler->shutdown();

    destroy_descriptor_set_layout( bindless_descriptor_set_layout );
    destroy_descriptor_set( bindless_descriptor_set );
    destroy_buffer( fullscreen_vertex_buffer );
    destroy_buffer( dynamic_buffer );
    destroy_image( dummy_image );
    destroy_image_view( dummy_image_view );
    destroy_buffer( dummy_constant_buffer );
    destroy_buffer( dummy_storage_buffer );
    destroy_buffer( dummy_vertex_buffer );

    for ( u32 i = 0; i < GlobalSamplers::Count; ++i ) {
        destroy_sampler( global_samplers[ i ] );
    }

    // Add pending bindless images to delete.
    for ( u32 i = 0; i < image_views_to_update_bindless.size; ++i ) {
        ImageViewUpdate& update = image_views_to_update_bindless[ i ];
        if ( update.deleting ) {
            destroy_image_view_instant( update.handle.id );
        }
    }

    for ( u32 i = 0; i < buffers_to_update_bindless.size; ++i ) {
        BufferUpdate& update = buffers_to_update_bindless[ i ];
        if ( update.deleting ) {
            destroy_buffer_instant( update.handle.id );
        }
    }

    // Destroy swapchain
    destroy_swapchain();
    vkDestroySurfaceKHR( vulkan_instance, vulkan_window_surface, vulkan_allocation_callbacks );

    // Destroy all pending resources.
    for ( u32 i = 0; i < resource_deletion_queue.size; i++ ) {
        ResourceUpdate& resource_deletion = resource_deletion_queue[ i ];

        // Skip just freed resources.
        if ( resource_deletion.current_frame == -1 )
            continue;

        switch ( resource_deletion.type ) {

            case ResourceUpdateType::Buffer:
            {
                destroy_buffer_instant( resource_deletion.handle );
                break;
            }

            case ResourceUpdateType::Pipeline:
            {
                destroy_pipeline_instant( resource_deletion.handle );
                break;
            }

            case ResourceUpdateType::DescriptorSet:
            {
                destroy_descriptor_set_instant( resource_deletion.handle );
                break;
            }

            case ResourceUpdateType::DescriptorSetLayout:
            {
                destroy_descriptor_set_layout_instant( resource_deletion.handle );
                break;
            }

            case ResourceUpdateType::Sampler:
            {
                destroy_sampler_instant( resource_deletion.handle );
                break;
            }

            case ResourceUpdateType::ShaderState:
            {
                destroy_shader_state_instant( resource_deletion.handle );
                break;
            }

            case ResourceUpdateType::Image:
            {
                destroy_image_instant( resource_deletion.handle );
                break;
            }

            case ResourceUpdateType::ImageView:
            {
                destroy_image_view_instant( resource_deletion.handle );
                break;
            }

            case ResourceUpdateType::PagePool:
            {
                destroy_page_pool_instant( resource_deletion.handle );
                break;
            }

            case ResourceUpdateType::PipelineLayout:
            {
                destroy_pipeline_layout_instant( resource_deletion.handle );
                break;
            }

            case ResourceUpdateType::BLAS:
            {
                destroy_blas_instant( resource_deletion.handle );
                break;
            }

            case ResourceUpdateType::TLAS:
            {
                destroy_tlas_instant( resource_deletion.handle );
                break;
            }

            default:
            {
                RASSERTM( false, "Cannot process resource type %u\n", resource_deletion.type );
                break;
            }
        }
    }

    ShaderCompiler::shutdown();

    buffers_to_update_bindless.shutdown();
    image_views_to_update_bindless.shutdown();
    resource_deletion_queue.shutdown();
    descriptor_set_updates.shutdown();
    blas_build_requests.shutdown();
    tlas_build_requests.shutdown();

    // Resource tracker shutdown, checking leaks
#if defined (RAPTOR_GPU_DEVICE_RESOURCE_TRACKING)
    resource_tracker.shutdown();
#endif // RAPTOR_GPU_DEVICE_RESOURCE_TRACKING

    pipelines.shutdown();
    buffers_pool.shutdown();
    shader_states.shutdown();
    images.shutdown();
    image_views.shutdown();
    samplers.shutdown();
    page_pools_pool.shutdown();
    descriptor_set_layouts_pool.shutdown();
    descriptor_sets_pool.shutdown();
    pipeline_layouts.shutdown();
    blases.shutdown();
    tlases.shutdown();

    pending_sparse_queue_binds.shutdown();
    pending_sparse_memory_info.shutdown();

    // Remove the debug report callback
    if ( debug_options.enable_debug_utils ) {
        vkDestroyDebugUtilsMessengerEXT( vulkan_instance, vulkan_debug_utils_messenger, vulkan_allocation_callbacks );
    }

    // [TAG: BINDLESS]
    if ( bindless_supported ) {
        vkDestroyDescriptorPool( vulkan_device, vulkan_bindless_descriptor_pool, vulkan_allocation_callbacks );
    }

    vkDestroyDescriptorPool( vulkan_device, vulkan_descriptor_pool, vulkan_allocation_callbacks );

    rfree( gpu_profiler, allocator );

    // Put this here so that pools catch which kind of resource has leaked.
    vmaDestroyAllocator( vma_allocator );

    vkDestroyDevice( vulkan_device, vulkan_allocation_callbacks );

    vkDestroyInstance( vulkan_instance, vulkan_allocation_callbacks );

    volkFinalize();

    string_buffer.shutdown();

    fragment_shading_rates.shutdown();

    rprint( "Gpu Device shutdown\n" );
}

// Resource Creation ////////////////////////////////////////////////////////////

static VkImageUsageFlags vulkan_get_image_usage( const ImageCreation& creation ) {
    const bool is_render_target = ( creation.flags & TextureFlags::RenderTarget_mask ) == TextureFlags::RenderTarget_mask;
    const bool is_compute_used = ( creation.flags & TextureFlags::Compute_mask ) == TextureFlags::Compute_mask;
    const bool is_shading_rate_texture = ( creation.flags & TextureFlags::ShadingRate_mask ) == TextureFlags::ShadingRate_mask;

    // Default to always readable from shader.
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT;

    usage |= is_compute_used ? VK_IMAGE_USAGE_STORAGE_BIT : 0;

    usage |= is_shading_rate_texture ? VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR : 0;

    if ( TextureFormat::has_depth_or_stencil( creation.format ) ) {
        // Depth/Stencil textures are normally textures you render into.
        usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT; // TODO

    } else {
        usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT; // TODO
        usage |= is_render_target ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0;
    }

    return usage;
}

static void vulkan_create_image( GpuDevice& gpu, const ImageCreation& creation, ImageHandle handle, Image* image ) {

    bool is_cubemap = false;
    u32 layer_count = creation.array_layer_count;
    if ( creation.type == TextureType::TextureCube || creation.type == TextureType::Texture_Cube_Array ) {
        is_cubemap = true;
    }

    const bool is_sparse_texture = ( creation.flags & TextureFlags::Sparse_mask ) == TextureFlags::Sparse_mask;

    image->width = creation.width;
    image->height = creation.height;
    image->depth = creation.depth;
    image->mip_base_level = 0;        // For new textures, we have a view that is for all mips and layers.
    image->array_base_layer = 0;      // For new textures, we have a view that is for all mips and layers.
    image->array_layer_count = layer_count;
    image->mip_level_count = creation.mip_level_count;
    image->type = creation.type;
    image->name = creation.name;
    image->vk_format = creation.format;
    image->vk_usage = vulkan_get_image_usage( creation );
    image->sampler = nullptr;
    image->flags = creation.flags;
    image->handle = handle;
    image->sparse = is_sparse_texture;
    image->alias_image = ImageHandle();
    image->vma_allocation = 0;
    image->sync_state = {};
    // Defaults to main queue unless specified
    image->sync_state.owner_queue_family = creation.owner_queue_family != u16_max ? creation.owner_queue_family : gpu.vulkan_main_queue_family;

    //// Create the image
    VkImageCreateInfo image_info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    image_info.format = image->vk_format;
    image_info.flags = ( is_cubemap ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0 ) | ( is_sparse_texture ? ( VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT | VK_IMAGE_CREATE_SPARSE_BINDING_BIT ) : 0 );
    image_info.imageType = to_vk_image_type( image->type );
    image_info.extent.width = creation.width;
    image_info.extent.height = creation.height;
    image_info.extent.depth = creation.depth;
    image_info.mipLevels = creation.mip_level_count;
    image_info.arrayLayers = layer_count;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = image->vk_usage;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo memory_info{};
    memory_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    rprint( "creating tex %s\n", creation.name );

    if ( creation.alias.is_invalid() ) {
        if ( is_sparse_texture ) {
            check( vkCreateImage( gpu.vulkan_device, &image_info, gpu.vulkan_allocation_callbacks, &image->vk_image ) );
        } else {
            check( vmaCreateImage( gpu.vma_allocator, &image_info, &memory_info,
                                   &image->vk_image, &image->vma_allocation, nullptr ) );

#if defined (_DEBUG)
            vmaSetAllocationName( gpu.vma_allocator, image->vma_allocation, creation.name );
#endif // _DEBUG
        }
    } else {
        Image* alias_texture = gpu.get_image( creation.alias );
        RASSERT( alias_texture != nullptr );
        RASSERT( !is_sparse_texture );

        check( vmaCreateAliasingImage( gpu.vma_allocator, alias_texture->vma_allocation, &image_info, &image->vk_image ) );
        image->alias_image = creation.alias;
    }

    gpu.set_resource_name( VK_OBJECT_TYPE_IMAGE, (u64)image->vk_image, creation.name );

    //image->state = RESOURCE_STATE_UNDEFINED;
}

static void upload_texture_data( Image* texture, void* upload_data, GpuDevice& gpu ) {

    // Create stating buffer
    VkBufferCreateInfo buffer_info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    u32 image_size = texture->width * texture->height * 4;
    buffer_info.size = image_size;

    VmaAllocationCreateInfo memory_info{};
    memory_info.flags = VMA_ALLOCATION_CREATE_STRATEGY_BEST_FIT_BIT;
    memory_info.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;

    VmaAllocationInfo allocation_info{};
    VkBuffer staging_buffer;
    VmaAllocation staging_allocation;
    ( vmaCreateBuffer( gpu.vma_allocator, &buffer_info, &memory_info,
                       &staging_buffer, &staging_allocation, &allocation_info ) );

    // Copy buffer_data
    void* destination_data;
    vmaMapMemory( gpu.vma_allocator, staging_allocation, &destination_data );
    memcpy( destination_data, upload_data, static_cast<size_t>( image_size ) );
    vmaUnmapMemory( gpu.vma_allocator, staging_allocation );

    // Execute command buffer
    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    // TODO: threading
    CommandBuffer* command_buffer = gpu.allocate_command_buffer( 0, gpu.current_frame, CommandQueueType::Graphics );
    command_buffer->reset();
    vkBeginCommandBuffer( command_buffer->vk_command_buffer, &beginInfo );

    VkBufferImageCopy region = {};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;

    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;

    region.imageOffset = { 0, 0, 0 };
    region.imageExtent = { texture->width, texture->height, texture->depth };

    VkImageSubresourceRange range = raptor::range_color_full();

    // Copy from the staging buffer to the image
    command_buffer->add_image_barrier( texture->handle, range, { VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL } );
    command_buffer->flush_barriers();
    //util_add_image_barrier( &gpu, command_buffer->vk_command_buffer, image->vk_image, RESOURCE_STATE_UNDEFINED, RESOURCE_STATE_COPY_DEST, 0, 1, false );

    vkCmdCopyBufferToImage( command_buffer->vk_command_buffer, staging_buffer, texture->vk_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region );
    // Prepare first mip to create lower mipmaps
    if ( texture->mip_level_count > 1 ) {
        //range = raptor::range_color( 0, 1 );
        command_buffer->add_image_barrier( texture->handle, range, { VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL } );
        command_buffer->flush_barriers();
        //util_add_image_barrier( &gpu, command_buffer->vk_command_buffer, image->vk_image, RESOURCE_STATE_COPY_DEST, RESOURCE_STATE_COPY_SOURCE, 0, 1, false );
    }

    i32 w = texture->width;
    i32 h = texture->height;

    for ( int mip_index = 1; mip_index < texture->mip_level_count; ++mip_index ) {

        range = raptor::range_color( mip_index, 1 );
        command_buffer->add_image_barrier( texture->handle, range, { VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL } );
        command_buffer->flush_barriers();
        //util_add_image_barrier( &gpu, command_buffer->vk_command_buffer, image->vk_image, RESOURCE_STATE_UNDEFINED, RESOURCE_STATE_COPY_DEST, mip_index, 1, false );

        VkImageBlit blit_region{ };
        blit_region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit_region.srcSubresource.mipLevel = mip_index - 1;
        blit_region.srcSubresource.baseArrayLayer = 0;
        blit_region.srcSubresource.layerCount = 1;

        blit_region.srcOffsets[ 0 ] = { 0, 0, 0 };
        blit_region.srcOffsets[ 1 ] = { w, h, 1 };

        w /= 2;
        h /= 2;

        blit_region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit_region.dstSubresource.mipLevel = mip_index;
        blit_region.dstSubresource.baseArrayLayer = 0;
        blit_region.dstSubresource.layerCount = 1;

        blit_region.dstOffsets[ 0 ] = { 0, 0, 0 };
        blit_region.dstOffsets[ 1 ] = { w, h, 1 };

        vkCmdBlitImage( command_buffer->vk_command_buffer, texture->vk_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, texture->vk_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit_region, VK_FILTER_LINEAR );

        // Prepare current mip for next level
        command_buffer->add_image_barrier( texture->handle, range, { VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL } );
        command_buffer->flush_barriers();
        //util_add_image_barrier( &gpu, command_buffer->vk_command_buffer, image->vk_image, RESOURCE_STATE_COPY_DEST, RESOURCE_STATE_COPY_SOURCE, mip_index, 1, false );
    }

    // Transition
    range = raptor::range_color( 0, texture->mip_level_count );
    command_buffer->add_image_barrier( texture->handle, range,
                                       { VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                       VK_ACCESS_2_SHADER_READ_BIT,
                                       VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL } );

    command_buffer->flush_barriers();
    //util_add_image_barrier( &gpu, command_buffer->vk_command_buffer, image->vk_image, ( image->mip_level_count > 1 ) ? RESOURCE_STATE_COPY_SOURCE : RESOURCE_STATE_COPY_DEST, RESOURCE_STATE_SHADER_RESOURCE, 0, image->mip_level_count, false );
    //image->state = RESOURCE_STATE_SHADER_RESOURCE;

    vkEndCommandBuffer( command_buffer->vk_command_buffer );

    // Submit command buffer
    VkCommandBufferSubmitInfoKHR command_buffer_info{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO_KHR };
    command_buffer_info.commandBuffer = command_buffer->vk_command_buffer;

    VkSubmitInfo2KHR submit_info{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2_KHR };
    submit_info.commandBufferInfoCount = 1;
    submit_info.pCommandBufferInfos = &command_buffer_info;

    vkQueueSubmit2( gpu.vulkan_main_queue, 1, &submit_info, VK_NULL_HANDLE );

    vkQueueWaitIdle( gpu.vulkan_main_queue );

    vmaDestroyBuffer( gpu.vma_allocator, staging_buffer, staging_allocation );

    // TODO: free command buffer
    vkResetCommandBuffer( command_buffer->vk_command_buffer, VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT );
    // TODO: needs a proper refactor
    u32 pool_index = command_buffer_manager.pool_from_indices( gpu.current_frame, 0, CommandQueueType::Graphics );
    command_buffer_manager.used_buffers[ pool_index ]--;
}

ImageHandle GpuDevice::create_image( const ImageCreation& creation ) {

    ImageHandle handle = images.obtain();
    if ( handle.is_invalid() ) {
        return handle;
    }

    resource_tracker.track_create_resource( ResourceUpdateType::Image, handle.index(), creation.name );

    Image* texture = get_image( handle );

    vulkan_create_image( *this, creation, handle, texture );

    //// Copy buffer_data if present
    if ( creation.initial_data ) {
        upload_texture_data( texture, creation.initial_data, *this );
    }

    return handle;
}

ImageViewHandle GpuDevice::create_image_view( const ImageViewCreation& creation ) {
    ImageViewHandle handle = image_views.obtain();
    if ( handle.is_invalid() ) {
        return handle;
    }

    resource_tracker.track_create_resource( ResourceUpdateType::ImageView, handle.index(), creation.name );

    ImageView* image_view = get_image_view( handle );
    Image* parent_image = get_image( creation.parent_image );

    // Add image view data
    image_view->parent_image = creation.parent_image;
    image_view->handle = handle;
    image_view->subresource_range = creation.sub_resource;
    image_view->name = creation.name;
    image_view->view_type = creation.view_type;
    image_view->compute_access = ( parent_image->vk_usage & VK_IMAGE_USAGE_STORAGE_BIT ) != 0;

    //// Create the image view
    VkImageViewCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    info.image = parent_image->vk_image;
    info.format = parent_image->vk_format;

    if ( TextureFormat::has_depth_or_stencil( parent_image->vk_format ) ) {

        info.subresourceRange.aspectMask = TextureFormat::has_depth( parent_image->vk_format ) ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_COLOR_BIT) : 0;
        // TODO:gs
        //info.subresourceRange.aspectMask |= TextureFormat::has_stencil( creation.format ) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0;
    } else {
        info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    }

    info.viewType = creation.view_type;
    info.subresourceRange = creation.sub_resource;
    check( vkCreateImageView( vulkan_device, &info, vulkan_allocation_callbacks, &image_view->vk_image_view ) );

    set_resource_name( VK_OBJECT_TYPE_IMAGE_VIEW, (u64)image_view->vk_image_view, creation.name );

    return handle;
}

PipelineLayoutHandle GpuDevice::create_pipeline_layout( const PipelineLayoutCreation& creation ) {

    PipelineLayoutHandle handle = pipeline_layouts.obtain();
    if ( handle.is_invalid() ) {
        return handle;
    }

    resource_tracker.track_create_resource( ResourceUpdateType::PipelineLayout, handle.index(), creation.name );

    PipelineLayout* layout = get_pipeline_layout( handle );

    VkDescriptorSetLayout vk_layouts[ k_max_descriptor_set_layouts ];

    u32 num_active_layouts = ( u32 )creation.layouts.size;

    // Create VkPipelineLayout
    for ( u32 l = 0; l < num_active_layouts; ++l ) {

        // [TAG: BINDLESS]
        // At index 0 there is the bindless layout.
        // TODO: improve API.
        //if ( bindless_supported && l == 0 ) {
        //    DescriptorSetLayout* s = access_descriptor_set_layout( bindless_descriptor_set_layout );
        //    // Avoid deletion of this set as it is global and will be freed after.
        //    layout->descriptor_set_layout_handles[ l ] = k_invalid_layout;
        //    vk_layouts[ l ] = s->vk_descriptor_set_layout;
        //    continue;
        //}

        // Access layout to cache the Vulkan layout.
        DescriptorSetLayout* descriptor_set_layout = get_descriptor_set_layout( creation.layouts[ l ] );

        // Cache descriptor set layout handle if not the bindless one
        if ( descriptor_set_layout->bindless ) {
            // Avoid deletion of this set as it is global and will be freed after.
            layout->descriptor_set_layout_handles[ l ] = {};
        }
        else {
            layout->descriptor_set_layout_handles[ l ] = creation.layouts[ l ];
        }

        vk_layouts[ l ] = descriptor_set_layout->vk_descriptor_set_layout;
    }

    layout->handle = handle;
    layout->num_active_layouts = num_active_layouts;
    layout->push_constant = creation.push_constant;

    VkPipelineLayoutCreateInfo pipeline_layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pipeline_layout_info.pSetLayouts = vk_layouts;
    pipeline_layout_info.setLayoutCount = num_active_layouts;
    pipeline_layout_info.pushConstantRangeCount = 0;

    // Push constants
    if ( creation.push_constant.size > 0 ) {
        pipeline_layout_info.pPushConstantRanges = &creation.push_constant;
        pipeline_layout_info.pushConstantRangeCount = 1;
    }

    check( vkCreatePipelineLayout( vulkan_device, &pipeline_layout_info, vulkan_allocation_callbacks, &layout->vk_pipeline_layout ) );

    set_resource_name( VK_OBJECT_TYPE_PIPELINE_LAYOUT, (u64)layout->vk_pipeline_layout, creation.name );

    return handle;
}

// BLAS/TLAS helpers /////////////////////////////////////////////////////
struct AccelerationStructureScratch {
    BufferHandle buffer;
    VkDeviceAddress address = 0;
};

static VkDeviceAddress align_device_address( VkDeviceAddress address, VkDeviceSize alignment ) {
    RASSERT( alignment > 0 );

    const VkDeviceAddress remainder = address % alignment;
    return remainder ? address + alignment - remainder : address;
}

static AccelerationStructureScratch create_acceleration_structure_scratch( GpuDevice& gpu, VkDeviceSize required_size,
                                                                           VkDeviceSize alignment, cstring name ) {
    AccelerationStructureScratch scratch{};

    RASSERT( required_size > 0 );
    RASSERT( alignment > 0 );

    const VkDeviceSize buffer_size = required_size + alignment - 1;

    scratch.buffer = gpu.create_buffer( {
        .size = buffer_size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .name = name } );

    if ( scratch.buffer.is_invalid() ) {
        return scratch;
    }

    const VkDeviceAddress base_address = gpu.get_buffer_device_address( scratch.buffer );
    scratch.address = align_device_address( base_address, alignment );

    RASSERT( base_address != 0 );
    RASSERT( scratch.address % alignment == 0 );
    RASSERT( scratch.address >= base_address );
    RASSERT( scratch.address + required_size <= base_address + buffer_size );

    return scratch;
}

BLASHandle GpuDevice::create_blas( const BLASCreation& creation ) {

    if ( !ray_tracing_present ) {
        rprint( "BLAS creation failed: ray tracing not supported or not enabled on this device\n" );
        return {};
    }

    BLASHandle handle = blases.obtain();
    if ( handle.is_invalid() ) {
        return handle;
    }

    BLASBuildInfo& build_info = blas_build_requests.push_use();
    build_info.geometries.init( allocator, ( u32 )creation.geometries.size );
    build_info.ranges.init( allocator, ( u32 )creation.geometries.size );
    build_info.build_info = {};
    build_info.build_sizes = {};

    for ( u32 i = 0; i < creation.geometries.size; ++i ) {

        const BLASGeometry& blas_geometry = creation.geometries[ i ];

        VkAccelerationStructureGeometryKHR& geometry = build_info.geometries.push_use();
        geometry = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
        geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geometry.flags = blas_geometry.opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;

        geometry.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        geometry.geometry.triangles.vertexData.deviceAddress = get_buffer_device_address( blas_geometry.vertex_buffer ) + blas_geometry.vertex_buffer_offset;
        geometry.geometry.triangles.vertexStride = blas_geometry.vertex_stride;
        geometry.geometry.triangles.maxVertex = blas_geometry.max_vertex;
        geometry.geometry.triangles.indexType = blas_geometry.index_type;
        geometry.geometry.triangles.indexData.deviceAddress = get_buffer_device_address( blas_geometry.index_buffer );
        geometry.geometry.triangles.transformData.deviceAddress = blas_geometry.transform_buffer.is_valid() ? get_buffer_device_address(blas_geometry.transform_buffer) : 0;

        VkAccelerationStructureBuildRangeInfoKHR& build_range_info = build_info.ranges.push_use();
        build_range_info.primitiveCount = blas_geometry.primitive_count;
        build_range_info.primitiveOffset = blas_geometry.index_buffer_offset;
        build_range_info.transformOffset = blas_geometry.transform_buffer_offset;
        build_range_info.firstVertex = 0;
    }

    VkAccelerationStructureBuildGeometryInfoKHR& blas_build_info = build_info.build_info;
    blas_build_info = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    blas_build_info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    blas_build_info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
    blas_build_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    blas_build_info.geometryCount = build_info.geometries.size;
    blas_build_info.pGeometries = build_info.geometries.data;

    ArenaAllocator* temp_allocator = MemoryService::instance()->get_thread_allocator();
    sizet base_marker = temp_allocator->get_marker();

    Array<u32> max_primitives_count;
    max_primitives_count.init( temp_allocator, build_info.geometries.size, build_info.geometries.size );

    for ( u32 range_index = 0; range_index < build_info.geometries.size; range_index++ ) {
        max_primitives_count[ range_index ] = build_info.ranges[ range_index ].primitiveCount;
    }

    VkAccelerationStructureBuildSizesInfoKHR& blas_build_size_info = build_info.build_sizes;
    blas_build_size_info = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    vkGetAccelerationStructureBuildSizesKHR( vulkan_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &blas_build_info, max_primitives_count.data, &blas_build_size_info );

    temp_allocator->free_marker( base_marker );

    VkBufferUsageFlags blas_buffer_usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    BufferHandle blas_buffer = create_buffer( {
        .size = blas_build_size_info.accelerationStructureSize,
        .usage = blas_buffer_usage,
        .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .name = "blas_buffer" } );

    if ( blas_buffer.is_invalid() ) {
        blases.destroy( handle );
        return {};
    }

    Buffer* v_blas_buffer = get_buffer( blas_buffer );
    RASSERT( v_blas_buffer );
    RASSERT( v_blas_buffer->vk_buffer != VK_NULL_HANDLE );

    const VkDeviceSize scratch_alignment = 128;// acceleration_structure_properties.minAccelerationStructureScratchOffsetAlignment;

    const AccelerationStructureScratch blas_scratch = create_acceleration_structure_scratch( *this, blas_build_size_info.buildScratchSize,
                                                                                             scratch_alignment, "blas_scratch_buffer" );

    if ( blas_scratch.buffer.is_invalid() ) {
        destroy_buffer( blas_buffer );
        blases.destroy( handle );
        return {};
    }

    VkAccelerationStructureCreateInfoKHR blas_create_info{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR
    };
    blas_create_info.buffer = v_blas_buffer->vk_buffer;
    blas_create_info.offset = 0;
    blas_create_info.size = blas_build_size_info.accelerationStructureSize;
    blas_create_info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;

    BLAS* blas = get_blas( handle );

    const VkResult result = vkCreateAccelerationStructureKHR( vulkan_device, &blas_create_info, vulkan_allocation_callbacks, &blas->as );

    if ( result != VK_SUCCESS ) {
        rprint( "Failed to create BLAS acceleration structure: %d\n", result );

        destroy_buffer( blas_scratch.buffer );
        destroy_buffer( blas_buffer );
        blases.destroy( handle );
        return {};
    }

    blas_build_info.dstAccelerationStructure = blas->as;
    blas_build_info.scratchData.deviceAddress = blas_scratch.address;

    RASSERT( blas_build_info.scratchData.deviceAddress % scratch_alignment == 0 );

    // Cache information.
    blas->blas_buffer = blas_buffer;
    blas->scratch_buffer = blas_scratch.buffer;
    blas->handle = handle;
    blas->name = creation.name;

    return handle;
}
TLASHandle GpuDevice::create_tlas( const TLASCreation& creation ) {

    if ( !ray_tracing_present ) {
        rprint( "TLAS creation failed: ray tracing not supported or not enabled on this device\n" );
        return {};
    }

    TLASHandle handle = tlases.obtain();
    if ( handle.is_invalid() ) {
        return handle;
    }

    ArenaAllocator* temp_allocator = MemoryService::instance()->get_thread_allocator();
    const sizet base_marker = temp_allocator->get_marker();

    VkAccelerationStructureDeviceAddressInfoKHR blas_address_info{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR
    };

    Array<VkAccelerationStructureInstanceKHR> instances;
    instances.init( temp_allocator, ( u32 )creation.instances.size );

    for ( u32 i = 0; i < creation.instances.size; ++i ) {
        const TLASGeometryInstance& source_instance = creation.instances[ i ];
        BLAS* blas = get_blas( source_instance.blas );
        RASSERT( blas );
        RASSERT( blas->as != VK_NULL_HANDLE );

        blas_address_info.accelerationStructure = blas->as;
        const VkDeviceAddress blas_address = vkGetAccelerationStructureDeviceAddressKHR( vulkan_device, &blas_address_info );

        VkAccelerationStructureInstanceKHR& tlas_instance = instances.push_use();
        tlas_instance = {};
        tlas_instance.transform = source_instance.transform;
        tlas_instance.instanceCustomIndex = source_instance.instance_custom_index;
        tlas_instance.instanceShaderBindingTableRecordOffset = source_instance.sbt_record_offset;
        tlas_instance.mask = source_instance.mask;
        tlas_instance.flags = source_instance.flags;
        tlas_instance.accelerationStructureReference = blas_address;
    }

    const VkDeviceSize instance_buffer_size = sizeof( VkAccelerationStructureInstanceKHR ) * creation.instances.size;

    BufferHandle tlas_instance_buffer_handle = create_buffer( {
        .size = instance_buffer_size,
        .usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                            VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .name = "tlas_instance_buffer" } );

    if ( tlas_instance_buffer_handle.is_invalid() ) {
        temp_allocator->free_marker( base_marker );
        tlases.destroy( handle );
        return {};
    }

    Buffer* tlas_instance_buffer = get_buffer( tlas_instance_buffer_handle );
    RASSERT( tlas_instance_buffer );
    RASSERT( tlas_instance_buffer->mapped_data );

    memcpy( tlas_instance_buffer->mapped_data, instances.data, instance_buffer_size );
    flush_buffer( tlas_instance_buffer_handle, 0, instance_buffer_size );

    const VkDeviceAddress instance_buffer_address = get_buffer_device_address( tlas_instance_buffer_handle );
    RASSERT( instance_buffer_address != 0 );
    RASSERT( instance_buffer_address % 16 == 0 );

    temp_allocator->free_marker( base_marker );

    // Cache build request.
    TLASBuildInfo& build_info = tlas_build_requests.push_use();

    VkAccelerationStructureGeometryKHR& tlas_geometry = build_info.geometry;
    tlas_geometry = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    tlas_geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlas_geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    tlas_geometry.geometry.instances.arrayOfPointers = VK_FALSE;
    tlas_geometry.geometry.instances.data.deviceAddress = instance_buffer_address;

    VkAccelerationStructureBuildGeometryInfoKHR& tlas_build_info = build_info.build_info;
    tlas_build_info = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    tlas_build_info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlas_build_info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tlas_build_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tlas_build_info.geometryCount = 1;
    tlas_build_info.pGeometries = &tlas_geometry;

    const u32 max_instance_count = ( u32 )creation.instances.size;

    VkAccelerationStructureBuildSizesInfoKHR tlas_build_size_info {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR
    };

    vkGetAccelerationStructureBuildSizesKHR( vulkan_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                             &tlas_build_info, &max_instance_count, &tlas_build_size_info );

    BufferHandle tlas_buffer = create_buffer( {
        .size = tlas_build_size_info.accelerationStructureSize,
        .usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .name = "tlas_buffer" } );

    if ( tlas_buffer.is_invalid() ) {
        destroy_buffer( tlas_instance_buffer_handle );
        tlases.destroy( handle );
        return {};
    }

    Buffer* vk_tlas_buffer = get_buffer( tlas_buffer );
    RASSERT( vk_tlas_buffer );
    RASSERT( vk_tlas_buffer->vk_buffer != VK_NULL_HANDLE );

    const VkDeviceSize scratch_alignment = 128;//acceleration_structure_properties.minAccelerationStructureScratchOffsetAlignment;

    const AccelerationStructureScratch tlas_scratch = create_acceleration_structure_scratch( *this, tlas_build_size_info.buildScratchSize,
                                                                                             scratch_alignment, "tlas_scratch_buffer" );

    if ( tlas_scratch.buffer.is_invalid() ) {
        destroy_buffer( tlas_buffer );
        destroy_buffer( tlas_instance_buffer_handle );
        tlases.destroy( handle );
        return {};
    }

    VkAccelerationStructureCreateInfoKHR tlas_create_info{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR
    };
    tlas_create_info.buffer = vk_tlas_buffer->vk_buffer;
    tlas_create_info.offset = 0;
    tlas_create_info.size = tlas_build_size_info.accelerationStructureSize;
    tlas_create_info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;

    TLAS* tlas = get_tlas( handle );

    const VkResult result = vkCreateAccelerationStructureKHR(
        vulkan_device,
        &tlas_create_info,
        vulkan_allocation_callbacks,
        &tlas->as
    );

    if ( result != VK_SUCCESS ) {
        rprint( "Failed to create TLAS acceleration structure: %d\n", result );

        destroy_buffer( tlas_scratch.buffer );
        destroy_buffer( tlas_buffer );
        destroy_buffer( tlas_instance_buffer_handle );
        tlases.destroy( handle );
        return {};
    }

    tlas_build_info.dstAccelerationStructure = tlas->as;
    tlas_build_info.scratchData.deviceAddress = tlas_scratch.address;

    RASSERT( tlas_build_info.scratchData.deviceAddress % scratch_alignment == 0 );

    VkAccelerationStructureBuildRangeInfoKHR& tlas_range_info = build_info.range;
    tlas_range_info = {};
    tlas_range_info.primitiveCount = ( u32 )creation.instances.size;

    // Cache information.
    tlas->scratch_buffer = tlas_scratch.buffer;
    tlas->tlas_buffer = tlas_buffer;
    tlas->instance_buffer = tlas_instance_buffer_handle;
    tlas->handle = handle;
    tlas->name = creation.name;

    return handle;
}
ShaderStateHandle GpuDevice::create_shader_state( const ShaderStateCreation& creation ) {

    ShaderStateHandle handle = {};

    if ( creation.stages.size == 0 ) {
        rprint( "Shader %s does not contain shader stages.\n", creation.name );
        return handle;
    }

    handle = shader_states.obtain();
    if ( handle.is_invalid() ) {
        return handle;
    }

    resource_tracker.track_create_resource( ResourceUpdateType::ShaderState, handle.index(), creation.name );

    // For each shader stage, compile them individually.
    u32 compiled_shaders = 0;

    ShaderState* shader_state = get_shader_state( handle );
    shader_state->type = VK_PIPELINE_BIND_POINT_GRAPHICS;
    shader_state->active_shaders = 0;

    u32 broken_stage = u32_max;

    for ( compiled_shaders = 0; compiled_shaders < creation.stages.size; ++compiled_shaders ) {
        VkShaderStageFlagBits stage_type = creation.stage_types[ compiled_shaders ];

        // Gives priority to compute: if any is present (and it should not be) then it is not a graphics pipeline.
        switch ( stage_type ) {

            case VK_SHADER_STAGE_COMPUTE_BIT:
            {
                shader_state->type = VK_PIPELINE_BIND_POINT_COMPUTE;
                break;
            }

            case VK_SHADER_STAGE_RAYGEN_BIT_KHR:
            case VK_SHADER_STAGE_ANY_HIT_BIT_KHR:
            case VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR:
            case VK_SHADER_STAGE_MISS_BIT_KHR:
            case VK_SHADER_STAGE_INTERSECTION_BIT_KHR:
            case VK_SHADER_STAGE_CALLABLE_BIT_KHR:
            {
                shader_state->type = VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR;
                break;
            }
        }

        const VkShaderModuleCreateInfo& shader_create_info = creation.stages[ compiled_shaders ];

        // Spir-V file is not generated when there is a compilation error, we can use this to know when compilation is succeded.
        if ( shader_create_info.pCode ) {

            shader_state->vk_shader_stages[ compiled_shaders ] = stage_type;

            if ( vkCreateShaderModule( vulkan_device, &shader_create_info, nullptr, &shader_state->vk_shader_modules[ compiled_shaders ] ) != VK_SUCCESS ) {
                broken_stage = compiled_shaders;
            }
        } else {
            broken_stage = compiled_shaders;
        }

        if ( broken_stage != u32_max ) {
            break;
        }

        set_resource_name( VK_OBJECT_TYPE_SHADER_MODULE, (u64)shader_state->vk_shader_modules[ compiled_shaders ], creation.name );
    }

    bool creation_failed = compiled_shaders != creation.stages.size;
    if ( !creation_failed ) {
        shader_state->active_shaders = compiled_shaders;
        shader_state->name = creation.name;
    }

    if ( creation_failed ) {
        destroy_shader_state( handle );
        handle = {};
    }

    return handle;
}

void GpuDevice::generate_pipeline_binaries( void* pipeline_info, Pipeline* pipeline, cstring cache_path, ArenaScope& temp_stack ) {
    VkPipelineCreateInfoKHR pipeline_create_info{ VK_STRUCTURE_TYPE_PIPELINE_CREATE_INFO_KHR };
    pipeline_create_info.pNext = pipeline_info;
    VkPipelineBinaryKeyKHR pipeline_key{ VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR };

    check( vkGetPipelineKeyKHR( vulkan_device, &pipeline_create_info, &pipeline_key ) );

    VkPipelineBinaryHandlesInfoKHR binaries{ VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR };

    VkPipelineBinaryCreateInfoKHR pipeline_binary_create_info{ VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR };
    pipeline_binary_create_info.pipeline = pipeline->vk_pipeline;

    check( vkCreatePipelineBinariesKHR( vulkan_device, &pipeline_binary_create_info, vulkan_allocation_callbacks, &binaries ) );

    Array<VkPipelineBinaryKHR> pipeline_binaries;
    pipeline_binaries.init( temp_stack.allocator, binaries.pipelineBinaryCount, binaries.pipelineBinaryCount );
    binaries.pPipelineBinaries = pipeline_binaries.data;

    check( vkCreatePipelineBinariesKHR( vulkan_device, &pipeline_binary_create_info, vulkan_allocation_callbacks, &binaries ) );

    FileHandle cache_handle{ };
    file_open( cache_path, "wb", &cache_handle );

    file_write( &global_binary_key.keySize, sizeof( u32 ), 1, cache_handle );
    file_write( &global_binary_key.key, global_binary_key.keySize, 1, cache_handle );

    file_write( &pipeline_key.keySize, sizeof( u32 ), 1, cache_handle );
    file_write( &pipeline_key.key, pipeline_key.keySize, 1, cache_handle );

    file_write( &binaries.pipelineBinaryCount, sizeof( u32 ), 1, cache_handle );

    Array<VkPipelineBinaryKeyKHR> binary_keys;
    binary_keys.init( temp_stack.allocator, binaries.pipelineBinaryCount, binaries.pipelineBinaryCount );
    memset( binary_keys.data, 0, sizeof( VkPipelineBinaryKeyKHR ) * binaries.pipelineBinaryCount );

    for ( u32 i = 0; i < binaries.pipelineBinaryCount; ++i ) {
        Array<u8> pipeline_binary_data;

        sizet pipeline_binary_size = 0;

        binary_keys[ i ].sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR;

        VkPipelineBinaryDataInfoKHR pipeline_binary_data_info{ VK_STRUCTURE_TYPE_PIPELINE_BINARY_DATA_INFO_KHR };
        pipeline_binary_data_info.pipelineBinary = binaries.pPipelineBinaries[ i ];
        check( vkGetPipelineBinaryDataKHR( vulkan_device, &pipeline_binary_data_info, &binary_keys[ i ], &pipeline_binary_size, nullptr ) );

        pipeline_binary_data.init( temp_stack.allocator, ( u32 )pipeline_binary_size, ( u32 )pipeline_binary_size );
        check( vkGetPipelineBinaryDataKHR( vulkan_device, &pipeline_binary_data_info, &binary_keys[ i ], &pipeline_binary_size, pipeline_binary_data.data ) );

        file_write( &binary_keys[ i ].keySize, sizeof( u32 ), 1, cache_handle );
        file_write( binary_keys[ i ].key, binary_keys[ i ].keySize, 1, cache_handle );

        file_write( &pipeline_binary_size, sizeof( u32 ), 1, cache_handle );
        file_write( pipeline_binary_data.data, ( u32 )pipeline_binary_size, 1, cache_handle );
    }

    file_close( cache_handle );

    VkReleaseCapturedPipelineDataInfoKHR release_info{ VK_STRUCTURE_TYPE_RELEASE_CAPTURED_PIPELINE_DATA_INFO_KHR };
    release_info.pipeline = pipeline->vk_pipeline;
    check( vkReleaseCapturedPipelineDataKHR( vulkan_device, &release_info, vulkan_allocation_callbacks ) );
}

bool GpuDevice::restore_pipeline_binaries( cstring cache_path, ArenaScope& temp_stack, Array<VkPipelineBinaryKHR>& pipeline_binaries, VkPipelineBinaryInfoKHR& pipeline_binary_info ) {
    // Load pipeline binary from disk
    FileReadResult read_result = file_read_binary( cache_path, temp_stack.allocator );
    char* binary_data = read_result.data;

    VkPipelineBinaryKeyKHR cache_key{ VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR };
    cache_key.keySize = *( ( u32* )binary_data );
    binary_data += sizeof( u32 );

    memcpy( cache_key.key, binary_data, cache_key.keySize );
    binary_data += cache_key.keySize;

    if ( cache_key.keySize != global_binary_key.keySize ||
         memcmp( cache_key.key, global_binary_key.key, cache_key.keySize ) != 0 ) {
        // Cache is no longer valid
        return false;
    }

    VkPipelineBinaryKeyKHR pipeline_key{ VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR };
    pipeline_key.keySize = *( ( u32* )binary_data );
    binary_data += sizeof( u32 );

    memcpy( pipeline_key.key, binary_data, pipeline_key.keySize );
    binary_data += pipeline_key.keySize;

    u32 binary_count = *( ( u32* )binary_data );
    binary_data += sizeof( u32 );

    Array<VkPipelineBinaryDataKHR> pipeline_binary_data;
    Array<VkPipelineBinaryKeyKHR> pipeline_binary_keys;
    pipeline_binary_data.init( temp_stack.allocator, binary_count );
    pipeline_binary_keys.init( temp_stack.allocator, binary_count );

    for ( u32 i = 0; i < binary_count; ++i ) {
        VkPipelineBinaryKeyKHR binary_key{ VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR };
        binary_key.keySize = *( ( u32* )binary_data );
        binary_data += sizeof( u32 );

        memcpy( binary_key.key, binary_data, binary_key.keySize );
        binary_data += binary_key.keySize;

        u32 binary_size = *( ( u32* )binary_data );
        binary_data += sizeof( u32 );

        VkPipelineBinaryDataKHR binary{
            .dataSize = binary_size,
            .pData = binary_data
        };
        pipeline_binary_data.push( binary );
        pipeline_binary_keys.push( binary_key );

        binary_data += binary_size;
    }

    pipeline_binaries.init( temp_stack.allocator, binary_count, binary_count );
    VkPipelineBinaryHandlesInfoKHR binary_handles{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR,
        .pipelineBinaryCount = binary_count,
        .pPipelineBinaries = pipeline_binaries.data
    };

    VkPipelineBinaryCreateInfoKHR pipeline_binary_create_info{ VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR };
    VkPipelineBinaryKeysAndDataKHR keys_and_data{
        .binaryCount = binary_count,
        .pPipelineBinaryKeys = pipeline_binary_keys.data,
        .pPipelineBinaryData = pipeline_binary_data.data
    };

    pipeline_binary_create_info.pKeysAndDataInfo = &keys_and_data;

    check( vkCreatePipelineBinariesKHR( vulkan_device, &pipeline_binary_create_info, vulkan_allocation_callbacks, &binary_handles ) );

    pipeline_binary_info.binaryCount = binary_count;
    pipeline_binary_info.pPipelineBinaries = pipeline_binaries.data;

    return true;
}

PipelineHandle GpuDevice::create_pipeline( const PipelineCreation& creation, const char* cache_path ) {

    ShaderStateHandle shader_state = creation.shader;
    if ( shader_state.is_invalid() ) {
        // Shader did not compile.
        rprint( "Cannot create pipeline %s: shader state is invalid.\n", creation.name );
        return {};
    }
    PipelineHandle handle = pipelines.obtain();
    if ( handle.is_invalid() ) {
        return handle;
    }

    resource_tracker.track_create_resource( ResourceUpdateType::Pipeline, handle.index(), creation.name );

    VkPipelineCache pipeline_cache = VK_NULL_HANDLE;
    VkPipelineCacheCreateInfo pipeline_cache_create_info{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };

    bool cache_exists = file_exists( cache_path );

    // VkPipelineCache cannot be used when using pipeline binaries
    if ( !pipeline_binary_present ) {
        if ( cache_path != nullptr && cache_exists ) {
            FileReadResult read_result = file_read_binary( cache_path, allocator );

            VkPipelineCacheHeaderVersionOne* cache_header = (VkPipelineCacheHeaderVersionOne*)read_result.data;

            if ( cache_header->deviceID == vulkan_physical_properties.deviceID &&
                cache_header->vendorID == vulkan_physical_properties.vendorID &&
                memcmp( cache_header->pipelineCacheUUID, vulkan_physical_properties.pipelineCacheUUID, VK_UUID_SIZE ) == 0 ) {
                pipeline_cache_create_info.initialDataSize = read_result.size;
                pipeline_cache_create_info.pInitialData = read_result.data;
            } else {
                cache_exists = false;
            }

            check( vkCreatePipelineCache( vulkan_device, &pipeline_cache_create_info, vulkan_allocation_callbacks, &pipeline_cache ) );

            allocator->deallocate( read_result.data );
        } else {
            check( vkCreatePipelineCache( vulkan_device, &pipeline_cache_create_info, vulkan_allocation_callbacks, &pipeline_cache ) );
        }
    }

    // Now that shaders have compiled we can create the pipeline.
    Pipeline* pipeline = get_pipeline( handle );
    ShaderState* shader_state_data = get_shader_state( shader_state );

    pipeline->shader_state = shader_state;

    ArenaAllocator* temp_allocator = MemoryService::instance()->get_thread_allocator();
    ArenaScope temp_stack( temp_allocator );

    Array<VkPipelineShaderStageCreateInfo> shader_stage_info_array;
    shader_stage_info_array.init( temp_stack.allocator, shader_state_data->active_shaders );

    // Init
    for ( u32 i = 0; i < shader_state_data->active_shaders; ++i ) {
        VkPipelineShaderStageCreateInfo& shader_stage_info = shader_stage_info_array.push_use();
        memset( &shader_stage_info, 0, sizeof( VkPipelineShaderStageCreateInfo ) );
        shader_stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shader_stage_info.pName = "main";
        shader_stage_info.stage = shader_state_data->vk_shader_stages[ i ];
        shader_stage_info.module = shader_state_data->vk_shader_modules[ i ];
    }

    // Specialization constants
    VkSpecializationInfo specialization_info;
    if ( creation.num_specialization_constants ) {

        specialization_info.mapEntryCount = creation.num_specialization_constants;
        // NOTE: we assume specialization constants to either be i32,u32 or floats.
        specialization_info.dataSize = creation.num_specialization_constants * sizeof( u32 );
        specialization_info.pMapEntries = creation.specialization_entries;
        specialization_info.pData = creation.specialization_data;

        for ( u32 i = 0; i < shader_state_data->active_shaders; ++i ) {
            shader_stage_info_array[ i ].pSpecializationInfo = &specialization_info;
        }
    }

    // Cache pipeline layout
    PipelineLayout* pipeline_layout = get_pipeline_layout( creation.layout );
    pipeline->layout = creation.layout;
    pipeline->cached_vk_layout = pipeline_layout->vk_pipeline_layout;

    // Create full pipeline
    if ( shader_state_data->type == VK_PIPELINE_BIND_POINT_GRAPHICS ) {
        VkGraphicsPipelineCreateInfo pipeline_info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };

        pipeline_info.flags = creation.flags;

        //// Shader stage
        pipeline_info.pStages = shader_stage_info_array.data;
        pipeline_info.stageCount = shader_stage_info_array.size;
        //// PipelineLayout
        pipeline_info.layout = pipeline->cached_vk_layout;

        //// Vertex input
        VkPipelineVertexInputStateCreateInfo vertex_input_info = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vertex_input_info.vertexAttributeDescriptionCount = creation.vertex_input.attributes.size;
        vertex_input_info.pVertexAttributeDescriptions = creation.vertex_input.attributes.data;

        vertex_input_info.vertexBindingDescriptionCount = creation.vertex_input.bindings.size;
        vertex_input_info.pVertexBindingDescriptions = creation.vertex_input.bindings.data;

        pipeline_info.pVertexInputState = &vertex_input_info;

        //// Input Assembly
        VkPipelineInputAssemblyStateCreateInfo input_assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        input_assembly.topology = creation.topology;
        input_assembly.primitiveRestartEnable = VK_FALSE;

        pipeline_info.pInputAssemblyState = &input_assembly;

        //// Color Blending
        VkPipelineColorBlendAttachmentState color_blend_attachment[ 8 ];

        if ( creation.blend_state.blend_states.size ) {
            RASSERTM( creation.blend_state.blend_states.size == creation.render_pass_output.num_color_formats,
                "Blend states (count: %u) mismatch with output targets (count %u)! If blend states are active, they must be defined for all outputs",
                creation.blend_state.blend_states.size, creation.render_pass_output.num_color_formats );
            for ( u32 i = 0; i < creation.blend_state.blend_states.size; i++ ) {
                const BlendState& blend_state = creation.blend_state.blend_states[ i ];

                color_blend_attachment[ i ].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
                color_blend_attachment[ i ].blendEnable = blend_state.blend_disabled ? VK_FALSE : VK_TRUE;
                color_blend_attachment[ i ].srcColorBlendFactor = blend_state.source_color;
                color_blend_attachment[ i ].dstColorBlendFactor = blend_state.destination_color;
                color_blend_attachment[ i ].colorBlendOp = blend_state.color_operation;

                if ( blend_state.separate_blend ) {
                    color_blend_attachment[ i ].srcAlphaBlendFactor = blend_state.source_alpha;
                    color_blend_attachment[ i ].dstAlphaBlendFactor = blend_state.destination_alpha;
                    color_blend_attachment[ i ].alphaBlendOp = blend_state.alpha_operation;
                } else {
                    color_blend_attachment[ i ].srcAlphaBlendFactor = blend_state.source_color;
                    color_blend_attachment[ i ].dstAlphaBlendFactor = blend_state.destination_color;
                    color_blend_attachment[ i ].alphaBlendOp = blend_state.color_operation;
                }
            }
        } else {
            // Default non blended state
            for ( u32 i = 0; i < creation.render_pass_output.num_color_formats; ++i ) {
                color_blend_attachment[ i ] = {};
                color_blend_attachment[ i ].blendEnable = VK_FALSE;
                color_blend_attachment[ i ].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            }
        }

        VkPipelineColorBlendStateCreateInfo color_blending{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        color_blending.logicOpEnable = VK_FALSE;
        color_blending.logicOp = VK_LOGIC_OP_COPY; // Optional
        color_blending.attachmentCount = creation.blend_state.blend_states.size ? creation.blend_state.blend_states.size : creation.render_pass_output.num_color_formats;
        color_blending.pAttachments = color_blend_attachment;
        color_blending.blendConstants[ 0 ] = 0.0f; // Optional
        color_blending.blendConstants[ 1 ] = 0.0f; // Optional
        color_blending.blendConstants[ 2 ] = 0.0f; // Optional
        color_blending.blendConstants[ 3 ] = 0.0f; // Optional

        pipeline_info.pColorBlendState = &color_blending;

        //// Depth Stencil
        VkPipelineDepthStencilStateCreateInfo depth_stencil{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };

        depth_stencil.depthWriteEnable = creation.depth_stencil.depth_write_enable ? VK_TRUE : VK_FALSE;
        depth_stencil.stencilTestEnable = creation.depth_stencil.stencil_enable ? VK_TRUE : VK_FALSE;
        depth_stencil.depthTestEnable = creation.depth_stencil.depth_enable ? VK_TRUE : VK_FALSE;
        depth_stencil.depthCompareOp = creation.depth_stencil.depth_comparison;
        if ( creation.depth_stencil.stencil_enable ) {
            // TODO: add stencil
            RASSERT( false );
        }

        pipeline_info.pDepthStencilState = &depth_stencil;

        //// Multisample
        VkPipelineMultisampleStateCreateInfo multisampling = {};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.sampleShadingEnable = VK_FALSE;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        multisampling.minSampleShading = 1.0f; // Optional
        multisampling.pSampleMask = nullptr; // Optional
        multisampling.alphaToCoverageEnable = VK_FALSE; // Optional
        multisampling.alphaToOneEnable = VK_FALSE; // Optional

        pipeline_info.pMultisampleState = &multisampling;

        //// Rasterizer
        VkPipelineRasterizationStateCreateInfo rasterizer{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rasterizer.depthClampEnable = VK_FALSE;
        rasterizer.rasterizerDiscardEnable = VK_FALSE;
        rasterizer.polygonMode = creation.rasterization.fill;
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = creation.rasterization.cull_mode;
        rasterizer.frontFace = creation.rasterization.front;
        rasterizer.depthBiasEnable = VK_FALSE;
        rasterizer.depthBiasConstantFactor = 0.0f; // Optional
        rasterizer.depthBiasClamp = 0.0f; // Optional
        rasterizer.depthBiasSlopeFactor = 0.0f; // Optional

        pipeline_info.pRasterizationState = &rasterizer;

        //// Tessellation
        pipeline_info.pTessellationState;


        //// Viewport state
        VkViewport viewport = {};
        viewport.x = 0.0f;
        viewport.y = (float)swapchain_height;
        viewport.width = (float)swapchain_width;
        viewport.height = -(float)swapchain_height;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;

        VkRect2D scissor = {};
        scissor.offset = { 0, 0 };
        scissor.extent = { swapchain_width, swapchain_height };

        VkPipelineViewportStateCreateInfo viewport_state{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewport_state.viewportCount = 1;
        viewport_state.pViewports = &viewport;
        viewport_state.scissorCount = 1;
        viewport_state.pScissors = &scissor;

        pipeline_info.pViewportState = &viewport_state;

        //// Render Pass
        VkPipelineRenderingCreateInfoKHR pipeline_rendering_create_info{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
        pipeline_rendering_create_info.viewMask = 0;
        pipeline_rendering_create_info.colorAttachmentCount = creation.render_pass_output.num_color_formats;
        pipeline_rendering_create_info.pColorAttachmentFormats = creation.render_pass_output.num_color_formats > 0 ? creation.render_pass_output.color_formats : nullptr;
        pipeline_rendering_create_info.depthAttachmentFormat = creation.render_pass_output.depth_stencil_format;
        pipeline_rendering_create_info.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;

        pipeline_info.pNext = &pipeline_rendering_create_info;

        //// Dynamic states
        VkPipelineDynamicStateCreateInfo dynamic_state{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };

        StaticArray<VkDynamicState, 5> dynamic_states;
        dynamic_states.push( VK_DYNAMIC_STATE_VIEWPORT );
        dynamic_states.push( VK_DYNAMIC_STATE_SCISSOR );
        dynamic_states.push( VK_DYNAMIC_STATE_DEPTH_BIAS );
        dynamic_states.push( VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE );

        if ( fragment_shading_rate_present ) {
            dynamic_states.push( VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR );
        }

        dynamic_state.dynamicStateCount = ( u32 )dynamic_states.size;
        dynamic_state.pDynamicStates = dynamic_states.data;

        pipeline_info.pDynamicState = &dynamic_state;

        if ( pipeline_binary_present ) {
            bool valid_cache = false;
            if ( cache_path != nullptr && cache_exists ) {
                Array<VkPipelineBinaryKHR> pipeline_binaries;
                VkPipelineBinaryInfoKHR pipeline_binary_info{ VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR };
                if ( restore_pipeline_binaries( cache_path, temp_stack, pipeline_binaries, pipeline_binary_info ) ) {
                    pipeline_binary_info.pNext = pipeline_info.pNext;
                    pipeline_info.pNext = &pipeline_binary_info;

                    VkResult result = vkCreateGraphicsPipelines( vulkan_device, VK_NULL_HANDLE, 1, &pipeline_info, vulkan_allocation_callbacks, &pipeline->vk_pipeline );
                    if ( result == VK_SUCCESS ) {
                        // If result is VK_PIPELINE_COMPILE_REQUIRED_EXT, it means the pipeline binaries are no longer valid
                        // and we need to recompile the pipeline
                        for ( u32 i = 0; i < pipeline_binaries.size; ++i ) {
                            vkDestroyPipelineBinaryKHR( vulkan_device, pipeline_binaries[ i ], vulkan_allocation_callbacks);
                        }

                        valid_cache = true;
                    }
                }
            }

            if ( !valid_cache ) {
                VkPipelineCreateFlags2CreateInfo flags2{ VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO };
                flags2.flags = VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR;

                flags2.pNext = pipeline_info.pNext;
                pipeline_info.pNext = &flags2;

                check( vkCreateGraphicsPipelines( vulkan_device, VK_NULL_HANDLE, 1, &pipeline_info, vulkan_allocation_callbacks, &pipeline->vk_pipeline ) );

                if ( cache_path != nullptr ) {
                    generate_pipeline_binaries( &pipeline_info, pipeline, cache_path, temp_stack );
                }
            }
        } else {
            check( vkCreateGraphicsPipelines( vulkan_device, pipeline_cache, 1, &pipeline_info, vulkan_allocation_callbacks, &pipeline->vk_pipeline ) );
        }

        pipeline->vk_bind_point = VkPipelineBindPoint::VK_PIPELINE_BIND_POINT_GRAPHICS;
    } else if ( shader_state_data->type == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR ) {

        // Shader state order are different from ray tracing shader group order, thus search them
        u32 raygen_stage_index = u32_max, miss_stage_index = u32_max, hit_stage_index = u32_max;

        for ( u32 i = 0; i < shader_state_data->active_shaders; ++i ) {
            switch ( shader_state_data->vk_shader_stages[ i ] ) {
                case VK_SHADER_STAGE_RAYGEN_BIT_KHR:
                {
                    raygen_stage_index = i;
                    break;
                }
                case VK_SHADER_STAGE_ANY_HIT_BIT_KHR:
                case VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR:
                {
                    hit_stage_index = i;
                    break;
                }
                case VK_SHADER_STAGE_MISS_BIT_KHR:
                {
                    miss_stage_index = i;
                    break;
                }
            }
        }

        RASSERT( raygen_stage_index != u32_max );
        RASSERT( miss_stage_index != u32_max );
        RASSERT( hit_stage_index != u32_max );

        // For ray-traced pipeline, we need shader group info.
        Array<VkRayTracingShaderGroupCreateInfoKHR> shader_group_info_array;
        shader_group_info_array.init( temp_stack.allocator, RayTracingGroup::Count );

        VkRayTracingShaderGroupCreateInfoKHR& raygen_group = shader_group_info_array.push_use();
        raygen_group = { VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR };
        raygen_group.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        raygen_group.generalShader = raygen_stage_index;
        raygen_group.closestHitShader = VK_SHADER_UNUSED_KHR;
        raygen_group.anyHitShader = VK_SHADER_UNUSED_KHR;
        raygen_group.intersectionShader = VK_SHADER_UNUSED_KHR;

        VkRayTracingShaderGroupCreateInfoKHR& miss_group = shader_group_info_array.push_use();
        miss_group = { VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR };
        miss_group.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        miss_group.generalShader = miss_stage_index;
        miss_group.closestHitShader = VK_SHADER_UNUSED_KHR;
        miss_group.anyHitShader = VK_SHADER_UNUSED_KHR;
        miss_group.intersectionShader = VK_SHADER_UNUSED_KHR;

        VkRayTracingShaderGroupCreateInfoKHR& hit_group = shader_group_info_array.push_use();
        hit_group = { VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR };
        hit_group.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
        hit_group.generalShader = VK_SHADER_UNUSED_KHR;
        hit_group.closestHitShader = hit_stage_index;
        hit_group.anyHitShader = VK_SHADER_UNUSED_KHR;
        hit_group.intersectionShader = VK_SHADER_UNUSED_KHR;

        VkRayTracingPipelineCreateInfoKHR pipeline_info{ VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR };
        pipeline_info.stageCount = shader_stage_info_array.size;
        pipeline_info.pStages = shader_stage_info_array.data;
        pipeline_info.groupCount = shader_group_info_array.size;
        pipeline_info.pGroups = shader_group_info_array.data;
        pipeline_info.maxPipelineRayRecursionDepth = 1;
        pipeline_info.pLibraryInfo = nullptr;
        pipeline_info.pLibraryInterface = nullptr;
        pipeline_info.pDynamicState = nullptr;
        pipeline_info.layout = pipeline->cached_vk_layout;

        if ( pipeline_binary_present ) {
            bool valid_cache = false;
            if ( cache_path != nullptr && cache_exists ) {
                Array<VkPipelineBinaryKHR> pipeline_binaries;
                VkPipelineBinaryInfoKHR pipeline_binary_info{ VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR };
                if ( restore_pipeline_binaries( cache_path, temp_stack, pipeline_binaries, pipeline_binary_info ) ) {
                    pipeline_binary_info.pNext = pipeline_info.pNext;
                    pipeline_info.pNext = &pipeline_binary_info;

                    check( vkCreateRayTracingPipelinesKHR( vulkan_device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipeline_info, vulkan_allocation_callbacks, &pipeline->vk_pipeline ) );

                    for ( u32 i = 0; i < pipeline_binaries.size; ++i ) {
                        vkDestroyPipelineBinaryKHR( vulkan_device, pipeline_binaries[ i ], vulkan_allocation_callbacks);
                    }
                    valid_cache = true;
                }
            }

            if ( !valid_cache ) {
                VkPipelineCreateFlags2CreateInfo flags2{ VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO };
                flags2.flags = VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR;

                flags2.pNext = pipeline_info.pNext;
                pipeline_info.pNext = &flags2;

                check( vkCreateRayTracingPipelinesKHR( vulkan_device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipeline_info, vulkan_allocation_callbacks, &pipeline->vk_pipeline ) );

                generate_pipeline_binaries( &pipeline_info, pipeline, cache_path, temp_stack );
            }
        } else {
            check( vkCreateRayTracingPipelinesKHR( vulkan_device, VK_NULL_HANDLE, pipeline_cache, 1, &pipeline_info, vulkan_allocation_callbacks, &pipeline->vk_pipeline ) );
        }

        pipeline->vk_bind_point = VkPipelineBindPoint::VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR;

        // Create Shader Binding Table (SBT)
        const u32 group_count = shader_group_info_array.size;
        const u32 handle_size = ray_tracing_pipeline_properties.shaderGroupHandleSize;
        const u32 handle_alignment = ray_tracing_pipeline_properties.shaderGroupHandleAlignment;
        const u32 base_alignment = ray_tracing_pipeline_properties.shaderGroupBaseAlignment;

        const u32 sbt_record_stride = ( u32 )memory_align( handle_size, handle_alignment );
        const u32 sbt_record_size = sbt_record_stride;

        const u32 group_handles_size = handle_size * group_count;

        sizet current_marker = temp_allocator->get_marker();

        Array<u8> group_handles;
        group_handles.init( temp_allocator, group_handles_size, group_handles_size );

        check( vkGetRayTracingShaderGroupHandlesKHR( vulkan_device, pipeline->vk_pipeline, 0, group_count, group_handles_size, group_handles.data ) );

        const u32 raygen_record_count = 1;
        const u32 miss_record_count = 1;
        const u32 hit_record_count = 1;

        // Calculate region offsets and sizes in the SBT buffer
        const sizet raygen_region_offset = 0;
        const sizet raygen_region_size = memory_align( sbt_record_stride * raygen_record_count, base_alignment );

        const sizet miss_region_offset = raygen_region_offset + raygen_region_size;
        const sizet miss_region_size = memory_align( sbt_record_stride * miss_record_count, base_alignment );

        const sizet hit_region_offset = miss_region_offset + miss_region_size;
        const sizet hit_region_size = memory_align( sbt_record_stride * hit_record_count, base_alignment );

        const sizet sbt_buffer_size = hit_region_offset + hit_region_size;

        // Allocate SBT memory and copy shader group handles
        Array<u8> sbt_data;
        sbt_data.init( temp_allocator, ( u32 )sbt_buffer_size, ( u32 )sbt_buffer_size );

        memset( sbt_data.data, 0, sbt_buffer_size );

        auto copy_sbt_record = [ & ]( sizet dst_offset, u32 group_index ) {
            RASSERT( group_index < group_count );
            memcpy( sbt_data.data + dst_offset, group_handles.data + group_index * handle_size, handle_size );
        };

        copy_sbt_record( raygen_region_offset, RayTracingGroup::Raygen );
        copy_sbt_record( miss_region_offset, RayTracingGroup::Miss );
        copy_sbt_record( hit_region_offset, RayTracingGroup::Hit );

        // Create unified SBT buffer
        pipeline->shader_binding_table = create_buffer( {
            .size = sbt_buffer_size,
            .usage = VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
            .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
            .name = "shader_binding_table" } );

        if ( pipeline->shader_binding_table.is_invalid() ) {
            rprint( "Failed to create shader binding table buffer\n" );
            RASSERT( false );
            return {};
        }

        MapBufferParameters map{ .buffer = pipeline->shader_binding_table, .offset = 0, .size = (u32)sbt_buffer_size };

        void* mapped_data = map_buffer( map );
        RASSERT( mapped_data );

        memcpy( mapped_data, sbt_data.data, sbt_buffer_size );
        flush_buffer( pipeline->shader_binding_table, 0, sbt_buffer_size );
        unmap_buffer( map );

        // Cache regions to be used when tracing rays.
        const VkDeviceAddress sbt_address = get_buffer_device_address( pipeline->shader_binding_table );

        RASSERT( ( sbt_address % base_alignment ) == 0 );

        RASSERT( ( sbt_address + raygen_region_offset ) % base_alignment == 0 );
        RASSERT( ( sbt_address + miss_region_offset ) % base_alignment == 0 );
        RASSERT( ( sbt_address + hit_region_offset ) % base_alignment == 0 );

        pipeline->sbt_raygen_region = {};
        pipeline->sbt_raygen_region.deviceAddress = sbt_address + raygen_region_offset;
        pipeline->sbt_raygen_region.stride = sbt_record_stride;
        pipeline->sbt_raygen_region.size = sbt_record_stride * raygen_record_count;

        pipeline->sbt_miss_region = {};
        pipeline->sbt_miss_region.deviceAddress = sbt_address + miss_region_offset;
        pipeline->sbt_miss_region.stride = sbt_record_stride;
        pipeline->sbt_miss_region.size = sbt_record_stride * miss_record_count;

        pipeline->sbt_hit_region = {};
        pipeline->sbt_hit_region.deviceAddress = sbt_address + hit_region_offset;
        pipeline->sbt_hit_region.stride = sbt_record_stride;
        pipeline->sbt_hit_region.size = sbt_record_stride * hit_record_count;

        pipeline->sbt_callable_region = {};

        temp_allocator->free_marker( current_marker );
    } else {
        VkComputePipelineCreateInfo pipeline_info{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };

        pipeline_info.stage = shader_stage_info_array[ 0 ];
        pipeline_info.layout = pipeline->cached_vk_layout;

        if ( pipeline_binary_present ) {
            bool valid_cache = false;
            if ( cache_path != nullptr && cache_exists ) {
                Array<VkPipelineBinaryKHR> pipeline_binaries;
                VkPipelineBinaryInfoKHR pipeline_binary_info{ VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR };
                if ( restore_pipeline_binaries( cache_path, temp_stack, pipeline_binaries, pipeline_binary_info ) ) {
                    pipeline_binary_info.pNext = pipeline_info.pNext;
                    pipeline_info.pNext = &pipeline_binary_info;

                    check( vkCreateComputePipelines( vulkan_device, VK_NULL_HANDLE, 1, &pipeline_info, vulkan_allocation_callbacks, &pipeline->vk_pipeline ) );

                    for ( u32 i = 0; i < pipeline_binaries.size; ++i ) {
                        vkDestroyPipelineBinaryKHR( vulkan_device, pipeline_binaries[ i ], vulkan_allocation_callbacks);
                    }

                    valid_cache = true;
                }
            }

            if ( !valid_cache ) {
                VkPipelineCreateFlags2CreateInfo flags2{ VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO };
                flags2.flags = VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR;

                flags2.pNext = pipeline_info.pNext;
                pipeline_info.pNext = &flags2;

                check( vkCreateComputePipelines( vulkan_device, VK_NULL_HANDLE, 1, &pipeline_info, vulkan_allocation_callbacks, &pipeline->vk_pipeline ) );

                generate_pipeline_binaries( &pipeline_info, pipeline, cache_path, temp_stack );
            }
        } else {
            check( vkCreateComputePipelines( vulkan_device, pipeline_cache, 1, &pipeline_info, vulkan_allocation_callbacks, &pipeline->vk_pipeline ) );
        }

        pipeline->vk_bind_point = VkPipelineBindPoint::VK_PIPELINE_BIND_POINT_COMPUTE;
    }

    if ( !pipeline_binary_present ) {
        if ( cache_path != nullptr && !cache_exists ) {
            sizet cache_data_size = 0;
            check( vkGetPipelineCacheData( vulkan_device, pipeline_cache, &cache_data_size, nullptr ) );

            void* cache_data = allocator->allocate( cache_data_size, 64 );
            check( vkGetPipelineCacheData( vulkan_device, pipeline_cache, &cache_data_size, cache_data ) );

            file_write_binary( cache_path, cache_data, cache_data_size );

            allocator->deallocate( cache_data );
        }

        vkDestroyPipelineCache( vulkan_device, pipeline_cache, vulkan_allocation_callbacks );
    }

    set_resource_name( VK_OBJECT_TYPE_PIPELINE, (u64)pipeline->vk_pipeline, creation.name );

    return handle;
}

BufferHandle GpuDevice::create_buffer( const BufferCreation& creation ) {
    RASSERTM( creation.size > 0, "Cannot create a zero-sized buffer." );
    RASSERTM( creation.usage != 0, "Buffer usage flags cannot be zero." );

    const bool mapped = ( creation.allocation_flags & VMA_ALLOCATION_CREATE_MAPPED_BIT ) != 0;
    const bool sequential_write = ( creation.allocation_flags & VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT ) != 0;
    const bool random_access = ( creation.allocation_flags & VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT ) != 0;

    RASSERTM( !mapped || sequential_write || random_access, "A mapped AUTO allocation requires a HOST_ACCESS flag." );
    RASSERTM( !( sequential_write && random_access ), "Specify either sequential write or random host access." );
    
    BufferHandle handle = buffers_pool.obtain();
    if ( handle.is_invalid() ) {
        return handle;
    }

    resource_tracker.track_create_resource( ResourceUpdateType::Buffer, handle.index(), creation.name );

    Buffer* buffer = get_buffer( handle );

    buffer->vk_buffer = VK_NULL_HANDLE;
    buffer->vma_allocation = VK_NULL_HANDLE;
    buffer->vk_device_memory = VK_NULL_HANDLE;
    buffer->vk_device_size = creation.size;
    buffer->memory_usage = creation.memory_usage;
    buffer->allocation_flags = creation.allocation_flags;

    buffer->handle = handle;
    buffer->name = creation.name;
    buffer->type_flags = creation.usage;
    buffer->size = static_cast< u32 >( creation.size );
    buffer->global_offset = 0;
    buffer->sync_state = {};
    buffer->mapped_data = nullptr;
    buffer->ready = true;

    buffer->persistent = mapped;
    buffer->device_only = creation.memory_usage == VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE ||
                          creation.memory_usage == VMA_MEMORY_USAGE_GPU_ONLY;

    VkBufferCreateInfo buffer_info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    buffer_info.size = creation.size;
    buffer_info.usage = creation.usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocation_create_info{};
    allocation_create_info.usage = creation.memory_usage;
    allocation_create_info.flags = creation.allocation_flags | VMA_ALLOCATION_CREATE_STRATEGY_BEST_FIT_BIT;

    VmaAllocationInfo allocation_info{};
    VkResult result = vmaCreateBuffer( vma_allocator, &buffer_info, &allocation_create_info,
                                       &buffer->vk_buffer, &buffer->vma_allocation, &allocation_info );

    if ( result != VK_SUCCESS ) {
        resource_tracker.track_destroy_resource( ResourceUpdateType::Buffer, handle.index() );
        buffers_pool.destroy( handle );
        return {};
    }

    buffer->vk_device_memory = allocation_info.deviceMemory;
    buffer->mapped_data = static_cast< u8* >( allocation_info.pMappedData );

#if defined ( _DEBUG )
    vmaSetAllocationName( vma_allocator, buffer->vma_allocation, creation.name );
#endif

    set_resource_name( VK_OBJECT_TYPE_BUFFER, reinterpret_cast< u64 >( buffer->vk_buffer ), creation.name );






    return handle;
}

SamplerHandle GpuDevice::create_sampler( const SamplerCreation& creation ) {

    SamplerHandle handle = samplers.obtain();
    if ( handle.is_invalid() ) {
        return handle;
    }

    resource_tracker.track_create_resource( ResourceUpdateType::Sampler, handle.index(), creation.name );

    Sampler* sampler = get_sampler( handle );

    sampler->address_mode_u = creation.address_mode_u;
    sampler->address_mode_v = creation.address_mode_v;
    sampler->address_mode_w = creation.address_mode_w;
    sampler->min_filter = creation.min_filter;
    sampler->mag_filter = creation.mag_filter;
    sampler->mip_filter = creation.mip_filter;
    sampler->name = creation.name;
    sampler->reduction_mode = creation.reduction_mode;

    VkSamplerCreateInfo create_info{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    create_info.addressModeU = creation.address_mode_u;
    create_info.addressModeV = creation.address_mode_v;
    create_info.addressModeW = creation.address_mode_w;
    create_info.minFilter = creation.min_filter;
    create_info.magFilter = creation.mag_filter;
    create_info.mipmapMode = creation.mip_filter;
    create_info.anisotropyEnable = 0;
    create_info.compareEnable = 0;
    create_info.unnormalizedCoordinates = 0;
    create_info.borderColor = VkBorderColor::VK_BORDER_COLOR_INT_OPAQUE_WHITE;
    create_info.minLod = 0;
    create_info.maxLod = 16;
    // TODO:
    /*float                   mipLodBias;
    float                   maxAnisotropy;
    VkCompareOp             compareOp;
    VkBorderColor           borderColor;
    VkBool32                unnormalizedCoordinates;*/

    VkSamplerReductionModeCreateInfoEXT createInfoReduction = { VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO_EXT };
    // Add optional reduction mode.
    if ( creation.reduction_mode != VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE_EXT ) {
        createInfoReduction.reductionMode = creation.reduction_mode;

        create_info.pNext = &createInfoReduction;
    }

    vkCreateSampler( vulkan_device, &create_info, vulkan_allocation_callbacks, &sampler->vk_sampler );

    set_resource_name( VK_OBJECT_TYPE_SAMPLER, (u64)sampler->vk_sampler, creation.name );

    return handle;
}

DescriptorSetLayoutHandle GpuDevice::create_descriptor_set_layout( const DescriptorSetLayoutCreation& creation ) {
    DescriptorSetLayoutHandle handle = descriptor_set_layouts_pool.obtain();
    if ( handle.is_invalid() ) {
        return handle;
    }

    resource_tracker.track_create_resource( ResourceUpdateType::DescriptorSetLayout, handle.index(), creation.name );

    DescriptorSetLayout* descriptor_set_layout = get_descriptor_set_layout( handle );
    descriptor_set_layout->binding_bitset = 0;

    for ( u32 r = 0; r < creation.bindings.size; ++r ) {
        const VkDescriptorSetLayoutBinding& input_binding = creation.bindings[ r ];
        // RASSERT( input_binding.binding < 64 );
        descriptor_set_layout->binding_bitset |= u64( 1 ) << (u64)input_binding.binding;
    }

    descriptor_set_layout->handle = handle;
    descriptor_set_layout->set_index = u16( creation.set_index );
    descriptor_set_layout->bindless = creation.bindless ? 1 : 0;
    descriptor_set_layout->dynamic = creation.dynamic ? 1 : 0;

    const bool skip_bindless_bindings = bindless_supported && !creation.bindless;
    u32 used_bindings = 0;

    ArenaScope scoped_allocator( MemoryService::instance()->get_thread_allocator() );
    Array<VkDescriptorSetLayoutBinding> vk_bindings;
    vk_bindings.init( scoped_allocator.allocator, ( u32 )creation.bindings.size );

    for ( u32 r = 0; r < creation.bindings.size; ++r ) {
        const VkDescriptorSetLayoutBinding& input_binding = creation.bindings[ r ];
        // [TAG: BINDLESS]
        // Skip bindings for images and textures as they are bindless, thus bound in the global bindless arrays (one for images, one for textures).
        // TODO(marco): better solution to allow individual image views to be bound
        if ( creation.set_index == 0 && skip_bindless_bindings /*&& (binding.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || binding.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)*/ ) {
            continue;
        }

        VkDescriptorSetLayoutBinding& vk_binding = vk_bindings.push_use();
        vk_binding = input_binding;
        ++used_bindings;

        vk_binding.descriptorType = vk_binding.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : vk_binding.descriptorType;

        // TODO:
        vk_binding.stageFlags = VK_SHADER_STAGE_ALL;
        vk_binding.pImmutableSamplers = nullptr;
    }

    // Create the descriptor set layout
    VkDescriptorSetLayoutCreateInfo layout_info = { 
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = used_bindings,
        .pBindings = vk_bindings.data };

    if ( creation.bindless ) {
        // TODO: re-enable variable descriptor count
        // Binding flags
        VkDescriptorBindingFlags bindless_flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT_EXT;//VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT_EXT
        bindless_flags |= VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT_EXT;
        // Needs update after bind flag.
        layout_info.flags |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT_EXT;

        Array<VkDescriptorBindingFlags> binding_flags;
        binding_flags.init( scoped_allocator.allocator, used_bindings, used_bindings );

        for ( u32 r = 0; r < creation.bindings.size; ++r ) {
            binding_flags[ r ] = bindless_flags;
        }

        VkDescriptorSetLayoutBindingFlagsCreateInfoEXT extended_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO_EXT, nullptr };
        extended_info.bindingCount = used_bindings;
        extended_info.pBindingFlags = binding_flags.data;

        layout_info.pNext = &extended_info;
        vkCreateDescriptorSetLayout( vulkan_device, &layout_info, vulkan_allocation_callbacks, &descriptor_set_layout->vk_descriptor_set_layout );
    } else {
        vkCreateDescriptorSetLayout( vulkan_device, &layout_info, vulkan_allocation_callbacks, &descriptor_set_layout->vk_descriptor_set_layout );
    }

    set_resource_name( VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, ( u64 )descriptor_set_layout->vk_descriptor_set_layout, creation.name );

    return handle;
}

#if defined (_DEBUG)
#define DESCRIPTOR_ALIASING_CHECK
#endif // _DEBUG

DescriptorSetHandle GpuDevice::create_descriptor_set( const DescriptorSetCreation& creation ) {

    DescriptorSetHandle handle = descriptor_sets_pool.obtain();
    if ( handle.is_invalid() ) {
        return handle;
    }

    resource_tracker.track_create_resource( ResourceUpdateType::DescriptorSet, handle.index(), creation.name);

    DescriptorSet* descriptor_set = get_descriptor_set( handle );
    *descriptor_set = {};

    const DescriptorSetLayout* descriptor_set_layout = get_descriptor_set_layout( creation.layout );

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo alloc_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_set_layout->bindless ? vulkan_bindless_descriptor_pool : vulkan_descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &descriptor_set_layout->vk_descriptor_set_layout };

    if ( descriptor_set_layout->bindless ) {
        VkDescriptorSetVariableDescriptorCountAllocateInfoEXT count_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO_EXT };
        u32 max_binding = k_max_bindless_resources - 1;
        count_info.descriptorSetCount = 1;
        // This number is the max allocatable count
        count_info.pDescriptorCounts = &max_binding;
        alloc_info.pNext = &count_info;
        check( vkAllocateDescriptorSets( vulkan_device, &alloc_info, &descriptor_set->vk_descriptor_set ) );
    } else {
        check( vkAllocateDescriptorSets( vulkan_device, &alloc_info, &descriptor_set->vk_descriptor_set ) );
    }

    set_resource_name( VK_OBJECT_TYPE_DESCRIPTOR_SET, ( u64 )descriptor_set->vk_descriptor_set, creation.name );

    // Cache data
    descriptor_set->layout = descriptor_set_layout;

    // Allocate temporary arrays to write descriptors
    ArenaAllocator* temp_allocator = MemoryService::instance()->get_thread_allocator();
    sizet marker = temp_allocator->get_marker();

    Array<VkWriteDescriptorSet> descriptors_to_modify;
    Array<VkDescriptorBufferInfo> buffer_infos;
    Array<VkDescriptorImageInfo> image_infos;

    const u32 num_buffer_infos = u32( creation.buffers.size + creation.ssbos.size + creation.dynamic_buffers.size );
    u32 num_image_infos = u32( creation.textures.size + creation.images.size + creation.samplers.size );
    for ( u32 i = 0; i < creation.image_arrays.size; ++i ) {
        num_image_infos += creation.image_arrays[ i ].count;
    }
    const u32 num_tlas_descriptor = creation.tlas.tlas.is_valid() ? 1 : 0;

    descriptors_to_modify.init( temp_allocator, num_buffer_infos + num_image_infos + num_tlas_descriptor );
    buffer_infos.init( temp_allocator, num_buffer_infos );
    image_infos.init( temp_allocator, num_image_infos );

    Sampler* sampler = get_sampler( global_samplers[ GlobalSamplers::LinearClamp ] );

#if defined (DESCRIPTOR_ALIASING_CHECK)
    BitSet bindings_written;
    bindings_written.init( temp_allocator, 2048 );
    bindings_written.clear();
#endif // DESCRIPTOR_ALIASING_CHECK

    for ( u32 i = 0; i < creation.textures.size; ++i ) {

        const TextureDescriptor& descriptor = creation.textures[ i ];

        if ( !has_binding( descriptor_set_layout, descriptor.binding ) ) {
            continue;
        }

#if defined (DESCRIPTOR_ALIASING_CHECK)
        RASSERTM( !bindings_written.get_bit( descriptor.binding ), "Binding %d is being written more than once in the same descriptor set. Please check your descriptor set creation for set %d.", descriptor.binding, descriptor_set_layout->set_index );
        bindings_written.set_bit( descriptor.binding );
#endif // DESCRIPTOR_ALIASING_CHECK

        ImageView* image_view = get_image_view( descriptor.texture );
        Image* texture = get_image( image_view->parent_image );

        VkDescriptorImageInfo& image_info = image_infos.push_use();
        image_info = {
            .sampler = texture->sampler ? texture->sampler->vk_sampler : sampler->vk_sampler,
            .imageView = image_view->vk_image_view,
            .imageLayout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL_KHR
        };

        VkWriteDescriptorSet& descriptor_write = descriptors_to_modify.push_use();

        descriptor_write = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = descriptor_set->vk_descriptor_set,
            .dstBinding = descriptor.binding,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &image_info,
        };
        descriptor_write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptor_write.dstSet = descriptor_set->vk_descriptor_set;
        descriptor_write.dstBinding = descriptor.binding;
        descriptor_write.dstArrayElement = 0;
        descriptor_write.descriptorCount = 1;
        descriptor_write.pImageInfo = &image_info;
    }

#if defined (DESCRIPTOR_ALIASING_CHECK)
    bindings_written.clear();
#endif // DESCRIPTOR_ALIASING_CHECK

    for ( u32 i = 0; i < creation.images.size; ++i ) {

        const TextureDescriptor& descriptor = creation.images[ i ];

        if ( !has_binding( descriptor_set_layout, descriptor.binding ) ) {
            continue;
        }

#if defined (DESCRIPTOR_ALIASING_CHECK)
        RASSERTM( !bindings_written.get_bit( descriptor.binding ), "Binding %d is being written more than once in the same descriptor set. Please check your descriptor set creation for set %d.", descriptor.binding, descriptor_set_layout->set_index );
        bindings_written.set_bit( descriptor.binding );
#endif // DESCRIPTOR_ALIASING_CHECK

        ImageView* image_view = get_image_view( descriptor.texture );

        VkDescriptorImageInfo& image_info = image_infos.push_use();
        image_info.sampler = nullptr;
        image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        image_info.imageView = image_view->vk_image_view;

        VkWriteDescriptorSet& descriptor_write = descriptors_to_modify.push_use();
        descriptor_write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        descriptor_write.dstSet = descriptor_set->vk_descriptor_set;
        descriptor_write.dstBinding = descriptor.binding;
        descriptor_write.dstArrayElement = 0;
        descriptor_write.descriptorCount = 1;
        descriptor_write.pImageInfo = &image_info;
    }

#if defined (DESCRIPTOR_ALIASING_CHECK)
    bindings_written.clear();
#endif // DESCRIPTOR_ALIASING_CHECK

    for ( u32 i = 0; i < creation.image_arrays.size; ++i ) {

        const TextureArrayDescriptor& descriptor = creation.image_arrays[ i ];

        if ( !has_binding( descriptor_set_layout, descriptor.binding ) ) {
            continue;
        }

#if defined (DESCRIPTOR_ALIASING_CHECK)
        RASSERTM( !bindings_written.get_bit( descriptor.binding ), "Binding %d is being written more than once in the same descriptor set. Please check your descriptor set creation for set %d.", descriptor.binding, descriptor_set_layout->set_index );
        bindings_written.set_bit( descriptor.binding );
#endif // DESCRIPTOR_ALIASING_CHECK

        for ( u32 i = 0; i < descriptor.count; ++i ) {
            ImageView* image_view = get_image_view( descriptor.textures[ i ] );

            VkDescriptorImageInfo& image_info = image_infos.push_use();
            image_info.sampler = nullptr;
            image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            image_info.imageView = image_view->vk_image_view;

            VkWriteDescriptorSet& descriptor_write = descriptors_to_modify.push_use();
            descriptor_write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            descriptor_write.dstSet = descriptor_set->vk_descriptor_set;
            descriptor_write.dstBinding = descriptor.binding;
            descriptor_write.dstArrayElement = i;
            descriptor_write.descriptorCount = 1;
            descriptor_write.pImageInfo = &image_info;
        }
    }

#if defined (DESCRIPTOR_ALIASING_CHECK)
    bindings_written.clear();
#endif // DESCRIPTOR_ALIASING_CHECK

    for ( u32 i = 0; i < creation.buffers.size; ++i ) {

        const BufferDescriptor& descriptor = creation.buffers[ i ];

        if ( !has_binding( descriptor_set_layout, descriptor.binding ) ) {
            continue;
        }

#if defined (DESCRIPTOR_ALIASING_CHECK)
        RASSERTM( !bindings_written.get_bit( descriptor.binding ), "Binding %d is being written more than once in the same descriptor set. Please check your descriptor set creation for set %d.", descriptor.binding, descriptor_set_layout->set_index );
        bindings_written.set_bit( descriptor.binding );
#endif // DESCRIPTOR_ALIASING_CHECK

        Buffer* buffer = get_buffer( descriptor.buffer );

        VkDescriptorBufferInfo& buffer_info = buffer_infos.push_use();
        buffer_info.offset = 0;
        buffer_info.range = buffer->size;

        buffer_info.buffer = buffer->vk_buffer;

        VkWriteDescriptorSet& descriptor_write = descriptors_to_modify.push_use();
        descriptor_write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptor_write.dstSet = descriptor_set->vk_descriptor_set;
        descriptor_write.dstBinding = descriptor.binding;
        descriptor_write.dstArrayElement = 0;
        descriptor_write.descriptorCount = 1;
        descriptor_write.pBufferInfo = &buffer_info;
    }

#if defined (DESCRIPTOR_ALIASING_CHECK)
    bindings_written.clear();
#endif // DESCRIPTOR_ALIASING_CHECK

    for ( u32 i = 0; i < creation.ssbos.size; ++i ) {

        const BufferDescriptor& descriptor = creation.ssbos[ i ];

        if ( !has_binding( descriptor_set_layout, descriptor.binding ) ) {
            continue;
        }

#if defined (DESCRIPTOR_ALIASING_CHECK)
        RASSERTM( !bindings_written.get_bit( descriptor.binding ), "Binding %d is being written more than once in the same descriptor set. Please check your descriptor set creation for set %d.", descriptor.binding, descriptor_set_layout->set_index );
        bindings_written.set_bit( descriptor.binding );
#endif // DESCRIPTOR_ALIASING_CHECK

        Buffer* buffer = get_buffer( descriptor.buffer );

        VkDescriptorBufferInfo& buffer_info = buffer_infos.push_use();
        buffer_info.buffer = buffer->vk_buffer;
        buffer_info.offset = 0;
        buffer_info.range = buffer->size;

        VkWriteDescriptorSet& descriptor_write = descriptors_to_modify.push_use();
        descriptor_write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        descriptor_write.dstSet = descriptor_set->vk_descriptor_set;
        descriptor_write.dstBinding = descriptor.binding;
        descriptor_write.dstArrayElement = 0;
        descriptor_write.descriptorCount = 1;
        descriptor_write.pBufferInfo = &buffer_info;
    }

#if defined (DESCRIPTOR_ALIASING_CHECK)
    bindings_written.clear();
#endif // DESCRIPTOR_ALIASING_CHECK

    for ( u32 i = 0; i < creation.samplers.size; ++i ) {

        const SamplerDescriptor& descriptor = creation.samplers[ i ];

        if ( !has_binding( descriptor_set_layout, descriptor.binding ) ) {
            continue;
        }

#if defined (DESCRIPTOR_ALIASING_CHECK)
        RASSERTM( !bindings_written.get_bit( descriptor.binding ), "Binding %d is being written more than once in the same descriptor set. Please check your descriptor set creation for set %d.", descriptor.binding, descriptor_set_layout->set_index );
        bindings_written.set_bit( descriptor.binding );
#endif // DESCRIPTOR_ALIASING_CHECK

        Sampler* sampler = get_sampler( descriptor.sampler );

        VkDescriptorImageInfo& image_info = image_infos.push_use();
        image_info.sampler = sampler->vk_sampler;

        VkWriteDescriptorSet& descriptor_write = descriptors_to_modify.push_use();
        descriptor_write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        descriptor_write.dstSet = descriptor_set->vk_descriptor_set;
        descriptor_write.dstBinding = creation.samplers[ i ].binding;
        descriptor_write.dstArrayElement = 0;
        descriptor_write.descriptorCount = 1;
        descriptor_write.pImageInfo = &image_info;
    }

#if defined (DESCRIPTOR_ALIASING_CHECK)
    bindings_written.clear();
#endif // DESCRIPTOR_ALIASING_CHECK

    // Add dynamic buffer descriptor
    for ( u32 d = 0; d < (u32)creation.dynamic_buffers.size; ++d ) {

        const DynamicBufferBinding& binding = creation.dynamic_buffers[ d ];
        Buffer* buffer = get_buffer( dynamic_buffer );

#if defined (DESCRIPTOR_ALIASING_CHECK)
        RASSERTM( !bindings_written.get_bit( binding.binding ), "Binding %d is being written more than once in the same descriptor set. Please check your descriptor set creation for set %d.", binding.binding, descriptor_set_layout->set_index );
        bindings_written.set_bit( binding.binding );
#endif // DESCRIPTOR_ALIASING_CHECK

        VkDescriptorBufferInfo& buffer_info = buffer_infos.push_use();
        buffer_info.buffer = buffer->vk_buffer;
        buffer_info.offset = 0;
        buffer_info.range = binding.size;

        VkWriteDescriptorSet& descriptor_write = descriptors_to_modify.push_use();
        descriptor_write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        descriptor_write.dstSet = descriptor_set->vk_descriptor_set;
        descriptor_write.dstBinding = binding.binding;
        descriptor_write.dstArrayElement = 0;
        descriptor_write.descriptorCount = 1;
        descriptor_write.pBufferInfo = &buffer_info;
    }

#if defined (DESCRIPTOR_ALIASING_CHECK)
    bindings_written.clear();
#endif // DESCRIPTOR_ALIASING_CHECK

    // NOTE(marco): this has to be static as it's used outside this function
    VkWriteDescriptorSetAccelerationStructureKHR as_info;
    as_info = VkWriteDescriptorSetAccelerationStructureKHR{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };

    if ( creation.tlas.tlas.is_valid() ) {

        TLAS* tlas = get_tlas( creation.tlas.tlas );

#if defined (DESCRIPTOR_ALIASING_CHECK)
        RASSERTM( !bindings_written.get_bit( creation.tlas.binding ), "Binding %d is being written more than once in the same descriptor set. Please check your descriptor set creation for set %d.", creation.tlas.binding, descriptor_set_layout->set_index );
        bindings_written.set_bit( creation.tlas.binding );
#endif // DESCRIPTOR_ALIASING_CHECK

        as_info.accelerationStructureCount = 1;
        as_info.pAccelerationStructures = &tlas->as;

        VkWriteDescriptorSet& descriptor_write = descriptors_to_modify.push_use();
        descriptor_write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        descriptor_write.dstSet = descriptor_set->vk_descriptor_set;
        descriptor_write.dstBinding = creation.tlas.binding;
        descriptor_write.dstArrayElement = 0;
        descriptor_write.descriptorCount = 1;
        descriptor_write.pNext = &as_info;
    }

    // Actually modify the descriptors
    vkUpdateDescriptorSets( vulkan_device, descriptors_to_modify.size, descriptors_to_modify.data, 0, nullptr );

    temp_allocator->free_marker( marker );

    return handle;
}

// Resource Destruction ///////////////////////////////////////////////////

void GpuDevice::destroy_buffer( BufferHandle handle ) {

    Buffer* buffer = get_buffer( handle );
    if ( buffer ) {

        resource_tracker.track_destroy_resource( ResourceUpdateType::Buffer, handle.index() );

        resource_deletion_queue.push( { ResourceUpdateType::Buffer, handle.id, current_frame, 1 } );

    } else {
        rprint( "Graphics error: trying to free invalid Buffer %u\n", handle.index() );
    }
}

void GpuDevice::destroy_image( ImageHandle handle ) {

    Image* image = images.get( handle );
    if ( image ) {

        resource_tracker.track_destroy_resource( ResourceUpdateType::Image, handle.index() );

        resource_deletion_queue.push( { ResourceUpdateType::Image, handle.id, current_frame, 1 } );
    } else {
        rprint( "Graphics error: trying to free invalid Image %u\n", handle.index() );
    }
}

void GpuDevice::destroy_image_view( ImageViewHandle handle ) {

    ImageView* image_view = image_views.get( handle );
    if ( image_view ) {

        resource_tracker.track_destroy_resource( ResourceUpdateType::ImageView, handle.index() );

        resource_deletion_queue.push( { ResourceUpdateType::ImageView, handle.id, current_frame, 1 } );
    } else {
        rprint( "Graphics error: trying to free invalid ImageView %u\n", handle.index() );
    }
}

void GpuDevice::destroy_pipeline_layout( PipelineLayoutHandle handle ) {

    PipelineLayout* pipeline_layout = get_pipeline_layout( handle );
    if ( pipeline_layout ) {

        resource_tracker.track_destroy_resource( ResourceUpdateType::PipelineLayout, handle.index() );
        resource_deletion_queue.push( { ResourceUpdateType::PipelineLayout, handle.id, current_frame, 1 } );

        // First descriptor set is always the global set, do not destroy it.
        for ( u32 l = 1; l < pipeline_layout->num_active_layouts; ++l ) {
            if ( pipeline_layout->descriptor_set_layout_handles[ l ].is_valid() ) {
                destroy_descriptor_set_layout( pipeline_layout->descriptor_set_layout_handles[ l ] );
            }
        }
    } else {
        rprint( "Graphics error: trying to free invalid PipelineLayout %u\n", handle.index() );
    }
}

void GpuDevice::destroy_blas( BLASHandle handle ) {
    if ( handle.is_valid() ) {

        //resource_tracker.track_destroy_resource( ResourceUpdateType::ShaderState, shader.index );
        resource_deletion_queue.push( { ResourceUpdateType::BLAS, handle.id, current_frame, 1 } );

        // Delete dependent resources
        BLAS* blas = get_blas( handle );
        if ( blas ) {
            resource_deletion_queue.push( { ResourceUpdateType::Buffer, blas->blas_buffer.id, current_frame,  1 } );
            // TODO: scratch buffer should be common ?
            resource_deletion_queue.push( { ResourceUpdateType::Buffer, blas->scratch_buffer.id, current_frame,  1 } );
        }

    } else {
        rprint( "Graphics error: trying to free invalid BLAS %u\n", handle.index() );
    }
}

void GpuDevice::destroy_tlas( TLASHandle handle ) {
    if ( handle.is_valid() ) {

        //resource_tracker.track_destroy_resource( ResourceUpdateType::ShaderState, shader.index );
        resource_deletion_queue.push( { ResourceUpdateType::TLAS, handle.id, current_frame, 1 } );

        // Delete dependent resources
        TLAS* tlas = get_tlas( handle );
        if ( tlas ) {
            resource_deletion_queue.push( { ResourceUpdateType::Buffer, tlas->tlas_buffer.id, current_frame,  1 } );
            // TODO: scratch buffer should be common ?
            resource_deletion_queue.push( { ResourceUpdateType::Buffer, tlas->scratch_buffer.id, current_frame,  1 } );
            resource_deletion_queue.push( { ResourceUpdateType::Buffer, tlas->instance_buffer.id, current_frame,  1 } );
        }

    } else {
        rprint( "Graphics error: trying to free invalid TLAS %u\n", handle.index() );
    }
}

void GpuDevice::add_image_view_to_bindless( ImageViewHandle image_view ) {
    // Add deferred bindless update.
    if ( bindless_supported ) {
        image_views_to_update_bindless.push( { image_view, current_frame, 0 } );
    }
}

void GpuDevice::remove_image_view_from_bindless( ImageViewHandle image_view ) {
    // Add deferred bindless update.
    if ( bindless_supported ) {
        image_views_to_update_bindless.push( { image_view, current_frame, 1 } );
    }
}

void GpuDevice::add_buffer_to_bindless( BufferHandle buffer ) {
    // Add deferred bindless update.
    if ( bindless_supported ) {
        buffers_to_update_bindless.push( { buffer, current_frame, 0 } );
    }
}

void GpuDevice::remove_buffer_from_bindless( BufferHandle buffer ) {
    // Add deferred bindless update.
    if ( bindless_supported ) {
        buffers_to_update_bindless.push( { buffer, current_frame, 1 } );
    }
}

void GpuDevice::destroy_pipeline( PipelineHandle handle ) {

    Pipeline* pipeline = get_pipeline( handle );
    if ( pipeline ) {

        resource_tracker.track_destroy_resource( ResourceUpdateType::Pipeline, handle.index() );

        resource_deletion_queue.push( { ResourceUpdateType::Pipeline, handle.id, current_frame, 1 } );
        // Shader state creation is handled internally when creating a pipeline, thus add this to track correctly.
        ShaderState* shader_state_data = get_shader_state( pipeline->shader_state );
        if ( shader_state_data->type == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR ) {
            destroy_buffer( pipeline->shader_binding_table );
        }

        destroy_shader_state( pipeline->shader_state );
    } else {
        rprint( "Graphics error: trying to free invalid Pipeline %u\n", handle.index() );
    }
}

void GpuDevice::destroy_sampler( SamplerHandle handle ) {

    Sampler* sampler = get_sampler( handle );
    if ( sampler ) {

        resource_tracker.track_destroy_resource( ResourceUpdateType::Sampler, handle.index() );

        resource_deletion_queue.push( { ResourceUpdateType::Sampler, handle.id, current_frame, 1 } );
    } else {
        rprint( "Graphics error: trying to free invalid Sampler %u\n", handle.index() );
    }
}

void GpuDevice::destroy_descriptor_set_layout( DescriptorSetLayoutHandle handle ) {

    DescriptorSetLayout* descriptor_set_layout = get_descriptor_set_layout( handle );
    if ( descriptor_set_layout ) {

        resource_tracker.track_destroy_resource( ResourceUpdateType::DescriptorSetLayout, handle.index() );

        resource_deletion_queue.push( { ResourceUpdateType::DescriptorSetLayout, handle.id, current_frame, 1 } );
    } else {
        rprint( "Graphics error: trying to free invalid DescriptorSetLayout %u\n", handle.index() );
    }
}

void GpuDevice::destroy_descriptor_set( DescriptorSetHandle handle ) {
    DescriptorSet* descriptor_set = get_descriptor_set( handle );
    if ( descriptor_set ) {

        resource_tracker.track_destroy_resource( ResourceUpdateType::DescriptorSet, handle.index() );

        resource_deletion_queue.push( { ResourceUpdateType::DescriptorSet, handle.id, current_frame, 1 } );
    } else {
        rprint( "Graphics error: trying to free invalid DescriptorSet %u\n", handle.index() );
    }
}

void GpuDevice::destroy_shader_state( ShaderStateHandle shader ) {

    ShaderState* shader_state = get_shader_state( shader );
    if ( shader_state ) {

        resource_tracker.track_destroy_resource( ResourceUpdateType::ShaderState, shader.index() );

        resource_deletion_queue.push( { ResourceUpdateType::ShaderState, shader.id, current_frame, 1 } );
    } else {
        rprint( "Graphics error: trying to free invalid Shader %u\n", shader.index() );
    }
}
// Real destruction methods - the other enqueue only the resources.
void GpuDevice::destroy_buffer_instant( ResourceHandle raw_handle ) {

    BufferHandle handle;
    handle.id = raw_handle;

    Buffer* buffer = get_buffer( handle );

    if ( buffer ) {
        vmaDestroyBuffer( vma_allocator, buffer->vk_buffer, buffer->vma_allocation );

        buffers_pool.destroy( handle );
    }
}

void GpuDevice::destroy_image_instant( ResourceHandle image ) {

    ImageHandle handle;
    handle.id = image;

    Image* v_image = images.get( handle );

    if ( v_image ) {
        // Standard image: vma allocation valid
        if ( v_image->vma_allocation != 0 ) {
            vmaDestroyImage( vma_allocator, v_image->vk_image, v_image->vma_allocation );
        } else if ( ( v_image->flags & TextureFlags::Sparse_mask ) == TextureFlags::Sparse_mask ) {
            // Sparse textures
            vkDestroyImage( vulkan_device, v_image->vk_image, vulkan_allocation_callbacks );
        } else if ( v_image->vma_allocation == nullptr ) {
            // Aliased textures
            vkDestroyImage( vulkan_device, v_image->vk_image, vulkan_allocation_callbacks );
        }

        images.destroy( handle );
    }
}

void GpuDevice::destroy_image_view_instant( ResourceHandle view ) {

    ImageViewHandle handle;
    handle.id = view;

    ImageView* v_view = image_views.get( handle );
    if ( v_view ) {
        vkDestroyImageView( vulkan_device, v_view->vk_image_view, vulkan_allocation_callbacks );
        v_view->vk_image_view = VK_NULL_HANDLE;

        image_views.destroy( handle );
    }
}

void GpuDevice::destroy_pipeline_instant( ResourceHandle raw_handle ) {

    PipelineHandle handle;
    handle.id = raw_handle;

    Pipeline* pipeline = pipelines.get( handle );

    if ( pipeline ) {
        vkDestroyPipeline( vulkan_device, pipeline->vk_pipeline, vulkan_allocation_callbacks );

        pipelines.destroy( handle );
    }
}

void GpuDevice::destroy_sampler_instant( ResourceHandle sampler ) {

    SamplerHandle handle;
    handle.id = sampler;

    Sampler* v_sampler = samplers.get( handle );

    if ( v_sampler ) {
        vkDestroySampler( vulkan_device, v_sampler->vk_sampler, vulkan_allocation_callbacks );

        samplers.destroy( handle );
    }
}

void GpuDevice::destroy_descriptor_set_layout_instant( ResourceHandle descriptor_set_layout ) {

    DescriptorSetLayoutHandle handle;
    handle.id = descriptor_set_layout;

    DescriptorSetLayout* v_descriptor_set_layout = descriptor_set_layouts_pool.get( handle );

    if ( v_descriptor_set_layout ) {
        vkDestroyDescriptorSetLayout( vulkan_device, v_descriptor_set_layout->vk_descriptor_set_layout, vulkan_allocation_callbacks );

        v_descriptor_set_layout->vk_descriptor_set_layout = nullptr;

        descriptor_set_layouts_pool.destroy( handle );
    }
}

void GpuDevice::destroy_descriptor_set_instant( ResourceHandle descriptor_set ) {

    DescriptorSetHandle handle;
    handle.id = descriptor_set;

    DescriptorSet* v_descriptor_set = get_descriptor_set( handle );

    if ( v_descriptor_set ) {
        // Contains the allocation for all the resources, binding and samplers arrays.
        rfree( v_descriptor_set->resources, allocator );
        // This is freed with the DescriptorSet pool.
        //vkFreeDescriptorSets
        descriptor_sets_pool.destroy( handle );
    }
}

void GpuDevice::destroy_pipeline_layout_instant( ResourceHandle handle ) {

    PipelineLayoutHandle layout_handle;
    layout_handle.id = handle;

    PipelineLayout* pipeline_layout = get_pipeline_layout( layout_handle );

    if ( pipeline_layout ) {
        vkDestroyPipelineLayout( vulkan_device, pipeline_layout->vk_pipeline_layout, vulkan_allocation_callbacks );
        pipeline_layouts.destroy( layout_handle );
    }
}

void GpuDevice::destroy_blas_instant( ResourceHandle handle ) {
    BLASHandle blas_handle;
    blas_handle.id = handle;

    BLAS* blas = get_blas( blas_handle );

    if ( blas ) {
        vkDestroyAccelerationStructureKHR( vulkan_device, blas->as, vulkan_allocation_callbacks );
        blases.destroy( blas_handle );
    }
}

void GpuDevice::destroy_tlas_instant( ResourceHandle handle ) {
    TLASHandle tlas_handle;
    tlas_handle.id = handle;

    TLAS* tlas = get_tlas( tlas_handle );

    if ( tlas ) {
        vkDestroyAccelerationStructureKHR( vulkan_device, tlas->as, vulkan_allocation_callbacks );
        tlases.destroy( tlas_handle );
    }
}

void GpuDevice::destroy_shader_state_instant( ResourceHandle shader ) {

    ShaderStateHandle shader_handle;
    shader_handle.id = shader;

    ShaderState* shader_state = get_shader_state( shader_handle );
    if ( shader_state ) {

        for ( size_t i = 0; i < shader_state->active_shaders; i++ ) {
            vkDestroyShaderModule( vulkan_device, shader_state->vk_shader_modules[ i ], vulkan_allocation_callbacks );
        }

        shader_states.destroy( shader_handle );
    }
}

void GpuDevice::set_resource_name( VkObjectType type, u64 handle, const char* name ) {

    if ( !debug_utils_extension_present ) {
        return;
    }
    VkDebugUtilsObjectNameInfoEXT name_info = { VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT };
    name_info.objectType = type;
    name_info.objectHandle = handle;
    name_info.pObjectName = name;
    vkSetDebugUtilsObjectNameEXT( vulkan_device, &name_info );
}

// Swapchain //////////////////////////////////////////////////////////////

void GpuDevice::create_swapchain() {

    //// Check if surface is supported
    // TODO: Windows only!
    VkBool32 surface_supported;
    vkGetPhysicalDeviceSurfaceSupportKHR( vulkan_physical_device, vulkan_main_queue_family, vulkan_window_surface, &surface_supported );
    if ( surface_supported != VK_TRUE ) {
        rprint( "Error no WSI support on physical device 0\n" );
    }

    VkSurfaceCapabilitiesKHR surface_capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR( vulkan_physical_device, vulkan_window_surface, &surface_capabilities );

    VkExtent2D swapchain_extent = surface_capabilities.currentExtent;
    if ( swapchain_extent.width == UINT32_MAX ) {
        swapchain_extent.width = raptor::clamp<u32>( swapchain_extent.width, surface_capabilities.minImageExtent.width, surface_capabilities.maxImageExtent.width );
        swapchain_extent.height = raptor::clamp<u32>( swapchain_extent.height, surface_capabilities.minImageExtent.height, surface_capabilities.maxImageExtent.height );
    }

    rprint( "Create swapchain %u %u - saved %u %u, min image %u\n", swapchain_extent.width, swapchain_extent.height, swapchain_width, swapchain_height, surface_capabilities.minImageCount );

    swapchain_width = (u16)swapchain_extent.width;
    swapchain_height = (u16)swapchain_extent.height;

    //vulkan_swapchain_image_count = surface_capabilities.minImageCount + 2;

    VkSwapchainCreateInfoKHR swapchain_create_info = {};
    swapchain_create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchain_create_info.surface = vulkan_window_surface;
    swapchain_create_info.minImageCount = vulkan_swapchain_image_count;
    swapchain_create_info.imageFormat = vulkan_surface_format.format;
    swapchain_create_info.imageExtent = swapchain_extent;
    swapchain_create_info.clipped = VK_TRUE;
    swapchain_create_info.imageArrayLayers = 1;
    swapchain_create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swapchain_create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchain_create_info.preTransform = surface_capabilities.currentTransform;
    swapchain_create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchain_create_info.presentMode = vulkan_present_mode;

    VkResult result = vkCreateSwapchainKHR( vulkan_device, &swapchain_create_info, 0, &vulkan_swapchain );
    check( result );

    //// Cache swapchain images
    vkGetSwapchainImagesKHR( vulkan_device, vulkan_swapchain, &vulkan_swapchain_image_count, NULL );

    Array<VkImage> swapchain_images;
    swapchain_images.init( allocator, vulkan_swapchain_image_count, vulkan_swapchain_image_count );
    vkGetSwapchainImagesKHR( vulkan_device, vulkan_swapchain, &vulkan_swapchain_image_count, swapchain_images.data );

    // Manually transition the image
    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    CommandBuffer* command_buffer = allocate_command_buffer( 0, current_frame, CommandQueueType::Graphics );
    vkBeginCommandBuffer( command_buffer->vk_command_buffer, &beginInfo );

    for ( u32 iv = 0; iv < vulkan_swapchain_image_count; iv++ ) {

        //resource_tracker.track_create_resource( ResourceUpdateType::Texture, vk_framebuffer->color_attachments[ 0 ].index, "swapchain" );

        // Manual creation of image
        ImageHandle handle = images.obtain();
        Image* color = get_image( handle );
        *color = {};
        color->vk_image = swapchain_images[ iv ];
        color->vk_format = vulkan_surface_format.format;
        color->type = TextureType::Texture2D;
        color->width = swapchain_width;
        color->height = swapchain_height;

        // Manual creation of image view
        ImageViewHandle image_view = image_views.obtain();
        ImageView* vk_image_view = get_image_view( image_view );

        vk_image_view->parent_image = handle;
        vk_image_view->name = "SwapchainImage_View";
        vk_image_view->view_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_image_view->compute_access = false;
        vk_image_view->subresource_range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

        VkImageViewCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        info.image = swapchain_images[ iv ];
        info.format = vulkan_surface_format.format;
        info.viewType = vk_image_view->view_type;
        info.subresourceRange = vk_image_view->subresource_range;
        check( vkCreateImageView( vulkan_device, &info, vulkan_allocation_callbacks, &vk_image_view->vk_image_view ) );

        set_resource_name( VK_OBJECT_TYPE_IMAGE_VIEW, (u64)vk_image_view->vk_image_view, vk_image_view->name );

        ImageCreation depth_image_creation;
        depth_image_creation.set_size( swapchain_width, swapchain_height, 1 ).set_format_type( VK_FORMAT_D32_SFLOAT, TextureType::Texture2D ).set_name( "DepthImage_Texture" );
        ImageHandle depth_handle = create_image( depth_image_creation );

        // Manual creation of image view
        ImageViewHandle depth_view = image_views.obtain();
        ImageView* vk_depth_view = get_image_view( depth_view );

        vk_depth_view->parent_image = depth_handle;
        vk_depth_view->name = "SwapchainDepth_View";
        vk_depth_view->view_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_depth_view->compute_access = false;
        vk_depth_view->subresource_range = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };

        Image* depth_stencil_texture = get_image( depth_handle );

        info.image = depth_stencil_texture->vk_image;
        info.format = depth_stencil_texture->vk_format;
        info.viewType = vk_depth_view->view_type;
        info.subresourceRange = vk_depth_view->subresource_range;
        check( vkCreateImageView( vulkan_device, &info, vulkan_allocation_callbacks, &vk_depth_view->vk_image_view ) );

        set_resource_name( VK_OBJECT_TYPE_IMAGE_VIEW, (u64)vk_depth_view->vk_image_view, vk_depth_view->name );


        // Cache images and image views
        vulkan_swapchain_images[ iv ] = handle;
        vulkan_swapchain_image_views[ iv ] = image_view;
        vulkan_swapchain_depth_images[ iv ] = depth_handle;
        vulkan_swapchain_depth_image_views[ iv ] = depth_view;

        command_buffer->add_image_barrier(
            handle,
            raptor::range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 ),
            ImageSyncState{
                .stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .access = VK_ACCESS_2_NONE,
                .layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            }
        );
        command_buffer->flush_barriers();
        //util_add_image_barrier( this, command_buffer->vk_command_buffer, color->vk_image, RESOURCE_STATE_UNDEFINED, RESOURCE_STATE_PRESENT, 0, 1, false );
    }

    vkEndCommandBuffer( command_buffer->vk_command_buffer );

    // Submit command buffer
    VkCommandBufferSubmitInfoKHR command_buffer_info{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO_KHR };
    command_buffer_info.commandBuffer = command_buffer->vk_command_buffer;

    VkSubmitInfo2KHR submit_info{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2_KHR };
    submit_info.commandBufferInfoCount = 1;
    submit_info.pCommandBufferInfos = &command_buffer_info;

    check( vkQueueSubmit2( vulkan_main_queue, 1, &submit_info, VK_NULL_HANDLE ) );

    vkQueueWaitIdle( vulkan_main_queue );

    swapchain_images.shutdown();
}

void GpuDevice::destroy_swapchain() {

    for ( size_t iv = 0; iv < vulkan_swapchain_image_count; iv++ ) {

        // TODO(gabriel): check this.
        Image* vk_image = get_image( vulkan_swapchain_images[ iv ] );
        ImageView* vk_image_view = get_image_view( vulkan_swapchain_image_views[ iv ] );

        vkDestroyImageView( vulkan_device, vk_image_view->vk_image_view, vulkan_allocation_callbacks );
        images.destroy( vulkan_swapchain_images[ iv ] );
        image_views.destroy( vulkan_swapchain_image_views[ iv ] );

        //
        ImageHandle depth_image = vulkan_swapchain_depth_images[ iv ];
        if ( depth_image.is_valid() ) {
            destroy_image_instant( depth_image.id );

            destroy_image_view_instant( vulkan_swapchain_depth_image_views[ iv ].id );
        }
    }

    vkDestroySwapchainKHR( vulkan_device, vulkan_swapchain, vulkan_allocation_callbacks );
}

void GpuDevice::resize_swapchain() {

    vkDeviceWaitIdle( vulkan_device );

    VkSurfaceCapabilitiesKHR surface_capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR( vulkan_physical_device, vulkan_window_surface, &surface_capabilities );
    VkExtent2D swapchain_extent = surface_capabilities.currentExtent;

    // Skip zero-sized swapchain
    //rprint( "Requested swapchain resize %u %u\n", swapchain_extent.width, swapchain_extent.height );
    if ( swapchain_extent.width == 0 || swapchain_extent.height == 0 ) {
        //rprint( "Cannot create a zero-sized swapchain\n" );
        return;
    }

    // Destroy swapchain images and framebuffers
    destroy_swapchain();

    // Create swapchain
    create_swapchain();

    vkDeviceWaitIdle( vulkan_device );
}

// Descriptor Set /////////////////////////////////////////////////////////

void GpuDevice::update_descriptor_set( DescriptorSetHandle descriptor_set ) {

    if ( descriptor_set.is_valid() ) {

        DescriptorSetUpdate new_update = { descriptor_set, current_frame };
        descriptor_set_updates.push( new_update );


    } else {
        rprint( "Graphics error: trying to update invalid DescriptorSet %u\n", descriptor_set.index() );
    }
}

void GpuDevice::update_descriptor_set_instant( const DescriptorSetUpdate& update ) {

    // TODO: never used, re-implement ?
    RASSERT( false );
    // Use a dummy descriptor set to delete the vulkan descriptor set handle
    //DescriptorSetHandle dummy_delete_descriptor_set_handle = { descriptor_sets.obtain_resource() };
    //DescriptorSet* dummy_delete_descriptor_set = access_descriptor_set( dummy_delete_descriptor_set_handle );

    //DescriptorSet* descriptor_set = access_descriptor_set( update.descriptor_set );
    //const DescriptorSetLayout* descriptor_set_layout = descriptor_set->layout;

    //dummy_delete_descriptor_set->vk_descriptor_set = descriptor_set->vk_descriptor_set;
    //dummy_delete_descriptor_set->bindings = nullptr;
    //dummy_delete_descriptor_set->resources = nullptr;
    //dummy_delete_descriptor_set->samplers = nullptr;
    //dummy_delete_descriptor_set->num_resources = 0;

    //destroy_descriptor_set( dummy_delete_descriptor_set_handle );

    //// Allocate the new descriptor set and update its content.
    //VkWriteDescriptorSet descriptor_write[ 8 ];
    //VkDescriptorBufferInfo buffer_info[ 8 ];
    //VkDescriptorImageInfo image_info[ 8 ];

    //Sampler* vk_default_sampler = access_sampler( global_samplers[ GlobalSamplers::LinearClamp ] );

    //VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    //allocInfo.descriptorPool = vulkan_descriptor_pool;
    //allocInfo.descriptorSetCount = 1;
    //allocInfo.pSetLayouts = &descriptor_set->layout->vk_descriptor_set_layout;
    //vkAllocateDescriptorSets( vulkan_device, &allocInfo, &descriptor_set->vk_descriptor_set );

    //u32 num_resources = descriptor_set_layout->num_bindings;
    /*fill_write_descriptor_sets( *this, descriptor_set_layout, descriptor_set, descriptor_write, buffer_info, image_info, vk_default_sampler->vk_sampler,
                                num_resources );*/

                                //vkUpdateDescriptorSets( vulkan_device, num_resources, descriptor_write, 0, nullptr );
}

u32 GpuDevice::get_memory_heap_count() {
    return vma_allocator->GetMemoryHeapCount();
}

void GpuDevice::resize_buffer( BufferHandle buffer_handle, u32 new_size ) {

    Buffer* buffer = get_buffer( buffer_handle );
    RASSERT( buffer );
    RASSERT( new_size > 0 );

    if ( buffer->size == new_size ) {
        return;
    }

    const BufferCreation creation{
        .size = new_size,
        .usage = buffer->type_flags,
        .memory_usage = buffer->memory_usage,
        .allocation_flags = buffer->allocation_flags,
        .name = buffer->name
    };

    BufferHandle new_buffer_handle = create_buffer( creation );
    if ( new_buffer_handle.is_invalid() ) {
        rprint( "Failed to resize buffer %s to %llu bytes\n", buffer->name, static_cast< unsigned long long >( new_size ) );
        return;
    }

    Buffer* new_buffer = get_buffer( new_buffer_handle );
    RASSERT( new_buffer );

    // After the swap:
    // - buffer_handle keeps the new allocation;
    // - new_buffer_handle owns the old allocation and can be destroyed.
    Buffer temporary_buffer;
    memory_copy( &temporary_buffer, buffer, sizeof( Buffer ) );
    memory_copy( buffer, new_buffer, sizeof( Buffer ) );
    memory_copy( new_buffer, &temporary_buffer, sizeof( Buffer ) );

    buffer->handle = buffer_handle;
    new_buffer->handle = new_buffer_handle;

    // This now destroys the old allocation through the normal deferred path.
    destroy_buffer( new_buffer_handle );
}

void GpuDevice::resize_image( ImageHandle image, u32 width, u32 height ) {

    resize_image_3d( image, width, height, 1 );
}

void GpuDevice::resize_image( ImageHandle image, u32 width, u32 height, u32 mip_levels ) {

    resize_image_3d( image, width, height, 1, mip_levels );
}

void GpuDevice::resize_image_3d( ImageHandle image, u32 width, u32 height, u32 depth ) {
    Image* vk_image = get_image( image );

    resize_image_3d( image, width, height, depth, vk_image->mip_level_count );
}

void GpuDevice::resize_image_3d( ImageHandle image, u32 width, u32 height, u32 depth, u32 mip_levels ) {

    Image* vk_image = get_image( image );

    if ( vk_image->width == width && vk_image->height == height && vk_image->depth == depth ) {
        return;
    }

    // Queue deletion of image by creating a temporary one
    ImageHandle image_to_delete = images.obtain();
    Image* vk_image_to_delete = get_image( image_to_delete );

    // Cache all informations (image, image view, flags, ...) into image to delete.
    // Missing even one information (like it is a image view, sparse, ...)
    // can lead to memory leaks.
    memory_copy( vk_image_to_delete, vk_image, sizeof( Image ) );
    // Update handle so it can be used to update bindless to dummy image
    // and delete the old image and image view.
    vk_image_to_delete->handle = image_to_delete;

    // Re-create image in place.
    ImageCreation tc;
    tc.set_flags( vk_image->flags ).set_format_type( vk_image->vk_format, vk_image->type )
        .set_name( vk_image->name ).set_size( width, height, depth )
        .set_mips( mip_levels );
    vulkan_create_image( *this, tc, vk_image->handle, vk_image );

    destroy_image( image_to_delete );
}

void GpuDevice::recreate_image_view( ImageViewHandle image_view ) {

    ImageView* vk_image_view = get_image_view( image_view );

    ImageViewHandle image_view_to_delete = image_views.obtain();
    ImageView* vk_image_view_to_delete = get_image_view( image_view_to_delete );

    memory_copy( vk_image_view_to_delete, vk_image_view, sizeof( ImageView ) );
    vk_image_view_to_delete->handle = image_view_to_delete;

    destroy_image_view( image_view_to_delete );

    // Re-create image view in place
    Image* parent_image = get_image( vk_image_view->parent_image );
    //// Create the image view
    VkImageViewCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    info.image = parent_image->vk_image;
    info.format = parent_image->vk_format;
    info.viewType = vk_image_view->view_type;
    info.subresourceRange = vk_image_view->subresource_range;
    check( vkCreateImageView( vulkan_device, &info, vulkan_allocation_callbacks, &vk_image_view->vk_image_view ) );

    set_resource_name( VK_OBJECT_TYPE_IMAGE_VIEW, (u64)vk_image_view->vk_image_view, vk_image_view->name );
}

PagePoolHandle GpuDevice::allocate_image_pool( ImageHandle image_handle, u32 pool_size ) {

    Image* image = get_image( image_handle );
    if ( image == nullptr ) {
        RASSERT( false );
        return {};
    }

    PagePoolHandle pool_handle = page_pools_pool.obtain();
    if ( pool_handle.is_invalid() ) {
        return pool_handle;
    }

    PagePool* page_pool = get_page_pool( pool_handle );
    RASSERT( image->sparse );

    // TODO(marco):
    // VkSparseMemoryBind
    // VkSparseImageMemoryBind
    // vkQueueBindSparse

    u32 property_count = 0;

    VkPhysicalDeviceSparseImageFormatInfo2 format_info{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SPARSE_IMAGE_FORMAT_INFO_2 };
    format_info.format = image->vk_format;
    format_info.type = to_vk_image_type( image->type );
    format_info.samples = VK_SAMPLE_COUNT_1_BIT;
    format_info.usage = image->vk_usage;
    format_info.tiling = VK_IMAGE_TILING_OPTIMAL;

    vkGetPhysicalDeviceSparseImageFormatProperties2( vulkan_physical_device, &format_info, &property_count, nullptr );

    RASSERT( property_count > 0 );

    Array<VkSparseImageFormatProperties2> properties;
    properties.init( allocator, property_count, property_count );
    memset( properties.data, 0, sizeof( VkSparseImageFormatProperties2 ) * property_count );

    for ( u32 p = 0; p < property_count; ++p ) {
        properties[ p ].sType = VK_STRUCTURE_TYPE_SPARSE_IMAGE_FORMAT_PROPERTIES_2;
        properties[ p ].pNext = nullptr;
    }

    vkGetPhysicalDeviceSparseImageFormatProperties2( vulkan_physical_device, &format_info, &property_count, properties.data );

    u32 block_width = properties[ 0 ].properties.imageGranularity.width;
    u32 block_height = properties[ 0 ].properties.imageGranularity.height;

    properties.shutdown();

    VkImageSparseMemoryRequirementsInfo2 sparse_memory_requirement_info{ VK_STRUCTURE_TYPE_IMAGE_SPARSE_MEMORY_REQUIREMENTS_INFO_2 };
    sparse_memory_requirement_info.image = image->vk_image;

    VkMemoryRequirements memory_requirements{ };
    vkGetImageMemoryRequirements( vulkan_device, image->vk_image, &memory_requirements );

    u32 block_count = pool_size / ( block_width * block_height );

    page_pool->block_width = block_width;
    page_pool->block_height = block_height;
    page_pool->block_size = ( u32 )memory_requirements.alignment; // NOTE(marco): alignment corresponds to block size for sparse textures
    page_pool->used_pages = 0;
    page_pool->free_list = nullptr;
    page_pool->size = pool_size;

    page_pool->vma_allocations.init( allocator, block_count, block_count );
    page_pool->allocations.init( allocator, block_count, block_count );

    VmaAllocationCreateInfo allocation_create_info{ };
    allocation_create_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    VkMemoryRequirements page_memory_requirements;
    page_memory_requirements.memoryTypeBits = memory_requirements.memoryTypeBits;
    page_memory_requirements.alignment = memory_requirements.alignment;
    page_memory_requirements.size = memory_requirements.alignment;

    vmaAllocateMemoryPages( vma_allocator, &page_memory_requirements, &allocation_create_info, block_count, page_pool->vma_allocations.data, nullptr );

    return pool_handle;
}

void GpuDevice::destroy_page_pool( PagePoolHandle pool_handle ) {

    PagePool* page_pool = get_page_pool( pool_handle );
    if ( page_pool ) {

        //resource_tracker.track_destroy_resource( ResourceUpdateType::PagePool, pool_handle.index );

        resource_deletion_queue.push( { ResourceUpdateType::PagePool, pool_handle.id, current_frame + k_max_frames, 1 } );
    } else {
        rprint( "Graphics error: trying to free invalid PagePool %u\n", pool_handle.index() );
    }
}

void GpuDevice::destroy_page_pool_instant( ResourceHandle raw_handle ) {

    PagePoolHandle handle;
    handle.id = raw_handle;

    PagePool* page_pool = get_page_pool( handle );
    if ( page_pool ) {
        vmaFreeMemoryPages( vma_allocator, page_pool->vma_allocations.size, page_pool->vma_allocations.data );

        page_pool->vma_allocations.shutdown();
        page_pool->allocations.shutdown();

        page_pools_pool.destroy( handle );
    }
}

void GpuDevice::reset_pool( PagePoolHandle pool_handle ) {
    PagePool* page_pool = get_page_pool( pool_handle );
    if ( page_pool == nullptr ) {
        RASSERT( false );
        return;
    }

    page_pool->used_pages = 0;
    page_pool->free_list = nullptr;
}

void GpuDevice::bind_image_pages( PagePoolHandle pool_handle, ImageHandle image_handle, u32 x, u32 y, u32 width, u32 height, u32 layer ) {
    PagePool* page_pool = get_page_pool( pool_handle );
    if ( page_pool == nullptr ) {
        RASSERT( false );
        return;
    }

    Image* image = get_image( image_handle );
    if ( image == nullptr ) {
        RASSERT( false );
        return;
    }

    RASSERT( image->sparse );

    u32 block_width = page_pool->block_width;
    u32 block_height = page_pool->block_height;
    u32 num_blocks_x = width / block_width;
    u32 num_blocks_y = height / block_height;
    u32 num_blocks = num_blocks_x * num_blocks_y;

    if ( page_pool->used_pages + num_blocks >= page_pool->allocations.size ) {
        RASSERT( false );
        return;
    }

    u32 array_offset = pending_sparse_queue_binds.size;

    VkImageAspectFlags aspect = TextureFormat::has_depth( image->vk_format ) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    for ( u32 block_y = 0; block_y < num_blocks_y; ++block_y ) {
        for ( u32 block_x = 0; block_x < num_blocks_x; ++block_x ) {
            VkSparseImageMemoryBind sparse_bind{ };

            VmaAllocation allocation = page_pool->vma_allocations[ page_pool->used_pages++ ];
            VmaAllocationInfo allocation_info{ };
            vmaGetAllocationInfo( vma_allocator, allocation, &allocation_info );

            i32 dest_x = (i32)( block_x * block_width + x );
            i32 dest_y = (i32)( block_y * block_height + y );

            sparse_bind.subresource.aspectMask = aspect;
            sparse_bind.subresource.arrayLayer = layer;
            sparse_bind.offset = { dest_x, dest_y, 0 };
            sparse_bind.extent = { block_width, block_height, 1 };
            sparse_bind.memory = allocation_info.deviceMemory;
            sparse_bind.memoryOffset = allocation_info.offset;

            pending_sparse_queue_binds.push( sparse_bind );
        }
    }

    SparseMemoryBindInfo bind_info{ };
    bind_info.image = image->vk_image;
    bind_info.binding_array_offset = array_offset;
    bind_info.count = num_blocks;

    pending_sparse_memory_info.push( bind_info );
}


//
//
//
//void GpuDevice::fill_barrier( FramebufferHandle framebuffer, ExecutionBarrier& out_barrier ) {
//
//    Framebuffer* vk_framebuffer = access_framebuffer( framebuffer );
//
//    out_barrier.num_image_barriers = 0;
//
//    if ( vk_framebuffer ) {
//        const u32 rts = vk_framebuffer->num_color_attachments;
//        for ( u32 i = 0; i < rts; ++i ) {
//            out_barrier.image_barriers[ out_barrier.num_image_barriers++ ].image = vk_framebuffer->color_attachments[ i ];
//        }
//
//        if ( vk_framebuffer->depth_stencil_attachment.index != k_invalid_index ) {
//            out_barrier.image_barriers[ out_barrier.num_image_barriers++ ].image = vk_framebuffer->depth_stencil_attachment;
//        }
//    }
//}

bool GpuDevice::buffer_ready( BufferHandle buffer_ ) {
    Buffer* buffer = get_buffer( buffer_ );
    return buffer->ready;
}

void GpuDevice::wait_for_previous_frame() {
    // Fence wait and reset
    if ( absolute_frame >= k_max_frames ) {
        u64 graphics_timeline_value = absolute_frame - ( k_max_frames - 1 );
        u64 compute_timeline_value = last_compute_semaphore_value;

        u64 wait_values[]{ graphics_timeline_value, compute_timeline_value };

        VkSemaphore semaphores[]{ vulkan_graphics_timeline_semaphore, vulkan_compute_timeline_semaphore };

        VkSemaphoreWaitInfo semaphore_wait_info{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        semaphore_wait_info.semaphoreCount = has_async_work ? 2 : 1;
        semaphore_wait_info.pSemaphores = semaphores;
        semaphore_wait_info.pValues = wait_values;

        vkWaitSemaphores( vulkan_device, &semaphore_wait_info, ~0ull );
    }
}

VkResult GpuDevice::acquire_next_swapchain_image() {
    VkSemaphore image_acquired_semaphore = get_current_image_acquired_semaphore();
    return vkAcquireNextImageKHR( vulkan_device, vulkan_swapchain, UINT64_MAX, image_acquired_semaphore, VK_NULL_HANDLE, &vulkan_image_index );
}

void GpuDevice::update_descriptors() {
    // Descriptor Set Updates
    if ( descriptor_set_updates.size ) {
        for ( i32 i = descriptor_set_updates.size - 1; i >= 0; i-- ) {
            DescriptorSetUpdate& update = descriptor_set_updates[ i ];

            //if ( update.frame_issued == current_frame )
            {
                update_descriptor_set_instant( update );

                update.frame_issued = u32_max;
                descriptor_set_updates.delete_swap( i );
            }
        }
    }
}

void GpuDevice::reset_pools() {

    // Command pool reset
    command_buffer_manager.reset_pools( current_frame );
    // Dynamic memory update
    const u32 used_size = dynamic_allocated_size - ( dynamic_per_frame_size * previous_frame );
    dynamic_max_per_frame_size = raptor_max( used_size, dynamic_max_per_frame_size );
    dynamic_allocated_size = dynamic_per_frame_size * current_frame;

    gpu_profiler->reset_pools( current_frame );
}

void GpuDevice::update_bindless_resources() {
    DescriptorSetLayout* layout = get_descriptor_set_layout( bindless_descriptor_set_layout );
    
    // Handle deferred writes to bindless image views.
    if ( image_views_to_update_bindless.size ) {

        VkWriteDescriptorSet bindless_descriptor_writes[ k_max_bindless_resources ];
        VkDescriptorImageInfo bindless_image_info[ k_max_bindless_resources ];

        ImageView* vk_dummy_image_view = get_image_view( dummy_image_view );

        u32 current_write_index = 0;
        for ( i32 it = image_views_to_update_bindless.size - 1; it >= 0; it-- ) {
            ImageViewUpdate& image_view_to_update = image_views_to_update_bindless[ it ];

            //if ( texture_to_update.current_frame == current_frame )
            {
                ImageView* image_view = get_image_view( image_view_to_update.handle );

                if ( image_view == nullptr ) {
                    continue;
                }

                if ( image_view->vk_image_view == VK_NULL_HANDLE ) {
                    continue;
                }

                // Handles should be the same.
                RASSERT( image_view->handle == image_view_to_update.handle );

                Sampler* vk_default_sampler = get_sampler( global_samplers[ GlobalSamplers::LinearClamp ] );
                VkDescriptorImageInfo& descriptor_image_info = bindless_image_info[ current_write_index ];

                // Update image view and sampler if valid
                if ( !image_view_to_update.deleting ) {
                    descriptor_image_info.imageView = image_view->vk_image_view;

                    Image* image = get_image( image_view->parent_image );
                    if ( image->sampler != nullptr ) {
                        descriptor_image_info.sampler = image->sampler->vk_sampler;
                    } else
                    {
                        descriptor_image_info.sampler = vk_default_sampler->vk_sampler;
                    }
                } else {
                    // Deleting: set to default image view and sampler in the current slot.
                    descriptor_image_info.imageView = vk_dummy_image_view->vk_image_view;
                    descriptor_image_info.sampler = vk_default_sampler->vk_sampler;
                }

                descriptor_image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                VkWriteDescriptorSet& descriptor_write = bindless_descriptor_writes[ current_write_index ];
                descriptor_write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                descriptor_write.descriptorCount = 1;
                descriptor_write.dstArrayElement = image_view_to_update.handle.index();
                descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                descriptor_write.dstSet = vulkan_bindless_descriptor_set_cached;
                descriptor_write.dstBinding = k_bindless_texture_binding;
                descriptor_write.pImageInfo = &descriptor_image_info;

                image_view_to_update.current_frame = u32_max;
                // Cache this value, as delete_swap will modify the texture_to_update reference.
                const bool add_image_view_to_delete = image_view_to_update.deleting;
                image_views_to_update_bindless.delete_swap( it );

                ++current_write_index;

                // Debug
                //if ( strcmp("", image->name) == 0 ) {
                    //rprint( "%s image %u\n", add_texture_to_delete ? "Deleting" : "Updating", image->handle.index );
                //}

                // Add optional compute bindless descriptor update
                //if ( image->flags & TextureFlags::Compute_mask ) {
                if ( image_view->compute_access ) {
                    VkWriteDescriptorSet& descriptor_write_image = bindless_descriptor_writes[ current_write_index ];
                    VkDescriptorImageInfo& descriptor_image_info_compute = bindless_image_info[ current_write_index ];
                    descriptor_image_info_compute = descriptor_image_info;
                    descriptor_image_info_compute.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

                    // Copy common data from descriptor and image info
                    descriptor_write_image = descriptor_write;

                    descriptor_write_image.dstBinding = k_bindless_image_binding;
                    descriptor_write_image.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                    descriptor_write_image.pImageInfo = &descriptor_image_info_compute;

                    ++current_write_index;
                }
            }
        }

        if ( current_write_index ) {
            vkUpdateDescriptorSets( vulkan_device, current_write_index, bindless_descriptor_writes, 0, nullptr );
        }
    }

    // Handle deferred writes to bindless buffers.
    if ( buffers_to_update_bindless.size ) {
        VkWriteDescriptorSet bindless_descriptor_writes[ k_max_bindless_resources ];
        VkDescriptorBufferInfo bindless_buffer_info[ k_max_bindless_resources ];

        Buffer* vk_dummy_buffer = get_buffer( dummy_storage_buffer );

        u32 current_write_index = 0;
        for ( i32 it = buffers_to_update_bindless.size - 1; it >= 0; it-- ) {
            BufferUpdate& buffer_to_update = buffers_to_update_bindless[ it ];
            //if ( buffer_to_update.current_frame == current_frame )
            {
                Buffer* buffer = get_buffer( buffer_to_update.handle );
                if ( buffer->vk_buffer == VK_NULL_HANDLE ) {
                    continue;
                }

                VkWriteDescriptorSet& descriptor_write = bindless_descriptor_writes[ current_write_index ];
                descriptor_write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                descriptor_write.descriptorCount = 1;
                descriptor_write.dstArrayElement = buffer_to_update.handle.index();
                descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                descriptor_write.dstSet = vulkan_bindless_descriptor_set_cached;
                descriptor_write.dstBinding = k_bindless_ssbo_binding;
                // Handles should be the same.
                RASSERT( buffer->handle == buffer_to_update.handle );
                VkDescriptorBufferInfo& descriptor_buffer_info = bindless_buffer_info[ current_write_index ];
                // Update image view and sampler if valid
                if ( !buffer_to_update.deleting ) {
                    descriptor_buffer_info.buffer = buffer->vk_buffer;
                    descriptor_buffer_info.offset = 0;
                    descriptor_buffer_info.range = VK_WHOLE_SIZE;
                } else {
                    // Deleting: set to default dummy buffer in the current slot.
                    descriptor_buffer_info.buffer = vk_dummy_buffer->vk_buffer;
                    descriptor_buffer_info.offset = 0;
                    descriptor_buffer_info.range = VK_WHOLE_SIZE;
                }

                descriptor_write.pBufferInfo = &descriptor_buffer_info;

                buffer_to_update.current_frame = u32_max;
                // Cache this value, as delete_swap will modify the image_view_to_update reference.
                const bool add_buffer_to_delete = buffer_to_update.deleting;
                buffers_to_update_bindless.delete_swap( it );
                ++current_write_index;


                // Debug
                //if ( strcmp("", image->name) == 0 ) {
                    //rprint( "%s image %u\n", add_texture_to_delete ? "Deleting" : "Updating", image->handle.index );
                //}
                if ( add_buffer_to_delete ) {
                    resource_deletion_queue.push( { ResourceUpdateType::Buffer, buffer->handle.id, current_frame, 1 } );
                }
            }
        }

        if ( current_write_index ) {
            vkUpdateDescriptorSets( vulkan_device, current_write_index, bindless_descriptor_writes, 0, nullptr );
        }
    }
}

bool GpuDevice::update_sparse_resources() {
    bool has_pending_sparse_bindings = pending_sparse_memory_info.size > 0;

    if ( has_pending_sparse_bindings ) {
        // TODO(marco): use fence or semaphores
        check( vkQueueWaitIdle( vulkan_main_queue ) );

        Array<VkSparseImageMemoryBindInfo> sparse_binding_infos;
        sparse_binding_infos.init( allocator, pending_sparse_memory_info.size, pending_sparse_memory_info.size );

        for ( u32 b = 0; b < pending_sparse_memory_info.size; ++b ) {
            SparseMemoryBindInfo& internal_info = pending_sparse_memory_info[ b ];

            VkSparseImageMemoryBindInfo& info = sparse_binding_infos[ b ];
            info.image = internal_info.image;
            info.bindCount = internal_info.count;
            info.pBinds = pending_sparse_queue_binds.data + internal_info.binding_array_offset;
        }

        VkBindSparseInfo sparse_info{ VK_STRUCTURE_TYPE_BIND_SPARSE_INFO };
        sparse_info.imageBindCount = sparse_binding_infos.size;
        sparse_info.pImageBinds = sparse_binding_infos.data;
        sparse_info.signalSemaphoreCount = 1;
        sparse_info.pSignalSemaphores = &vulkan_bind_binary_semaphore;

        check( vkQueueBindSparse( vulkan_main_queue, 1, &sparse_info, VK_NULL_HANDLE ) );

        sparse_binding_infos.shutdown();

        pending_sparse_memory_info.clear();
        pending_sparse_queue_binds.clear();

        return true;
    }

    return false;
}

CommandBuffer* GpuDevice::flush_acceleration_structure_builds() {
    const bool has_blas = blas_build_requests.size > 0;
    const bool has_tlas = tlas_build_requests.size > 0;

    if ( !has_blas && !has_tlas ) {
        return nullptr;
    }

    CommandBuffer* cb = allocate_command_buffer( 0, current_frame, CommandQueueType::Compute );

    cb->begin();

    if ( has_blas ) {

        ArenaAllocator* temp_allocator = MemoryService::instance()->get_thread_allocator();
        sizet base_marker = temp_allocator->get_marker();

        Array<VkAccelerationStructureBuildGeometryInfoKHR> build_infos;
        Array<VkAccelerationStructureBuildRangeInfoKHR*> build_ranges;

        build_infos.init( temp_allocator, blas_build_requests.size,
                          blas_build_requests.size );
        build_ranges.init( temp_allocator, blas_build_requests.size,
                           blas_build_requests.size );

        for ( u32 i = 0; i < blas_build_requests.size; ++i ) {
            BLASBuildInfo& build = blas_build_requests[ i ];

            build_infos[ i ] = build.build_info;
            // Cache pointer here, as grow/realloc of build_infos/build_ranges arrays will invalidate it.
            build_infos[ i ].pGeometries = build.geometries.data;
            build_ranges[ i ] = build.ranges.data;
        }

        vkCmdBuildAccelerationStructuresKHR( cb->vk_command_buffer, blas_build_requests.size,
                                             build_infos.data, build_ranges.data );

        temp_allocator->free_marker( base_marker );
    }

    if ( has_blas && has_tlas ) {
        cb->add_memory_barrier( VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                                VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR );
        cb->flush_barriers();
    }

    if ( has_tlas ) {

        cb->add_memory_barrier( VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR );
        cb->flush_barriers();

        ArenaAllocator* temp_allocator = MemoryService::instance()->get_thread_allocator();
        sizet base_marker = temp_allocator->get_marker();

        Array<VkAccelerationStructureBuildGeometryInfoKHR> build_infos;
        Array<VkAccelerationStructureBuildRangeInfoKHR*> build_ranges;

        build_infos.init( temp_allocator, tlas_build_requests.size,
                          tlas_build_requests.size );
        build_ranges.init( temp_allocator, tlas_build_requests.size,
                           tlas_build_requests.size );

        for ( u32 i = 0; i < tlas_build_requests.size; ++i ) {
            TLASBuildInfo& build = tlas_build_requests[ i ];

            build_infos[ i ] = build.build_info;
            // Cache pointer here, as grow/realloc of build_infos/build_ranges arrays will invalidate it.
            build_infos[ i ].pGeometries = &build.geometry;
            build_ranges[ i ] = &build.range;
        }

        vkCmdBuildAccelerationStructuresKHR( cb->vk_command_buffer, tlas_build_requests.size,
                                             build_infos.data, build_ranges.data );

        temp_allocator->free_marker( base_marker );

        cb->add_memory_barrier( VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                                VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR );
        cb->flush_barriers();
    }

    for ( BLASBuildInfo& build_info : blas_build_requests ) {
        build_info.geometries.shutdown();
        build_info.ranges.shutdown();
    }

    blas_build_requests.clear();
    tlas_build_requests.clear();

    cb->end();

    return cb;
}

void GpuDevice::resolve_timestamps() {
    //
    // GPU Timestamp resolve
    if ( timestamps_enabled ) {
        ArenaAllocator* temp_allocator = MemoryService::instance()->get_thread_allocator();
        gpu_profiler->get_query_pool_results( temp_allocator );
    }
}

void GpuDevice::process_pending_resource_deletion() {
    // Resource deletion using reverse iteration and swap with last element.
    if ( resource_deletion_queue.size > 0 ) {
        for ( i32 i = resource_deletion_queue.size - 1; i >= 0; i-- ) {
            ResourceUpdate& resource_deletion = resource_deletion_queue[ i ];

            if ( resource_deletion.current_frame == current_frame ) {

                switch ( resource_deletion.type ) {

                    case ResourceUpdateType::Buffer:
                    {
                        destroy_buffer_instant( resource_deletion.handle );
                        break;
                    }

                    case ResourceUpdateType::Pipeline:
                    {
                        destroy_pipeline_instant( resource_deletion.handle );
                        break;
                    }

                    case ResourceUpdateType::DescriptorSet:
                    {
                        destroy_descriptor_set_instant( resource_deletion.handle );
                        break;
                    }

                    case ResourceUpdateType::DescriptorSetLayout:
                    {
                        destroy_descriptor_set_layout_instant( resource_deletion.handle );
                        break;
                    }

                    case ResourceUpdateType::Sampler:
                    {
                        destroy_sampler_instant( resource_deletion.handle );
                        break;
                    }

                    case ResourceUpdateType::ShaderState:
                    {
                        destroy_shader_state_instant( resource_deletion.handle );
                        break;
                    }

                    case ResourceUpdateType::Image:
                    {
                        destroy_image_instant( resource_deletion.handle );
                        break;
                    }

                    case ResourceUpdateType::ImageView:
                    {
                        destroy_image_view_instant( resource_deletion.handle );
                        break;
                    }

                    case ResourceUpdateType::PagePool:
                    {
                        destroy_page_pool_instant( resource_deletion.handle );
                        break;
                    }

                    case ResourceUpdateType::PipelineLayout:
                    {
                        destroy_pipeline_layout_instant( resource_deletion.handle );
                        break;
                    }

                    case ResourceUpdateType::BLAS:
                    {
                        destroy_blas_instant( resource_deletion.handle );
                        break;
                    }

                    case ResourceUpdateType::TLAS:
                    {
                        destroy_tlas_instant( resource_deletion.handle );
                        break;
                    }
                }

                // Mark resource as free
                resource_deletion.current_frame = u32_max;

                // Swap element
                resource_deletion_queue.delete_swap( i );
            }
        }
    }
}

void GpuDevice::present() {
    VkSemaphore render_complete_semaphore = get_current_render_complete_semaphore();

    VkPresentInfoKHR present_info{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &render_complete_semaphore;

    VkSwapchainKHR swap_chains[] = { vulkan_swapchain };
    present_info.swapchainCount = 1;
    present_info.pSwapchains = swap_chains;
    present_info.pImageIndices = &vulkan_image_index;
    present_info.pResults = nullptr; // Optional
    VkResult result = vkQueuePresentKHR( vulkan_main_queue, &present_info );

    RASSERT( result != VK_ERROR_DEVICE_LOST );

    if ( result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || resized ) {
        resized = false;
        resize_swapchain();

        // Advance frame counters that are skipped during this frame.
        frame_counters_advance();

        return;
    }

    //raptor::print_format( "Index %u, %u, %u\n", current_frame, previous_frame, vulkan_image_index );

    // This is called inside resize_swapchain as well to correctly work.
    frame_counters_advance();
}

void GpuDevice::dump_device_fault() {

    if ( !device_fault_enabled  ) {
        rprint( "Device fault information unavailable.\n" );
        return;
    }

    VkDeviceFaultCountsEXT counts{ .sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT };

    VkResult result = vkGetDeviceFaultInfoEXT( vulkan_device, &counts, nullptr );
    if ( result != VK_SUCCESS ) {
        rprint( "vkGetDeviceFaultInfoEXT counts failed: %d\n", result );
        return;
    }

    rprint( "Device fault counts: addresses %u, vendor infos %u, binary %llu\n",
            counts.addressInfoCount, counts.vendorInfoCount, ( u64 )counts.vendorBinarySize );

    Array<VkDeviceFaultAddressInfoEXT> address_infos;
    Array<VkDeviceFaultVendorInfoEXT> vendor_infos;

    VkDeviceFaultAddressInfoEXT* address_infos_data = nullptr;
    VkDeviceFaultVendorInfoEXT* vendor_infos_data = nullptr;
    void* vendor_binary_data = nullptr;

    if ( counts.addressInfoCount ) {
        address_infos.init( allocator, counts.addressInfoCount, counts.addressInfoCount );
        memset( address_infos.data, 0, sizeof( VkDeviceFaultAddressInfoEXT ) * counts.addressInfoCount );

        address_infos_data = address_infos.data;
    }

    if ( counts.vendorInfoCount ) {
        vendor_infos.init( allocator, counts.vendorInfoCount, counts.vendorInfoCount );
        memset( vendor_infos.data, 0, sizeof( VkDeviceFaultVendorInfoEXT ) * counts.vendorInfoCount );

        vendor_infos_data = vendor_infos.data;
    }

    if ( counts.vendorBinarySize ) {
        vendor_binary_data = ralloca( counts.vendorBinarySize, allocator );
        memset( vendor_binary_data, 0, counts.vendorBinarySize );
    }

    VkDeviceFaultInfoEXT fault_info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT,
        .pAddressInfos = address_infos.data,
        .pVendorInfos = vendor_infos.data,
        .pVendorBinaryData = vendor_binary_data
        
    };
    result = vkGetDeviceFaultInfoEXT( vulkan_device, &counts, &fault_info );

    rprint( "\n-------- VULKAN DEVICE FAULT --------\n" );
    rprint( "Result: %d\n", result );
    rprint( "Description: %s\n", fault_info.description );

    for ( u32 i = 0; i < counts.addressInfoCount; ++i ) {
        const VkDeviceFaultAddressInfoEXT& address = address_infos[ i ];

        rprint( "Address %u: type %u, address 0x%llx, precision 0x%llx\n", i, static_cast<u32>( address.addressType ), static_cast<u64>( address.reportedAddress ), static_cast<u64>( address.addressPrecision ) );
    }

    for ( u32 i = 0; i < counts.vendorInfoCount; ++i ) {
        const VkDeviceFaultVendorInfoEXT& vendor = vendor_infos[ i ];

        rprint( "Vendor %u: %s, code 0x%llx, data 0x%llx\n", i, vendor.description, static_cast<u64>( vendor.vendorFaultCode ), static_cast<u64>( vendor.vendorFaultData ) );
    }

    rprint( "--------------------------------\n\n" );
}

void GpuDevice::queue_submit( CommandQueueType queue_type, Span<CommandBuffer*> cbs, Span<VkSemaphoreSubmitInfoKHR> waits, Span<VkSemaphoreSubmitInfoKHR> signals ) {

    // TODO: use temporary allocator
    StaticArray<VkCommandBufferSubmitInfoKHR, 8> command_buffer_infos;

    for ( u32 c = 0; c < cbs.size; c++ ) {
        VkCommandBufferSubmitInfoKHR& cb_info = command_buffer_infos.push_use();
        cb_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO_KHR };
        cb_info.commandBuffer = cbs[ c ]->vk_command_buffer;
    }

    VkSubmitInfo2KHR submit_info{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2_KHR };
    submit_info.waitSemaphoreInfoCount = (u32)waits.size;
    submit_info.pWaitSemaphoreInfos = waits.data;
    submit_info.commandBufferInfoCount = command_buffer_infos.size;
    submit_info.pCommandBufferInfos = command_buffer_infos.data;
    submit_info.signalSemaphoreInfoCount = (u32)signals.size;
    submit_info.pSignalSemaphoreInfos = signals.data;

    VkQueue queue = VK_NULL_HANDLE;

    switch ( queue_type ) {
        case CommandQueueType::Graphics:
            queue = vulkan_main_queue;
            break;

        case CommandQueueType::Compute:
            queue = vulkan_compute_queue;
            break;

        default:
            RASSERT( false );
    }

    VkResult result = vkQueueSubmit2( queue, 1, &submit_info, VK_NULL_HANDLE );

    if ( result == VK_ERROR_DEVICE_LOST ) {
        device_lost = true;

        rprint( "VK_ERROR_DEVICE_LOST returned by vkQueueSubmit2, queue type %u.\n", queue_type );

        dump_device_fault();
    }

    if ( result != VK_SUCCESS ) {
        rprint( "vkQueueSubmit2 failed with result %d, queue type %u.\n", result, queue_type );
    }
}
//
//void GpuDevice::submit_compute_load( CommandBuffer* command_buffer ) {
//    has_async_work = true;
//
//    bool has_wait_semaphore = last_compute_semaphore_value > 0;
//
//    VkSemaphoreSubmitInfoKHR wait_semaphores[]{
//        { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR, nullptr, vulkan_compute_timeline_semaphore, last_compute_semaphore_value, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT_KHR, 0 }
//    };
//
//    last_compute_semaphore_value++;
//
//    VkSemaphoreSubmitInfoKHR signal_semaphores[]{
//        { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR, nullptr, vulkan_compute_timeline_semaphore, last_compute_semaphore_value, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT_KHR, 0 },
//    };
//
//    VkCommandBufferSubmitInfoKHR command_buffer_info{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO_KHR };
//    command_buffer_info.commandBuffer = command_buffer->vk_command_buffer;
//
//    VkSubmitInfo2KHR submit_info{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2_KHR };
//    submit_info.waitSemaphoreInfoCount = has_wait_semaphore ? 1 : 0;
//    submit_info.pWaitSemaphoreInfos = wait_semaphores;
//    submit_info.commandBufferInfoCount = 1;
//    submit_info.pCommandBufferInfos = &command_buffer_info;
//    submit_info.signalSemaphoreInfoCount = 1;
//    submit_info.pSignalSemaphoreInfos = signal_semaphores;
//
//    vkQueueSubmit2( vulkan_compute_queue, 1, &submit_info, VK_NULL_HANDLE );
//}
//
//void GpuDevice::submit_immediate( CommandBuffer* command_buffer ) {
//    //vkCmdEndQuery( command_buffer->vk_command_buffer, command_buffer->thread_frame_pool->vulkan_pipeline_stats_query_pool, 0 );
//
//    command_buffer->end();
//
//    vkResetFences( vulkan_device, 1, &vulkan_immediate_fence );
//
//    VkSubmitInfo submit_info = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
//    submit_info.waitSemaphoreCount = 0;
//    submit_info.pWaitSemaphores = nullptr;
//    submit_info.pWaitDstStageMask = nullptr;
//    submit_info.commandBufferCount = 1;
//    submit_info.pCommandBuffers = &command_buffer->vk_command_buffer;
//    submit_info.signalSemaphoreCount = 0;
//    submit_info.pSignalSemaphores = nullptr;
//
//    vkQueueSubmit( vulkan_main_queue, 1, &submit_info, vulkan_immediate_fence );
//
//    if ( vkGetFenceStatus( vulkan_device, vulkan_immediate_fence ) != VK_SUCCESS ) {
//        vkWaitForFences( vulkan_device, 1, &vulkan_immediate_fence, VK_TRUE, UINT64_MAX );
//    }
//}

static VkPresentModeKHR to_vk_present_mode( PresentMode::Enum mode ) {
    switch ( mode ) {
        case PresentMode::VSyncFast:
            return VK_PRESENT_MODE_MAILBOX_KHR;
        case PresentMode::VSyncRelaxed:
            return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
        case PresentMode::Immediate:
            return VK_PRESENT_MODE_IMMEDIATE_KHR;
        case PresentMode::VSync:
        default:
            return VK_PRESENT_MODE_FIFO_KHR;
    }
}

void GpuDevice::set_present_mode( PresentMode::Enum mode ) {

    // Request a certain mode and confirm that it is available. If not use VK_PRESENT_MODE_FIFO_KHR which is mandatory
    u32 supported_count = 0;

    static VkPresentModeKHR supported_mode_allocated[ 8 ];
    vkGetPhysicalDeviceSurfacePresentModesKHR( vulkan_physical_device, vulkan_window_surface, &supported_count, NULL );
    RASSERT( supported_count < 8 );
    vkGetPhysicalDeviceSurfacePresentModesKHR( vulkan_physical_device, vulkan_window_surface, &supported_count, supported_mode_allocated );

    bool mode_found = false;
    VkPresentModeKHR requested_mode = to_vk_present_mode( mode );
    for ( u32 j = 0; j < supported_count; j++ ) {
        if ( requested_mode == supported_mode_allocated[ j ] ) {
            mode_found = true;
            break;
        }
    }

    // Default to VK_PRESENT_MODE_FIFO_KHR that is guaranteed to always be supported
    vulkan_present_mode = mode_found ? requested_mode : VK_PRESENT_MODE_FIFO_KHR;
    // Use 4 for immediate ?
    vulkan_swapchain_image_count = 3;// vulkan_present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? 2 : 3;

    present_mode = mode_found ? mode : PresentMode::VSync;
}

void GpuDevice::link_image_sampler( ImageHandle image, SamplerHandle sampler ) {

    RASSERT( sampler.is_valid() );
    RASSERT( image.is_valid() );

    Image* image_vk = get_image( image );
    Sampler* sampler_vk = get_sampler( sampler );

    image_vk->sampler = sampler_vk;
}

void GpuDevice::set_initial_image_owner( ImageHandle image, QueueType::Enum queue_type ) {
    Image* image_vk = get_image( image );

    switch ( queue_type ) {
        case QueueType::CopyTransfer:
        {
            image_vk->sync_state.owner_queue_family = vulkan_transfer_queue_family;
            break;
        }
        case QueueType::Compute:
        {
            image_vk->sync_state.owner_queue_family = vulkan_compute_queue_family;
            break;
        }

        case QueueType::Graphics:
        default:
        {
            image_vk->sync_state.owner_queue_family = vulkan_main_queue_family;
            break;
        }
    }
}

void GpuDevice::frame_counters_advance() {
    previous_frame = current_frame;
    current_frame = ( current_frame + 1 ) % k_max_frames;

    ++absolute_frame;
}

VkDeviceAddress GpuDevice::get_buffer_device_address( BufferHandle handle ) {
    Buffer* buffer = get_buffer( handle );
    RASSERT( buffer != nullptr );

    VkBufferDeviceAddressInfoKHR device_address_info{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO_KHR };
    device_address_info.buffer = buffer->vk_buffer;

    return vkGetBufferDeviceAddress( vulkan_device, &device_address_info );
}

//
//
CommandBuffer* GpuDevice::allocate_command_buffer( u32 thread_index, u32 frame_index, CommandQueueType queue_type ) {
    CommandBuffer* cb = command_buffer_manager.allocate_command_buffer( frame_index, thread_index, queue_type );
    return cb;
}

//
//
CommandBuffer* GpuDevice::get_secondary_command_buffer( u32 thread_index, u32 frame_index ) {
    CommandBuffer* cb = command_buffer_manager.get_secondary_command_buffer( frame_index, thread_index );
    return cb;
}

// Resource Map/Unmap /////////////////////////////////////////////////////
void* GpuDevice::map_buffer( const MapBufferParameters& parameters ) {
    if ( parameters.buffer.is_invalid() )
        return nullptr;

    Buffer* buffer = get_buffer( parameters.buffer );

    //RASSERTM( !buffer->mapped_data, "Buffer is already persistently mapped." );

    void* data;
    vmaMapMemory( vma_allocator, buffer->vma_allocation, &data );

    return data;
}

void GpuDevice::unmap_buffer( const MapBufferParameters& parameters ) {
    if ( parameters.buffer.is_invalid() )
    {
        return;
     }

    Buffer* buffer = get_buffer( parameters.buffer );
    vmaUnmapMemory( vma_allocator, buffer->vma_allocation );
}

void GpuDevice::flush_buffer( BufferHandle handle, u32 offset, u32 size ) {
    Buffer* buffer = get_buffer( handle );
    vmaFlushAllocation( vma_allocator, buffer->vma_allocation, offset, size );
}

void* GpuDevice::dynamic_allocate( u32 size ) {
    void* mapped_memory = dynamic_mapped_memory + dynamic_allocated_size;
    dynamic_allocated_size += (u32)raptor::memory_align( size, ubo_alignment );
    return mapped_memory;
}

void* GpuDevice::dynamic_buffer_allocate( u32 size, u32 alignment, u32* dynamic_offset ) {
    const u32 required_alignment = raptor::max( ( u32 )ubo_alignment, alignment );

    const u32 aligned_offset = ( u32 )raptor::memory_align( dynamic_allocated_size, required_alignment );
    const u32 allocation_end = aligned_offset + size;
    const u32 frame_end = dynamic_per_frame_size * ( current_frame + 1 );

    RASSERTM( allocation_end <= frame_end, "Dynamic buffer overflow: frame %u, requested %u bytes", current_frame, size );

    *dynamic_offset = aligned_offset;
    dynamic_allocated_size = allocation_end;

    return dynamic_mapped_memory + aligned_offset;
}

// Utility methods ////////////////////////////////////////////////////////

void check_result( VkResult result ) {
    if ( result == VK_SUCCESS ) {
        return;
    }

    rprint( "Vulkan result: code(%u) - '%s'\n", result, string_VkResult( result ) );
    if ( result < 0 ) {
        RASSERTM( false, "Vulkan error: aborting." );
    }
}
// Device /////////////////////////////////////////////////////////////////

BufferHandle GpuDevice::get_fullscreen_vertex_buffer() const {
    return fullscreen_vertex_buffer;
}

ImageHandle GpuDevice::get_current_swapchain_image() const {
    return vulkan_swapchain_images[ vulkan_image_index ];
}

ImageViewHandle GpuDevice::get_current_swapchain_image_view() const {
    return vulkan_swapchain_image_views[ vulkan_image_index ];
}

ImageHandle GpuDevice::get_current_swapchain_depth_image() const {
    return vulkan_swapchain_depth_images[ vulkan_image_index ];
}

ImageViewHandle GpuDevice::get_current_swapchain_depth_image_view() const {
    return vulkan_swapchain_depth_image_views[ vulkan_image_index ];
}

BufferHandle GpuDevice::get_dummy_constant_buffer() const {
    return dummy_constant_buffer;
}

u64 GpuDevice::compute_frame_limiter_wait_value() const {
    const u64 next_frame_value = absolute_frame + 1;
    return ( next_frame_value > k_max_frames ) ? ( next_frame_value - k_max_frames ) : 0;
}

GpuSubmitSync GpuDevice::build_present_sync( VkPipelineStageFlags2 acquired_stage,
                                             VkPipelineStageFlags2 present_stage ) const {
    GpuSubmitSync out = build_frame_limiter_sync();

    // Wait image acquired (binary semaphore, thus value is 0).
    out.waits.push( build_semaphore_submit( get_current_image_acquired_semaphore(),
                                            0, acquired_stage ) );

    // Signal render complete semaphore (binary semaphore, thus value is 0).
    out.signals.push( build_semaphore_submit( get_current_render_complete_semaphore(),
                                              0, present_stage ) );

    // Signal frame completion timeline semaphore
    const u64 next_frame_value = absolute_frame + 1;
    out.signals.push( build_semaphore_submit( vulkan_graphics_timeline_semaphore,
                                              next_frame_value,
                                              present_stage ) );

    return out;
}

GpuSubmitSync GpuDevice::build_frame_limiter_sync() const {

    GpuSubmitSync out{};
    out.waits.clear();
    out.signals.clear();

    const u64 frame_limiter_value = compute_frame_limiter_wait_value();

    // Wait for frame completion timeline semaphore
    out.waits.push( build_semaphore_submit( vulkan_graphics_timeline_semaphore,
                                            frame_limiter_value,
                                            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT ) );
    return out;
}

GpuSubmitSync GpuDevice::build_compute_sync( u64 compute_signal_value,
                                             VkPipelineStageFlags2 signal_stage ) const {

    GpuSubmitSync out{};
    out.waits.clear();
    out.signals.clear();

    out.signals.push( build_semaphore_submit( vulkan_compute_timeline_semaphore,
                                              compute_signal_value,
                                              signal_stage ) );
    return out;
}

void GpuDevice::resize( u16 width, u16 height ) {
    swapchain_width = width;
    swapchain_height = height;

    resized = true;
}


// Resource Access ////////////////////////////////////////////////////////
ShaderState* GpuDevice::get_shader_state( ShaderStateHandle shader ) {
    return shader_states.get( shader );
}

const ShaderState* GpuDevice::get_shader_state( ShaderStateHandle shader ) const {
    return shader_states.get( shader );
}

Image* GpuDevice::get_image( ImageHandle texture ) {
    return images.get( texture );
}

const Image* GpuDevice::get_image( ImageHandle texture ) const {
    return images.get( texture );
}

ImageView* GpuDevice::get_image_view( ImageViewHandle image_view ) {
    return image_views.get( image_view );
}

const ImageView* GpuDevice::get_image_view( ImageViewHandle image_view ) const {
    return image_views.get( image_view );
}

Buffer* GpuDevice::get_buffer( BufferHandle handle ) {
    return buffers_pool.get( handle );
}

const Buffer* GpuDevice::get_buffer( BufferHandle handle ) const {
    return buffers_pool.get( handle );
}

Pipeline* GpuDevice::get_pipeline( PipelineHandle handle ) {
    return pipelines.get( handle );
}

const Pipeline* GpuDevice::get_pipeline( PipelineHandle handle ) const {
    return pipelines.get( handle );
}

Sampler* GpuDevice::get_sampler( SamplerHandle sampler ) {
    return samplers.get( sampler );
}

const Sampler* GpuDevice::get_sampler( SamplerHandle sampler ) const {
    return samplers.get( sampler );
}

DescriptorSetLayout* GpuDevice::get_descriptor_set_layout( DescriptorSetLayoutHandle descriptor_set_layout ) {
    return descriptor_set_layouts_pool.get( descriptor_set_layout );
}

const DescriptorSetLayout* GpuDevice::get_descriptor_set_layout( DescriptorSetLayoutHandle descriptor_set_layout ) const {
    return descriptor_set_layouts_pool.get( descriptor_set_layout );
}

DescriptorSetLayoutHandle GpuDevice::get_descriptor_set_layout( PipelineHandle pipeline_handle, int layout_index ) {
    Pipeline* pipeline = get_pipeline( pipeline_handle );
    RASSERT( pipeline != nullptr );
    PipelineLayout* layout = get_pipeline_layout( pipeline->layout );

    return  layout->descriptor_set_layout_handles[ layout_index ];
}

DescriptorSetLayoutHandle GpuDevice::get_descriptor_set_layout( PipelineHandle pipeline_handle, int layout_index ) const {
    const Pipeline* pipeline = get_pipeline( pipeline_handle );
    RASSERT( pipeline != nullptr );

    const PipelineLayout* layout = get_pipeline_layout( pipeline->layout );
    return  layout->descriptor_set_layout_handles[ layout_index ];
}

DescriptorSet* GpuDevice::get_descriptor_set( DescriptorSetHandle descriptor_set ) {
    return descriptor_sets_pool.get( descriptor_set );
}

const DescriptorSet* GpuDevice::get_descriptor_set( DescriptorSetHandle descriptor_set ) const {
    return descriptor_sets_pool.get( descriptor_set );
}

PipelineLayout* GpuDevice::get_pipeline_layout( PipelineLayoutHandle layout ) {
    return pipeline_layouts.get( layout );
}

const PipelineLayout* GpuDevice::get_pipeline_layout( PipelineLayoutHandle layout ) const {
    return pipeline_layouts.get( layout );
}

PagePool* GpuDevice::get_page_pool( PagePoolHandle page_pool ) {
    return page_pools_pool.get( page_pool );
}

const PagePool* GpuDevice::get_page_pool( PagePoolHandle page_pool ) const {
    return page_pools_pool.get( page_pool );
}

BLAS* GpuDevice::get_blas( BLASHandle blas ) {
    return blases.get( blas );
}

const BLAS* GpuDevice::get_blas( BLASHandle blas ) const {
    return blases.get( blas );
}

TLAS* GpuDevice::get_tlas( TLASHandle tlas ) {
    return tlases.get( tlas );
}

const TLAS* GpuDevice::get_tlas( TLASHandle tlas ) const {
    return tlases.get( tlas );
}

// GpuDeviceCreation //////////////////////////////////////////////////////
GpuDeviceCreation& GpuDeviceCreation::set_window( u32 width_, u32 height_, void* handle ) {
    width = (u16)width_;
    height = (u16)height_;
    window = handle;
    return *this;
}

GpuDeviceCreation& GpuDeviceCreation::set_allocator( Allocator* allocator_ ) {
    allocator = allocator_;
    return *this;
}

GpuDeviceCreation& GpuDeviceCreation::set_num_threads( u32 value ) {
    num_threads = value;
    return *this;
}

} // namespace raptor
