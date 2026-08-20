#pragma once

#include "foundation/array.hpp"
#include "foundation/platform.hpp"
#include "foundation/span.hpp"
#include "foundation/string_view.hpp"
#include "foundation/static_array.hpp"
#include "foundation/pool_handle.hpp"

#include "graphics/gpu_enum.hpp"

#include "external/volk.h"
#include "external/vk_mem_alloc.h"

#undef near
#undef far

VK_DEFINE_HANDLE( VmaAllocation )
struct VmaBudget;

namespace raptor {

namespace spirv {
    struct ParseResult;
} // namespace spirv

struct Allocator;
struct GpuDevice;
struct StringBuffer;

static const u32                    k_invalid_index = 0xffffffff;
static const u32                    k_invalid_texture_index = u16_max;

typedef u32                         ResourceHandle;

using BufferHandle                  = Handle<struct BufferTag>;
using DescriptorSetLayoutHandle     = Handle<struct DescriptorSetLayoutTag>;
using DescriptorSetHandle           = Handle<struct DescriptorSetTag>;
using ImageHandle                   = Handle<struct ImageTag>;
using ImageViewHandle               = Handle<struct ImageViewTag>;
using PagePoolHandle                = Handle<struct PagePoolTag>;
using PipelineHandle                = Handle<struct PipelineTag>;
using PipelineLayoutHandle          = Handle<struct PipelineLayoutTag>;
using SamplerHandle                 = Handle<struct SamplerTag>;
using ShaderStateHandle             = Handle<struct ShaderStateTag>;
using BLASHandle                    = Handle<struct BLASTag>;
using TLASHandle                    = Handle<struct TLASTag>;


// Consts ///////////////////////////////////////////////////////////////////////

static const u8                     k_max_image_outputs = 8;                // Maximum number of images/render_targets/fbo attachments usable.
static const u8                     k_max_descriptor_set_layouts = 4;       // Maximum number of layouts in the pipeline.
static const u8                     k_max_shader_stages = 5;                // Maximum simultaneous shader stages. Applicable to all different type of pipelines.
static const u8                     k_max_descriptors_per_set = 32;         // Maximum list elements for both descriptor set layout and descriptor sets.
static const u8                     k_max_vertex_bindings = 8;
static const u8                     k_max_vertex_attributes = 8;
static const u8                     k_max_headers = 8;
static const u8                     k_max_defines = 8;
static const u8                     k_max_specialization_constants = 4; // Must match spirv::k_max_specialization_constants

static const u32                    k_submit_header_sentinel = 0xfefeb7ba;
static const u32                    k_max_resource_deletions = 64;

// Resource creation structs ////////////////////////////////////////////////////

//
//
struct Rect2D {
    f32                             x = 0.0f;
    f32                             y = 0.0f;
    f32                             width = 0.0f;
    f32                             height = 0.0f;
}; // struct Rect2D

//
//
struct Rect2DInt {
    i16                             x = 0;
    i16                             y = 0;
    u16                             width = 0;
    u16                             height = 0;
}; // struct Rect2D

//
//
struct ClearColor {

    f32                             rgba[ 4 ];
}; // struct ClearColor

//
//
struct ClearDepthStencil {

    f32                             depth_value;
    u8                              stencil_value;
}; // struct ClearDepthStencil

//
//
struct Viewport {
    Rect2DInt                       rect;
    f32                             min_depth = 0.0f;
    f32                             max_depth = 0.0f;
}; // struct Viewport

//
//
struct ViewportState {
    u32                             num_viewports = 0;
    u32                             num_scissors = 0;

    Viewport*                       viewport = nullptr;
    Rect2DInt*                      scissors = nullptr;
}; // struct ViewportState

//
//
struct StencilOperationState {

    VkStencilOp                     fail = VK_STENCIL_OP_KEEP;
    VkStencilOp                     pass = VK_STENCIL_OP_KEEP;
    VkStencilOp                     depth_fail = VK_STENCIL_OP_KEEP;
    VkCompareOp                     compare = VK_COMPARE_OP_ALWAYS;
    u32                             compare_mask = 0xff;
    u32                             write_mask = 0xff;
    u32                             reference = 0xff;

}; // struct StencilOperationState

//
//
struct DepthStencilCreation {

    StencilOperationState           front;
    StencilOperationState           back;
    VkCompareOp                     depth_comparison = VK_COMPARE_OP_ALWAYS;

    u8                              depth_enable = 0;
    u8                              depth_write_enable = 0;
    u8                              stencil_enable = 0;

}; // struct DepthStencilCreation

struct BlendState {

    VkBlendFactor                   source_color        = VK_BLEND_FACTOR_ONE;
    VkBlendFactor                   destination_color   = VK_BLEND_FACTOR_ONE;
    VkBlendOp                       color_operation     = VK_BLEND_OP_ADD;

    VkBlendFactor                   source_alpha        = VK_BLEND_FACTOR_ONE;
    VkBlendFactor                   destination_alpha   = VK_BLEND_FACTOR_ONE;
    VkBlendOp                       alpha_operation     = VK_BLEND_OP_ADD;

    ColorWriteEnabled::Mask         color_write_mask    = ColorWriteEnabled::All_mask;

    u8                              blend_disabled = 0;
    u8                              separate_blend = 0;

}; // struct BlendState

struct BlendStateCreation {

    StaticArray<BlendState, k_max_image_outputs>  blend_states;

    BlendStateCreation&             reset();

}; // BlendStateCreation

//
//
struct RasterizationCreation {

    VkCullModeFlagBits              cull_mode   = VK_CULL_MODE_NONE;
    VkFrontFace                     front       = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkPolygonMode                   fill        = VK_POLYGON_MODE_FILL;
}; // struct RasterizationCreation

//
struct BufferCreation {
    VkDeviceSize                    size                = 0;
    VkBufferUsageFlags              usage               = 0;

    VmaMemoryUsage                  memory_usage        = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VmaAllocationCreateFlags        allocation_flags    = 0;

    cstring                         name                = nullptr;
}; // struct BufferCreation

//
//
struct ImageCreation {

    void*                           initial_data    = nullptr;
    u16                             width           = 1;
    u16                             height          = 1;
    u16                             depth           = 1;
    u16                             array_layer_count = 1;
    u8                              mip_level_count = 1;
    u8                              flags           = 0;    // TextureFlags bitmasks
    u16                             owner_queue_family = u16_max;

    VkFormat                        format          = VK_FORMAT_UNDEFINED;
    TextureType::Enum               type            = TextureType::Texture2D;

    ImageHandle                     alias;

    cstring                         name            = nullptr;

    ImageCreation&                  reset();
    ImageCreation&                  set_size( u16 width, u16 height, u16 depth );
    ImageCreation&                  set_flags( u8 flags );
    ImageCreation&                  set_mips( u32 mip_level_count );
    ImageCreation&                  set_layers( u32 layer_count );
    ImageCreation&                  set_format_type( VkFormat format, TextureType::Enum type );
    ImageCreation&                  set_name( cstring name );
    ImageCreation&                  set_data( void* data );
    ImageCreation&                  set_alias( ImageHandle alias );
    ImageCreation&                  set_owner( u32 value );

}; // struct ImageCreation


//
//
struct ImageSubResource {

    u16                             mip_base_level      = 0;
    u16                             mip_level_count     = 1;
    u16                             array_base_layer    = 0;
    u16                             array_layer_count   = 1;

}; // struct ImageSubResource

//
//
struct ImageViewCreation {

    ImageHandle                     parent_image;

    VkImageViewType                 view_type           = VK_IMAGE_VIEW_TYPE_1D;
    VkImageSubresourceRange         sub_resource;

    cstring                         name                = nullptr;

}; // struct ImageViewCreation

//
//
struct SamplerCreation {

    VkFilter                        min_filter  = VK_FILTER_NEAREST;
    VkFilter                        mag_filter  = VK_FILTER_NEAREST;
    VkSamplerMipmapMode             mip_filter  = VK_SAMPLER_MIPMAP_MODE_NEAREST;

    VkSamplerAddressMode            address_mode_u = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSamplerAddressMode            address_mode_v = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSamplerAddressMode            address_mode_w = VK_SAMPLER_ADDRESS_MODE_REPEAT;

    VkSamplerReductionMode          reduction_mode = VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE;

    cstring                         name        = nullptr;

    SamplerCreation&                set_min_mag_mip( VkFilter min, VkFilter mag, VkSamplerMipmapMode mip );
    SamplerCreation&                set_address_mode_u( VkSamplerAddressMode u );
    SamplerCreation&                set_address_mode_uv( VkSamplerAddressMode u, VkSamplerAddressMode v );
    SamplerCreation&                set_address_mode_uvw( VkSamplerAddressMode u, VkSamplerAddressMode v, VkSamplerAddressMode w );
    SamplerCreation&                set_reduction_mode( VkSamplerReductionMode mode );
    SamplerCreation&                set_name( const char* name );

}; // struct SamplerCreation

//
struct ShaderCompilationStage {

    StringView                      source_code;
    cstring                         source_file_path = nullptr;

    StaticArray<cstring, k_max_headers> headers;  // Headers used in the shader.
    StaticArray<u64, k_max_headers + 1> shader_file_hashes;

    StaticArray<cstring, k_max_defines> defines;

    VkShaderStageFlagBits           type = VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM;
}; // struct ShaderCompilationStage

//
//
struct ShaderCompilationCreation {

    StaticArray<ShaderCompilationStage, k_max_shader_stages> stages;
    StringView                      name;
    u16                             slang_input = 0;
}; // struct ShaderCompilationCreation

//
//
struct ShaderStateCreation {

    StaticArray<VkShaderModuleCreateInfo, k_max_shader_stages> stages;
    StaticArray<VkShaderStageFlagBits, k_max_shader_stages> stage_types;

    cstring                         name            = nullptr;
    StringBuffer*                   names_buffer    = nullptr;  // Used to store shader binding names.

    // Building helpers
    ShaderStateCreation&            reset();
    ShaderStateCreation&            set_name( const char* name );
    ShaderStateCreation&            add_stage( VkShaderModuleCreateInfo& stage, VkShaderStageFlagBits flag );

}; // struct ShaderStateCreation


//
//
struct BLASGeometry {
    VkFormat                        position_format;
    u32                             vertex_stride;
    u32                             max_vertex;

    BufferHandle                    vertex_buffer;
    u32                             vertex_buffer_offset;

    BufferHandle                    index_buffer;
    u32                             index_buffer_offset;
    VkIndexType                     index_type;
    u32                             primitive_count;

    BufferHandle                    transform_buffer;
    u32                             transform_buffer_offset;

    bool                            opaque = true;
}; // struct BLASGeometry

//
//
struct BLASCreation {
    Span<const BLASGeometry>        geometries;
    StringView                      name;
}; // struct BLASCreation

//
//
struct TLASGeometryInstance {
    BLASHandle                      blas;
    VkTransformMatrixKHR            transform;

    u32                             instance_custom_index   = 0;
    u32                             mask                    = 0xff;
    u32                             sbt_record_offset       = 0;
    VkGeometryInstanceFlagsKHR      flags                   = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
};

//
//
struct TLASCreation {
    Span<const TLASGeometryInstance> instances;
    StringView                      name;
};


//
//
struct DescriptorSetLayoutCreation {

    Span<const VkDescriptorSetLayoutBinding>  bindings;

    u32                             set_index   = 0;
    bool                            bindless    = false;
    bool                            dynamic     = false;

    cstring                         name        = nullptr;

}; // struct DescriptorSetLayoutCreation

//
//
struct PipelineLayoutCreation {
    Span<const DescriptorSetLayoutHandle>    layouts;

    VkPushConstantRange             push_constant;

    cstring                         name        = nullptr;
}; // struct PipelineLayoutCreation;

//
struct TextureDescriptor {
    ImageViewHandle                 texture;
    u16                             binding;
};

//
struct TextureArrayDescriptor {
    ImageViewHandle*                textures;
    u16                             binding;
    u16                             count;
};

//
struct BufferDescriptor {
    BufferHandle                    buffer;
    u16                             binding;
};

//
struct SamplerDescriptor {
    SamplerHandle                   sampler;
    u16                             binding;
};

//
struct DynamicBufferBinding {
    u32                             binding;
    u32                             size;
};


struct TLASDescriptor {
    TLASHandle                      tlas;
    u16                             binding;
};

//
//
struct DescriptorSetCreation {

    Span<const TextureDescriptor>   textures;
    Span<const TextureDescriptor>   images;
    Span<const TextureArrayDescriptor> image_arrays;
    Span<const BufferDescriptor>    buffers;
    Span<const BufferDescriptor>    ssbos;
    Span<const SamplerDescriptor>   samplers;
    Span<const DynamicBufferBinding> dynamic_buffers;

    // TODO:
    TLASDescriptor                  tlas;

    DescriptorSetLayoutHandle       layout;
    u32                             set_index = 0;

    cstring                         name;

}; // struct DescriptorSetCreation

//
//
struct DescriptorSetUpdate {
    DescriptorSetHandle             descriptor_set;

    u32                             frame_issued = 0;
}; // DescriptorSetUpdate

//
//
struct VertexInputCreation {

    StaticArray<VkVertexInputBindingDescription, k_max_vertex_bindings> bindings;
    StaticArray<VkVertexInputAttributeDescription, k_max_vertex_attributes> attributes;

}; // struct VertexInputCreation

//
//
struct RenderPassOutput {

    VkFormat                        color_formats[ k_max_image_outputs ];
    VkImageLayout                   color_final_layouts[ k_max_image_outputs ];
    VkAttachmentLoadOp              color_operations[ k_max_image_outputs ];

    VkFormat                        depth_stencil_format;
    VkImageLayout                   depth_stencil_final_layout;

    u32                             shading_rate_image_index = k_invalid_index;

    u32                             num_color_formats       = 0;
    u32                             multiview_mask          = 0;

    VkAttachmentLoadOp              depth_operation         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    VkAttachmentLoadOp              stencil_operation       = VK_ATTACHMENT_LOAD_OP_DONT_CARE;

    RenderPassOutput&               reset();
    RenderPassOutput&               color( VkFormat format, VkImageLayout layout, VkAttachmentLoadOp load_op );
    RenderPassOutput&               depth( VkFormat format, VkImageLayout layout );
    RenderPassOutput&               set_depth_stencil_operations( VkAttachmentLoadOp depth, VkAttachmentLoadOp stencil );
    RenderPassOutput&               set_multiview_mask( u32 mask );
    RenderPassOutput&               add_shading_rate_image();

}; // struct RenderPassOutput

//
//
struct PipelineCreation {

    RasterizationCreation           rasterization;
    DepthStencilCreation            depth_stencil;
    BlendStateCreation              blend_state;
    VertexInputCreation             vertex_input;

    VkPrimitiveTopology             topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineCreateFlags           flags;

    RenderPassOutput                render_pass_output;
    ShaderStateHandle               shader;

    // Pipeline interface
    PipelineLayoutHandle            layout;
    VkSpecializationMapEntry        specialization_entries[ k_max_specialization_constants ];
    u32                             specialization_data[ k_max_specialization_constants ];

    const ViewportState*            viewport            = nullptr;

    u32                             num_active_layouts  = 0;
    u32                             num_specialization_constants = 0;

    cstring                         name                = nullptr;
    cstring                         render_pass_name    = nullptr;

}; // struct PipelineCreation

//
//
struct DescriptorSetLayoutInfo {

    //
    // A single descriptor binding. It can be relative to one or more resources of the same type.
    //
    struct Binding {

        VkDescriptorType            type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
        u16                         index = 0;
        u16                         count = 0;
        cstring                     name = nullptr;  // Comes from external memory.
    }; // struct Binding

    Binding                         bindings[ k_max_descriptors_per_set ];
    u32                             num_bindings = 0;
    u32                             set_index = 0;
    bool                            bindless = false;
    bool                            dynamic = false;

    cstring                         name = nullptr;

    // Building helpers
    DescriptorSetLayoutInfo& reset();
    DescriptorSetLayoutInfo& add_binding( const Binding& binding );
    DescriptorSetLayoutInfo& add_binding( VkDescriptorType type, u32 index, u32 count, cstring name );
    DescriptorSetLayoutInfo& add_binding_at_index( const Binding& binding, int index );
    DescriptorSetLayoutInfo& set_name( cstring name );
    DescriptorSetLayoutInfo& set_set_index( u32 index );

}; // struct DescriptorSetLayoutInfo

// API-agnostic structs /////////////////////////////////////////////////////////

//
// Helper methods for texture formats
//
namespace TextureFormat {

    inline bool                     is_depth_stencil( VkFormat value ) {
        return value >= VK_FORMAT_D16_UNORM_S8_UINT && value < VK_FORMAT_BC1_RGB_UNORM_BLOCK;
    }
    inline bool                     is_depth_only( VkFormat value ) {
        return value >= VK_FORMAT_D16_UNORM && value < VK_FORMAT_S8_UINT;
    }
    inline bool                     is_stencil_only( VkFormat value ) {
        return value == VK_FORMAT_S8_UINT;
    }

    inline bool                     has_depth( VkFormat value ) {
        return is_depth_only(value) || is_depth_stencil( value );
    }
    inline bool                     has_stencil( VkFormat value ) {
        return value >= VK_FORMAT_S8_UINT && value <= VK_FORMAT_D32_SFLOAT_S8_UINT;
    }
    inline bool                     has_depth_or_stencil( VkFormat value ) {
        return value >= VK_FORMAT_D16_UNORM && value <= VK_FORMAT_D32_SFLOAT_S8_UINT;
    }

} // namespace TextureFormat


//
//
struct DescriptorData {

    void*                           data    = nullptr;

}; // struct DescriptorData

//
//
struct DescriptorBinding {

    VkDescriptorType                type;
    u16                             index   = 0;
    u16                             count   = 0;
    u16                             set     = 0;

    cstring                         name    = nullptr;
}; // struct DescriptorBinding

// API-agnostic resource modifications //////////////////////////////////////////

struct MapBufferParameters {
    BufferHandle                    buffer;
    u32                             offset = 0;
    u32                             size = 0;

}; // struct MapBufferParameters

// Updates //////////////////////////////////////////////////////////////

//
//
struct ResourceUpdate {

    ResourceUpdateType::Enum        type;
    ResourceHandle                  handle;
    u32                             current_frame;
    u32                             deleting;
}; // struct ResourceUpdate

//
//
struct BufferUpdate {
    BufferHandle                    handle;
    u32                             current_frame;
    u32                             deleting;
}; // struct BufferUpdate

//
//
struct ImageViewUpdate {
    ImageViewHandle                 handle;
    u32                             current_frame;
    u32                             deleting;
}; // struct ImageViewUpdate

// Resources /////////////////////////////////////////////////////////////

static const u32                    k_max_swapchain_images = 3;
static const u32                    k_max_frames           = 2;

//
//
struct BufferSyncState {

    VkPipelineStageFlags2           stage   = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    VkAccessFlags2                  access  = 0;
    u32                             owner_queue_family = VK_QUEUE_FAMILY_IGNORED;
}; // struct BufferSyncState

//
//
struct Buffer {

    VkBuffer                        vk_buffer;
    VmaAllocation                   vma_allocation;
    VkDeviceMemory                  vk_device_memory;
    VkDeviceSize                    vk_device_size;

    VkBufferUsageFlags              type_flags      = 0;
    u32                             size            = 0;
    u32                             global_offset   = 0;    // Offset into global constant, if dynamic

    VmaMemoryUsage                  memory_usage    = VMA_MEMORY_USAGE_AUTO;
    VmaAllocationCreateFlags        allocation_flags = 0;

    BufferHandle                    handle;

    BufferSyncState                 sync_state {};

    u8*                             mapped_data     = nullptr;
    cstring                         name            = nullptr;

    bool                            persistent      = false;
    bool                            device_only     = false;

    bool                            ready = true;

}; // struct Buffer


//
//
struct Sampler {

    VkSampler                       vk_sampler;

    VkFilter                        min_filter = VK_FILTER_NEAREST;
    VkFilter                        mag_filter = VK_FILTER_NEAREST;
    VkSamplerMipmapMode             mip_filter = VK_SAMPLER_MIPMAP_MODE_NEAREST;

    VkSamplerAddressMode            address_mode_u = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSamplerAddressMode            address_mode_v = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSamplerAddressMode            address_mode_w = VK_SAMPLER_ADDRESS_MODE_REPEAT;

    VkSamplerReductionMode          reduction_mode = VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE;

    cstring                         name    = nullptr;

}; // struct Sampler

//
//
struct ImageSyncState {

    VkPipelineStageFlags2           stage   = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    VkAccessFlags2                  access  = 0;
    VkImageLayout                   layout  = VK_IMAGE_LAYOUT_UNDEFINED;
    u32                             owner_queue_family = VK_QUEUE_FAMILY_IGNORED;
}; // struct ImageSyncState

//
//
struct Image {

    VkImage                         vk_image;
    VkFormat                        vk_format;
    VkImageUsageFlags               vk_usage;
    VmaAllocation                   vma_allocation;

    u16                             width           = 1;
    u16                             height          = 1;
    u16                             depth           = 1;
    u16                             array_layer_count = 1;
    u8                              mip_level_count = 1;
    u8                              flags           = 0;
    u16                             mip_base_level  = 0;    // Not 0 when texture is a view.
    u16                             array_base_layer = 0;   // Not 0 when texture is a view.
    bool                            sparse = false;

    ImageHandle                     handle;
    ImageHandle                     alias_image;
    TextureType::Enum               type    = TextureType::Texture2D;

    ImageSyncState                  sync_state{};

    Sampler*                        sampler = nullptr;

    cstring                         name    = nullptr;
}; // struct Image

struct ImageView {

    VkImageView                     vk_image_view;

    ImageHandle                     parent_image;
    ImageViewHandle                 handle;

    VkImageViewType                 view_type;
    VkImageSubresourceRange         subresource_range;

    cstring                         name = nullptr;
    bool                            compute_access = false;

}; // struct ImageView

//
//
struct ShaderState {

    VkShaderModule                  vk_shader_modules[ k_max_shader_stages ];
    VkShaderStageFlagBits           vk_shader_stages[ k_max_shader_stages ];

    cstring                         name            = nullptr;

    u32                             active_shaders  = 0;
    VkPipelineBindPoint             type;

}; // struct ShaderState

//
//
struct DescriptorSetLayout {

    VkDescriptorSetLayout           vk_descriptor_set_layout;

    u16                             set_index       = 0;
    u8                              bindless        = 0;
    u8                              dynamic         = 0;

    u64                             binding_bitset   = 0;

    DescriptorSetLayoutHandle       handle;

}; // struct DesciptorSetLayout

//
//
struct DescriptorSet {

    VkDescriptorSet                 vk_descriptor_set;

    ResourceHandle*                 resources       = nullptr;
    SamplerHandle*                  samplers        = nullptr;
    u16*                            bindings        = nullptr;
    VkAccelerationStructureKHR      as              = VK_NULL_HANDLE;

    const DescriptorSetLayout*      layout          = nullptr;
    u32                             num_resources   = 0;
}; // struct DesciptorSet

//
//
struct PipelineLayout {
    VkPipelineLayout                vk_pipeline_layout;

    DescriptorSetLayoutHandle       descriptor_set_layout_handles[ k_max_descriptor_set_layouts ];
    u32                             num_active_layouts = 0;
    VkPushConstantRange             push_constant;
    PipelineLayoutHandle            handle;
}; // struct PipelineLayout;

//
//
struct Pipeline {

    VkPipeline                      vk_pipeline;
    VkPipelineLayout                cached_vk_layout;  // Non-owned layout to speed up access.
    VkPipelineBindPoint             vk_bind_point;

    ShaderStateHandle               shader_state;
    PipelineLayoutHandle            layout;

    DepthStencilCreation            depth_stencil;
    BlendStateCreation              blend_state;
    RasterizationCreation           rasterization;

    BufferHandle                    shader_binding_table;

    VkStridedDeviceAddressRegionKHR sbt_raygen_region;
    VkStridedDeviceAddressRegionKHR sbt_miss_region;
    VkStridedDeviceAddressRegionKHR sbt_hit_region;
    VkStridedDeviceAddressRegionKHR sbt_callable_region;
}; // struct Pipeline

//
//
struct PagePoolAllocation {
    VmaAllocation*                  allocation;
    PagePoolAllocation*             next;
}; // struct PagePoolAllocation


//
//
struct SparseMemoryBindInfo {
    VkImage                         image;
    u32                             count;
    u32                             binding_array_offset;
}; // struct SparseMemoryBindInfo


//
//
struct PagePool {
    Array<PagePoolAllocation>       allocations;
    Array<VmaAllocation>            vma_allocations;

    u32                             block_width;
    u32                             block_height;
    u32                             block_size;

    u32                             size;
    u32                             used_pages;

    PagePoolAllocation*             free_list;
}; // struct PagePool

//
//
struct BLASBuildInfo {
    Array<VkAccelerationStructureGeometryKHR>   geometries;
    Array<VkAccelerationStructureBuildRangeInfoKHR> ranges;

    VkAccelerationStructureBuildGeometryInfoKHR build_info;
    VkAccelerationStructureBuildSizesInfoKHR    build_sizes;
}; // struct BLASBuildInfo

//
//
struct TLASBuildInfo {

    VkAccelerationStructureGeometryKHR          geometry;
    VkAccelerationStructureBuildRangeInfoKHR    range;

    VkAccelerationStructureBuildGeometryInfoKHR build_info;
    VkAccelerationStructureBuildSizesInfoKHR    build_sizes;
}; // struct TLASBuildInfo

//
//
struct BLAS {
    BufferHandle                    blas_buffer;
    BufferHandle                    scratch_buffer;

    BLASHandle                      handle;
    StringView                      name;

    VkAccelerationStructureKHR      as;
}; // struct BLAS

//
//
struct TLAS {
    BufferHandle                    tlas_buffer;
    BufferHandle                    scratch_buffer;
    BufferHandle                    instance_buffer;

    TLASHandle                      handle;
    StringView                      name;

    VkAccelerationStructureKHR      as;
}; // struct TLAS

//
//
struct ComputeLocalSize {

    u32                             x : 10;
    u32                             y : 10;
    u32                             z : 10;
    u32                             pad : 2;
}; // struct ComputeLocalSize

//
//
struct ResolutionInfo {
    // Actual render resolution, can be different from swapchain size due to dynamic resolution or render scaling.
    u16                             render_width;
    u16                             render_height;

    u16                             swapchain_width;
    u16                             swapchain_height;
}; // struct ResolutionInfo

// Enum translations. Use tables or switches depending on the case. ///////
cstring                     to_compiler_extension( VkShaderStageFlagBits value );
cstring                     to_stage_defines( VkShaderStageFlagBits value );

VkImageType                 to_vk_image_type( TextureType::Enum type );
VkImageViewType             to_vk_image_view_type( TextureType::Enum type );

VkFormat                    to_vk_vertex_format( VertexComponentFormat::Enum value );

VkPipelineStageFlags        to_vk_pipeline_stage( PipelineStage::Enum value );

VkFormat                    util_string_to_vk_format( cstring format );

void                        add_binding_if_unique( DescriptorSetLayoutInfo& creation, DescriptorSetLayoutInfo::Binding& binding );

bool                        has_binding( const DescriptorSetLayout* set_layout, u32 index );

// Range helpers /////////////////////////////////////////////////////////
// Full color image (all mips, all layers)
VkImageSubresourceRange     range_color_full();

// Color image custom range
VkImageSubresourceRange     range_color( u32 base_mip, u32 mip_count,
                                         u32 base_layer = 0, u32 layer_count = 1 );

// Full depth image (all mips, all layers). Optionally include stencil.
VkImageSubresourceRange     range_depth_full( bool include_stencil = false );

// Depth image custom range. Optionally include stencil.
VkImageSubresourceRange     range_depth( u32 base_mip, u32 mip_count,
                                         u32 base_layer = 0, u32 layer_count = 1,
                                         bool include_stencil = false );

// If you already know the aspect mask, generic helper.
VkImageSubresourceRange     range_aspect( VkImageAspectFlags aspect,
                                          u32 base_mip, u32 mip_count,
                                          u32 base_layer = 0, u32 layer_count = 1 );


} // namespace raptor
