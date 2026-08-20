#include "gpu_resources.hpp"

#include "gpu_device.hpp"

#include "foundation/assert.hpp"
#include "foundation/numerics.hpp"

#include <string.h>

namespace raptor {

// BlendStateCreation /////////////////////////////////////////////////////
BlendStateCreation& BlendStateCreation::reset() {
    blend_states.clear();

    return *this;
}

// TextureCreation ////////////////////////////////////////////////////////
ImageCreation& ImageCreation::reset() {
    mip_level_count = 1;
    array_layer_count = 1;
    initial_data = nullptr;
    alias = ImageHandle();

    width = height = depth = 1;
    format = VK_FORMAT_UNDEFINED;
    flags = 0;

    return *this;
}

ImageCreation& ImageCreation::set_size( u16 width_, u16 height_, u16 depth_ ) {
    width = width_;
    height = height_;
    depth = depth_;

    return *this;
}

ImageCreation& ImageCreation::set_flags( u8 flags_ ) {
    flags = flags_;

    return *this;
}

ImageCreation& ImageCreation::set_mips( u32 mip_level_count_ ) {
    mip_level_count = mip_level_count_;

    return *this;
}

ImageCreation& ImageCreation::set_layers( u32 layer_count_ ) {
    array_layer_count = layer_count_;

    return *this;
}

ImageCreation& ImageCreation::set_format_type( VkFormat format_, TextureType::Enum type_ ) {
    format = format_;
    type = type_;

    return *this;
}

ImageCreation& ImageCreation::set_name( cstring name_ ) {
    name = name_;

    return *this;
}

ImageCreation& ImageCreation::set_data( void* data_ ) {
    initial_data = data_;

    return *this;
}

ImageCreation& ImageCreation::set_alias( ImageHandle alias_ ) {
    alias = alias_;

    return *this;
}

ImageCreation& ImageCreation::set_owner( u32 value ) {
    owner_queue_family = (u16)value;
    return *this;
}

// SamplerCreation ////////////////////////////////////////////////////////
SamplerCreation& SamplerCreation::set_min_mag_mip( VkFilter min, VkFilter mag, VkSamplerMipmapMode mip ) {
    min_filter = min;
    mag_filter = mag;
    mip_filter = mip;

    return *this;
}

SamplerCreation& SamplerCreation::set_address_mode_u( VkSamplerAddressMode u ) {
    address_mode_u = u;

    return *this;
}

SamplerCreation& SamplerCreation::set_address_mode_uv( VkSamplerAddressMode u, VkSamplerAddressMode v ) {
    address_mode_u = u;
    address_mode_v = v;

    return *this;
}

SamplerCreation& SamplerCreation::set_address_mode_uvw( VkSamplerAddressMode u, VkSamplerAddressMode v, VkSamplerAddressMode w ) {
    address_mode_u = u;
    address_mode_v = v;
    address_mode_w = w;

    return *this;
}

SamplerCreation& SamplerCreation::set_reduction_mode( VkSamplerReductionMode mode ) {
    reduction_mode = mode;

    return *this;
}

SamplerCreation& SamplerCreation::set_name( const char* name_ ) {
    name = name_;

    return *this;
}


// ShaderStateCreation ////////////////////////////////////////////////////
ShaderStateCreation& ShaderStateCreation::reset() {
    stages.clear();

    return *this;
}

ShaderStateCreation& ShaderStateCreation::set_name( const char* name_ ) {
    name = name_;

    return *this;
}

ShaderStateCreation& ShaderStateCreation::add_stage( VkShaderModuleCreateInfo& stage, VkShaderStageFlagBits flag ) {
    stages.push( stage );
    stage_types.push( flag );

    return *this;
}

// DescriptorSetLayoutCreation ////////////////////////////////////////////
DescriptorSetLayoutInfo& DescriptorSetLayoutInfo::reset() {
    num_bindings = 0;
    set_index = 0;
    return *this;
}

DescriptorSetLayoutInfo& DescriptorSetLayoutInfo::add_binding( const Binding& binding ) {
    RASSERT( num_bindings < k_max_descriptors_per_set );
    bindings[num_bindings++] = binding;
    return *this;
}

DescriptorSetLayoutInfo& DescriptorSetLayoutInfo::add_binding( VkDescriptorType type, u32 index, u32 count, cstring name ) {
    RASSERT( num_bindings < k_max_descriptors_per_set );
    bindings[ num_bindings++ ] = { type, (u16)index, (u16)count, name };
    return *this;
}

DescriptorSetLayoutInfo& DescriptorSetLayoutInfo::add_binding_at_index( const Binding& binding, int index ) {
    RASSERT( index < k_max_descriptors_per_set );
    bindings[index] = binding;
    num_bindings = raptor::max( num_bindings, (u32)( index + 1 ) );
    return *this;
}

DescriptorSetLayoutInfo& DescriptorSetLayoutInfo::set_name( cstring name_ ) {
    name = name_;
    return *this;
}


DescriptorSetLayoutInfo& DescriptorSetLayoutInfo::set_set_index( u32 index ) {
    set_index = index;
    return *this;
}

// RenderPassOutput ///////////////////////////////////////////////////////
RenderPassOutput& RenderPassOutput::reset() {
    num_color_formats = 0;
    multiview_mask = 0;

    for ( u32 i = 0; i < k_max_image_outputs; ++i) {
        color_formats[ i ] = VK_FORMAT_UNDEFINED;
        color_final_layouts[ i ] = VK_IMAGE_LAYOUT_UNDEFINED;
        color_operations[ i ] = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    depth_stencil_format = VK_FORMAT_UNDEFINED;
    depth_operation = stencil_operation = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    return *this;
}

RenderPassOutput& RenderPassOutput::color( VkFormat format, VkImageLayout layout, VkAttachmentLoadOp load_op ) {
    color_formats[ num_color_formats ] = format;
    color_operations[ num_color_formats ] = load_op;
    color_final_layouts[ num_color_formats++ ] = layout;
    return *this;
}

RenderPassOutput& RenderPassOutput::depth( VkFormat format, VkImageLayout layout ) {
    depth_stencil_format = format;
    depth_stencil_final_layout = layout;
    return *this;
}

RenderPassOutput& RenderPassOutput::set_depth_stencil_operations( VkAttachmentLoadOp depth_, VkAttachmentLoadOp stencil_ ) {
    depth_operation = depth_;
    stencil_operation = stencil_;

    return *this;
}

RenderPassOutput& RenderPassOutput::set_multiview_mask( u32 mask ) {
    multiview_mask = mask;

    return *this;
}

RenderPassOutput& RenderPassOutput::add_shading_rate_image() {
    shading_rate_image_index = num_color_formats++;
    return *this;
}

// Methods ////////////////////////////////////////////////////////////////

cstring to_compiler_extension( VkShaderStageFlagBits value ) {
    switch ( value ) {
        case VK_SHADER_STAGE_VERTEX_BIT:
            return "vert";
        case VK_SHADER_STAGE_FRAGMENT_BIT:
            return "frag";
        case VK_SHADER_STAGE_COMPUTE_BIT:
            return "comp";
        case VK_SHADER_STAGE_MESH_BIT_NV:
            return "mesh";
        case VK_SHADER_STAGE_TASK_BIT_NV:
            return "task";
        case VK_SHADER_STAGE_RAYGEN_BIT_KHR:
            return "rgen";
        case VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR:
            return "rchit";
        case VK_SHADER_STAGE_ANY_HIT_BIT_KHR:
            return "rahit";
        case VK_SHADER_STAGE_MISS_BIT_KHR:
            return "rmiss";
        default:
            return "";
    }
}

//
cstring to_stage_defines( VkShaderStageFlagBits value ) {
    switch ( value ) {
        case VK_SHADER_STAGE_VERTEX_BIT:
            return "VERTEX";
        case VK_SHADER_STAGE_FRAGMENT_BIT:
            return "FRAGMENT";
        case VK_SHADER_STAGE_COMPUTE_BIT:
            return "COMPUTE";
        case VK_SHADER_STAGE_MESH_BIT_NV:
            return "MESH";
        case VK_SHADER_STAGE_TASK_BIT_NV:
            return "TASK";
        case VK_SHADER_STAGE_RAYGEN_BIT_KHR:
            return "RAYGEN";
        case VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR:
            return "CLOSEST_HIT";
        case VK_SHADER_STAGE_ANY_HIT_BIT_KHR:
            return "ANY_HIT";
        case VK_SHADER_STAGE_MISS_BIT_KHR:
            return "MISS";
        default:
            return "";
    }
}

//
// Texture1D, Texture2D, Texture3D, TextureCube, Texture_1D_Array, Texture_2D_Array, Texture_Cube_Array, Count
VkImageType to_vk_image_type( TextureType::Enum type ) {
    static VkImageType s_vk_target[ TextureType::Count ] = { VK_IMAGE_TYPE_1D, VK_IMAGE_TYPE_2D, VK_IMAGE_TYPE_3D, VK_IMAGE_TYPE_2D, VK_IMAGE_TYPE_1D, VK_IMAGE_TYPE_2D, VK_IMAGE_TYPE_2D };
    return s_vk_target[ type ];
}

//
//
VkImageViewType to_vk_image_view_type( TextureType::Enum type ) {
    static VkImageViewType s_vk_data[] = { VK_IMAGE_VIEW_TYPE_1D, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_VIEW_TYPE_1D_ARRAY, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY };
    return s_vk_data[ type ];
}

//
//
VkFormat to_vk_vertex_format( VertexComponentFormat::Enum value ) {
    // Float, Float2, Float3, Float4, Mat4,
    // Byte, Byte4N, UByte, UByte4, UByte4N, Short2, Short2N,
    // Short4, Short4N, Uint, Uint2, Uint4, Count
    static VkFormat s_vk_vertex_formats[ VertexComponentFormat::Count ] =
    { VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, /*MAT4 TODO*/VK_FORMAT_R32G32B32A32_SFLOAT,
      VK_FORMAT_R8_SINT, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8_UINT, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16_SNORM,
      VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32A32_UINT };

    return s_vk_vertex_formats[ value ];
}

//
//
VkPipelineStageFlags to_vk_pipeline_stage( PipelineStage::Enum value ) {
    static VkPipelineStageFlags s_vk_values[] = { VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT };
    return s_vk_values[ value ];
}

VkFormat util_string_to_vk_format( cstring format ) {
    if ( strcmp( format, "VK_FORMAT_R4G4_UNORM_PACK8" ) == 0 ) {
        return VK_FORMAT_R4G4_UNORM_PACK8;
    }
    if ( strcmp( format, "VK_FORMAT_R4G4B4A4_UNORM_PACK16" ) == 0 ) {
        return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
    }
    if ( strcmp( format, "VK_FORMAT_B4G4R4A4_UNORM_PACK16" ) == 0 ) {
        return VK_FORMAT_B4G4R4A4_UNORM_PACK16;
    }
    if ( strcmp( format, "VK_FORMAT_R5G6B5_UNORM_PACK16" ) == 0 ) {
        return VK_FORMAT_R5G6B5_UNORM_PACK16;
    }
    if ( strcmp( format, "VK_FORMAT_B5G6R5_UNORM_PACK16" ) == 0 ) {
        return VK_FORMAT_B5G6R5_UNORM_PACK16;
    }
    if ( strcmp( format, "VK_FORMAT_R5G5B5A1_UNORM_PACK16" ) == 0 ) {
        return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
    }
    if ( strcmp( format, "VK_FORMAT_B5G5R5A1_UNORM_PACK16" ) == 0 ) {
        return VK_FORMAT_B5G5R5A1_UNORM_PACK16;
    }
    if ( strcmp( format, "VK_FORMAT_A1R5G5B5_UNORM_PACK16" ) == 0 ) {
        return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
    }
    if ( strcmp( format, "VK_FORMAT_R8_UNORM" ) == 0 ) {
        return VK_FORMAT_R8_UNORM;
    }
    if ( strcmp( format, "VK_FORMAT_R8_SNORM" ) == 0 ) {
        return VK_FORMAT_R8_SNORM;
    }
    if ( strcmp( format, "VK_FORMAT_R8_USCALED" ) == 0 ) {
        return VK_FORMAT_R8_USCALED;
    }
    if ( strcmp( format, "VK_FORMAT_R8_SSCALED" ) == 0 ) {
        return VK_FORMAT_R8_SSCALED;
    }
    if ( strcmp( format, "VK_FORMAT_R8_UINT" ) == 0 ) {
        return VK_FORMAT_R8_UINT;
    }
    if ( strcmp( format, "VK_FORMAT_R8_SINT" ) == 0 ) {
        return VK_FORMAT_R8_SINT;
    }
    if ( strcmp( format, "VK_FORMAT_R8_SRGB" ) == 0 ) {
        return VK_FORMAT_R8_SRGB;
    }
    if ( strcmp( format, "VK_FORMAT_R8G8_UNORM" ) == 0 ) {
        return VK_FORMAT_R8G8_UNORM;
    }
    if ( strcmp( format, "VK_FORMAT_R8G8_SNORM" ) == 0 ) {
        return VK_FORMAT_R8G8_SNORM;
    }
    if ( strcmp( format, "VK_FORMAT_R8G8_USCALED" ) == 0 ) {
        return VK_FORMAT_R8G8_USCALED;
    }
    if ( strcmp( format, "VK_FORMAT_R8G8_SSCALED" ) == 0 ) {
        return VK_FORMAT_R8G8_SSCALED;
    }
    if ( strcmp( format, "VK_FORMAT_R8G8_UINT" ) == 0 ) {
        return VK_FORMAT_R8G8_UINT;
    }
    if ( strcmp( format, "VK_FORMAT_R8G8_SINT" ) == 0 ) {
        return VK_FORMAT_R8G8_SINT;
    }
    if ( strcmp( format, "VK_FORMAT_R8G8_SRGB" ) == 0 ) {
        return VK_FORMAT_R8G8_SRGB;
    }
    if ( strcmp( format, "VK_FORMAT_R8G8B8_UNORM" ) == 0 ) {
        return VK_FORMAT_R8G8B8_UNORM;
    }
    if ( strcmp( format, "VK_FORMAT_R8G8B8_SNORM" ) == 0 ) {
        return VK_FORMAT_R8G8B8_SNORM;
    }
    if ( strcmp( format, "VK_FORMAT_R8G8B8_USCALED" ) == 0 ) {
        return VK_FORMAT_R8G8B8_USCALED;
    }
    if ( strcmp( format, "VK_FORMAT_R8G8B8_SSCALED" ) == 0 ) {
        return VK_FORMAT_R8G8B8_SSCALED;
    }
    if ( strcmp( format, "VK_FORMAT_R8G8B8_UINT" ) == 0 ) {
        return VK_FORMAT_R8G8B8_UINT;
    }
    if ( strcmp( format, "VK_FORMAT_R8G8B8_SINT" ) == 0 ) {
        return VK_FORMAT_R8G8B8_SINT;
    }
    if ( strcmp( format, "VK_FORMAT_R8G8B8_SRGB" ) == 0 ) {
        return VK_FORMAT_R8G8B8_SRGB;
    }
    if ( strcmp( format, "VK_FORMAT_B8G8R8_UNORM" ) == 0 ) {
        return VK_FORMAT_B8G8R8_UNORM;
    }
    if ( strcmp( format, "VK_FORMAT_B8G8R8_SNORM" ) == 0 ) {
        return VK_FORMAT_B8G8R8_SNORM;
    }
    if ( strcmp( format, "VK_FORMAT_B8G8R8_USCALED" ) == 0 ) {
        return VK_FORMAT_B8G8R8_USCALED;
    }
    if ( strcmp( format, "VK_FORMAT_B8G8R8_SSCALED" ) == 0 ) {
        return VK_FORMAT_B8G8R8_SSCALED;
    }
    if ( strcmp( format, "VK_FORMAT_B8G8R8_UINT" ) == 0 ) {
        return VK_FORMAT_B8G8R8_UINT;
    }
    if ( strcmp( format, "VK_FORMAT_B8G8R8_SINT" ) == 0 ) {
        return VK_FORMAT_B8G8R8_SINT;
    }
    if ( strcmp( format, "VK_FORMAT_B8G8R8_SRGB" ) == 0 ) {
        return VK_FORMAT_B8G8R8_SRGB;
    }
    if ( strcmp( format, "VK_FORMAT_R8G8B8A8_UNORM" ) == 0 ) {
        return VK_FORMAT_R8G8B8A8_UNORM;
    }
    if ( strcmp( format, "VK_FORMAT_R8G8B8A8_SNORM" ) == 0 ) {
        return VK_FORMAT_R8G8B8A8_SNORM;
    }
    if ( strcmp( format, "VK_FORMAT_R8G8B8A8_USCALED" ) == 0 ) {
        return VK_FORMAT_R8G8B8A8_USCALED;
    }
    if ( strcmp( format, "VK_FORMAT_R8G8B8A8_SSCALED" ) == 0 ) {
        return VK_FORMAT_R8G8B8A8_SSCALED;
    }
    if ( strcmp( format, "VK_FORMAT_R8G8B8A8_UINT" ) == 0 ) {
        return VK_FORMAT_R8G8B8A8_UINT;
    }
    if ( strcmp( format, "VK_FORMAT_R8G8B8A8_SINT" ) == 0 ) {
        return VK_FORMAT_R8G8B8A8_SINT;
    }
    if ( strcmp( format, "VK_FORMAT_R8G8B8A8_SRGB" ) == 0 ) {
        return VK_FORMAT_R8G8B8A8_SRGB;
    }
    if ( strcmp( format, "VK_FORMAT_B8G8R8A8_UNORM" ) == 0 ) {
        return VK_FORMAT_B8G8R8A8_UNORM;
    }
    if ( strcmp( format, "VK_FORMAT_B8G8R8A8_SNORM" ) == 0 ) {
        return VK_FORMAT_B8G8R8A8_SNORM;
    }
    if ( strcmp( format, "VK_FORMAT_B8G8R8A8_USCALED" ) == 0 ) {
        return VK_FORMAT_B8G8R8A8_USCALED;
    }
    if ( strcmp( format, "VK_FORMAT_B8G8R8A8_SSCALED" ) == 0 ) {
        return VK_FORMAT_B8G8R8A8_SSCALED;
    }
    if ( strcmp( format, "VK_FORMAT_B8G8R8A8_UINT" ) == 0 ) {
        return VK_FORMAT_B8G8R8A8_UINT;
    }
    if ( strcmp( format, "VK_FORMAT_B8G8R8A8_SINT" ) == 0 ) {
        return VK_FORMAT_B8G8R8A8_SINT;
    }
    if ( strcmp( format, "VK_FORMAT_B8G8R8A8_SRGB" ) == 0 ) {
        return VK_FORMAT_B8G8R8A8_SRGB;
    }
    if ( strcmp( format, "VK_FORMAT_A8B8G8R8_UNORM_PACK32" ) == 0 ) {
        return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    }
    if ( strcmp( format, "VK_FORMAT_A8B8G8R8_SNORM_PACK32" ) == 0 ) {
        return VK_FORMAT_A8B8G8R8_SNORM_PACK32;
    }
    if ( strcmp( format, "VK_FORMAT_A8B8G8R8_USCALED_PACK32" ) == 0 ) {
        return VK_FORMAT_A8B8G8R8_USCALED_PACK32;
    }
    if ( strcmp( format, "VK_FORMAT_A8B8G8R8_SSCALED_PACK32" ) == 0 ) {
        return VK_FORMAT_A8B8G8R8_SSCALED_PACK32;
    }
    if ( strcmp( format, "VK_FORMAT_A8B8G8R8_UINT_PACK32" ) == 0 ) {
        return VK_FORMAT_A8B8G8R8_UINT_PACK32;
    }
    if ( strcmp( format, "VK_FORMAT_A8B8G8R8_SINT_PACK32" ) == 0 ) {
        return VK_FORMAT_A8B8G8R8_SINT_PACK32;
    }
    if ( strcmp( format, "VK_FORMAT_A8B8G8R8_SRGB_PACK32" ) == 0 ) {
        return VK_FORMAT_A8B8G8R8_SRGB_PACK32;
    }
    if ( strcmp( format, "VK_FORMAT_A2R10G10B10_UNORM_PACK32" ) == 0 ) {
        return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    }
    if ( strcmp( format, "VK_FORMAT_A2R10G10B10_SNORM_PACK32" ) == 0 ) {
        return VK_FORMAT_A2R10G10B10_SNORM_PACK32;
    }
    if ( strcmp( format, "VK_FORMAT_A2R10G10B10_USCALED_PACK32" ) == 0 ) {
        return VK_FORMAT_A2R10G10B10_USCALED_PACK32;
    }
    if ( strcmp( format, "VK_FORMAT_A2R10G10B10_SSCALED_PACK32" ) == 0 ) {
        return VK_FORMAT_A2R10G10B10_SSCALED_PACK32;
    }
    if ( strcmp( format, "VK_FORMAT_A2R10G10B10_UINT_PACK32" ) == 0 ) {
        return VK_FORMAT_A2R10G10B10_UINT_PACK32;
    }
    if ( strcmp( format, "VK_FORMAT_A2R10G10B10_SINT_PACK32" ) == 0 ) {
        return VK_FORMAT_A2R10G10B10_SINT_PACK32;
    }
    if ( strcmp( format, "VK_FORMAT_A2B10G10R10_UNORM_PACK32" ) == 0 ) {
        return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    }
    if ( strcmp( format, "VK_FORMAT_A2B10G10R10_SNORM_PACK32" ) == 0 ) {
        return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    }
    if ( strcmp( format, "VK_FORMAT_A2B10G10R10_USCALED_PACK32" ) == 0 ) {
        return VK_FORMAT_A2B10G10R10_USCALED_PACK32;
    }
    if ( strcmp( format, "VK_FORMAT_A2B10G10R10_SSCALED_PACK32" ) == 0 ) {
        return VK_FORMAT_A2B10G10R10_SSCALED_PACK32;
    }
    if ( strcmp( format, "VK_FORMAT_A2B10G10R10_UINT_PACK32" ) == 0 ) {
        return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    }
    if ( strcmp( format, "VK_FORMAT_A2B10G10R10_SINT_PACK32" ) == 0 ) {
        return VK_FORMAT_A2B10G10R10_SINT_PACK32;
    }
    if ( strcmp( format, "VK_FORMAT_R16_UNORM" ) == 0 ) {
        return VK_FORMAT_R16_UNORM;
    }
    if ( strcmp( format, "VK_FORMAT_R16_SNORM" ) == 0 ) {
        return VK_FORMAT_R16_SNORM;
    }
    if ( strcmp( format, "VK_FORMAT_R16_USCALED" ) == 0 ) {
        return VK_FORMAT_R16_USCALED;
    }
    if ( strcmp( format, "VK_FORMAT_R16_SSCALED" ) == 0 ) {
        return VK_FORMAT_R16_SSCALED;
    }
    if ( strcmp( format, "VK_FORMAT_R16_UINT" ) == 0 ) {
        return VK_FORMAT_R16_UINT;
    }
    if ( strcmp( format, "VK_FORMAT_R16_SINT" ) == 0 ) {
        return VK_FORMAT_R16_SINT;
    }
    if ( strcmp( format, "VK_FORMAT_R16_SFLOAT" ) == 0 ) {
        return VK_FORMAT_R16_SFLOAT;
    }
    if ( strcmp( format, "VK_FORMAT_R16G16_UNORM" ) == 0 ) {
        return VK_FORMAT_R16G16_UNORM;
    }
    if ( strcmp( format, "VK_FORMAT_R16G16_SNORM" ) == 0 ) {
        return VK_FORMAT_R16G16_SNORM;
    }
    if ( strcmp( format, "VK_FORMAT_R16G16_USCALED" ) == 0 ) {
        return VK_FORMAT_R16G16_USCALED;
    }
    if ( strcmp( format, "VK_FORMAT_R16G16_SSCALED" ) == 0 ) {
        return VK_FORMAT_R16G16_SSCALED;
    }
    if ( strcmp( format, "VK_FORMAT_R16G16_UINT" ) == 0 ) {
        return VK_FORMAT_R16G16_UINT;
    }
    if ( strcmp( format, "VK_FORMAT_R16G16_SINT" ) == 0 ) {
        return VK_FORMAT_R16G16_SINT;
    }
    if ( strcmp( format, "VK_FORMAT_R16G16_SFLOAT" ) == 0 ) {
        return VK_FORMAT_R16G16_SFLOAT;
    }
    if ( strcmp( format, "VK_FORMAT_R16G16B16_UNORM" ) == 0 ) {
        return VK_FORMAT_R16G16B16_UNORM;
    }
    if ( strcmp( format, "VK_FORMAT_R16G16B16_SNORM" ) == 0 ) {
        return VK_FORMAT_R16G16B16_SNORM;
    }
    if ( strcmp( format, "VK_FORMAT_R16G16B16_USCALED" ) == 0 ) {
        return VK_FORMAT_R16G16B16_USCALED;
    }
    if ( strcmp( format, "VK_FORMAT_R16G16B16_SSCALED" ) == 0 ) {
        return VK_FORMAT_R16G16B16_SSCALED;
    }
    if ( strcmp( format, "VK_FORMAT_R16G16B16_UINT" ) == 0 ) {
        return VK_FORMAT_R16G16B16_UINT;
    }
    if ( strcmp( format, "VK_FORMAT_R16G16B16_SINT" ) == 0 ) {
        return VK_FORMAT_R16G16B16_SINT;
    }
    if ( strcmp( format, "VK_FORMAT_R16G16B16_SFLOAT" ) == 0 ) {
        return VK_FORMAT_R16G16B16_SFLOAT;
    }
    if ( strcmp( format, "VK_FORMAT_R16G16B16A16_UNORM" ) == 0 ) {
        return VK_FORMAT_R16G16B16A16_UNORM;
    }
    if ( strcmp( format, "VK_FORMAT_R16G16B16A16_SNORM" ) == 0 ) {
        return VK_FORMAT_R16G16B16A16_SNORM;
    }
    if ( strcmp( format, "VK_FORMAT_R16G16B16A16_USCALED" ) == 0 ) {
        return VK_FORMAT_R16G16B16A16_USCALED;
    }
    if ( strcmp( format, "VK_FORMAT_R16G16B16A16_SSCALED" ) == 0 ) {
        return VK_FORMAT_R16G16B16A16_SSCALED;
    }
    if ( strcmp( format, "VK_FORMAT_R16G16B16A16_UINT" ) == 0 ) {
        return VK_FORMAT_R16G16B16A16_UINT;
    }
    if ( strcmp( format, "VK_FORMAT_R16G16B16A16_SINT" ) == 0 ) {
        return VK_FORMAT_R16G16B16A16_SINT;
    }
    if ( strcmp( format, "VK_FORMAT_R16G16B16A16_SFLOAT" ) == 0 ) {
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    }
    if ( strcmp( format, "VK_FORMAT_R32_UINT" ) == 0 ) {
        return VK_FORMAT_R32_UINT;
    }
    if ( strcmp( format, "VK_FORMAT_R32_SINT" ) == 0 ) {
        return VK_FORMAT_R32_SINT;
    }
    if ( strcmp( format, "VK_FORMAT_R32_SFLOAT" ) == 0 ) {
        return VK_FORMAT_R32_SFLOAT;
    }
    if ( strcmp( format, "VK_FORMAT_R32G32_UINT" ) == 0 ) {
        return VK_FORMAT_R32G32_UINT;
    }
    if ( strcmp( format, "VK_FORMAT_R32G32_SINT" ) == 0 ) {
        return VK_FORMAT_R32G32_SINT;
    }
    if ( strcmp( format, "VK_FORMAT_R32G32_SFLOAT" ) == 0 ) {
        return VK_FORMAT_R32G32_SFLOAT;
    }
    if ( strcmp( format, "VK_FORMAT_R32G32B32_UINT" ) == 0 ) {
        return VK_FORMAT_R32G32B32_UINT;
    }
    if ( strcmp( format, "VK_FORMAT_R32G32B32_SINT" ) == 0 ) {
        return VK_FORMAT_R32G32B32_SINT;
    }
    if ( strcmp( format, "VK_FORMAT_R32G32B32_SFLOAT" ) == 0 ) {
        return VK_FORMAT_R32G32B32_SFLOAT;
    }
    if ( strcmp( format, "VK_FORMAT_R32G32B32A32_UINT" ) == 0 ) {
        return VK_FORMAT_R32G32B32A32_UINT;
    }
    if ( strcmp( format, "VK_FORMAT_R32G32B32A32_SINT" ) == 0 ) {
        return VK_FORMAT_R32G32B32A32_SINT;
    }
    if ( strcmp( format, "VK_FORMAT_R32G32B32A32_SFLOAT" ) == 0 ) {
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
    if ( strcmp( format, "VK_FORMAT_R64_UINT" ) == 0 ) {
        return VK_FORMAT_R64_UINT;
    }
    if ( strcmp( format, "VK_FORMAT_R64_SINT" ) == 0 ) {
        return VK_FORMAT_R64_SINT;
    }
    if ( strcmp( format, "VK_FORMAT_R64_SFLOAT" ) == 0 ) {
        return VK_FORMAT_R64_SFLOAT;
    }
    if ( strcmp( format, "VK_FORMAT_R64G64_UINT" ) == 0 ) {
        return VK_FORMAT_R64G64_UINT;
    }
    if ( strcmp( format, "VK_FORMAT_R64G64_SINT" ) == 0 ) {
        return VK_FORMAT_R64G64_SINT;
    }
    if ( strcmp( format, "VK_FORMAT_R64G64_SFLOAT" ) == 0 ) {
        return VK_FORMAT_R64G64_SFLOAT;
    }
    if ( strcmp( format, "VK_FORMAT_R64G64B64_UINT" ) == 0 ) {
        return VK_FORMAT_R64G64B64_UINT;
    }
    if ( strcmp( format, "VK_FORMAT_R64G64B64_SINT" ) == 0 ) {
        return VK_FORMAT_R64G64B64_SINT;
    }
    if ( strcmp( format, "VK_FORMAT_R64G64B64_SFLOAT" ) == 0 ) {
        return VK_FORMAT_R64G64B64_SFLOAT;
    }
    if ( strcmp( format, "VK_FORMAT_R64G64B64A64_UINT" ) == 0 ) {
        return VK_FORMAT_R64G64B64A64_UINT;
    }
    if ( strcmp( format, "VK_FORMAT_R64G64B64A64_SINT" ) == 0 ) {
        return VK_FORMAT_R64G64B64A64_SINT;
    }
    if ( strcmp( format, "VK_FORMAT_R64G64B64A64_SFLOAT" ) == 0 ) {
        return VK_FORMAT_R64G64B64A64_SFLOAT;
    }
    if ( strcmp( format, "VK_FORMAT_B10G11R11_UFLOAT_PACK32" ) == 0 ) {
        return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    }
    if ( strcmp( format, "VK_FORMAT_E5B9G9R9_UFLOAT_PACK32" ) == 0 ) {
        return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    }
    if ( strcmp( format, "VK_FORMAT_D16_UNORM" ) == 0 ) {
        return VK_FORMAT_D16_UNORM;
    }
    if ( strcmp( format, "VK_FORMAT_X8_D24_UNORM_PACK32" ) == 0 ) {
        return VK_FORMAT_X8_D24_UNORM_PACK32;
    }
    if ( strcmp( format, "VK_FORMAT_D32_SFLOAT" ) == 0 ) {
        return VK_FORMAT_D32_SFLOAT;
    }
    if ( strcmp( format, "VK_FORMAT_S8_UINT" ) == 0 ) {
        return VK_FORMAT_S8_UINT;
    }
    if ( strcmp( format, "VK_FORMAT_D16_UNORM_S8_UINT" ) == 0 ) {
        return VK_FORMAT_D16_UNORM_S8_UINT;
    }
    if ( strcmp( format, "VK_FORMAT_D24_UNORM_S8_UINT" ) == 0 ) {
        return VK_FORMAT_D24_UNORM_S8_UINT;
    }
    if ( strcmp( format, "VK_FORMAT_D32_SFLOAT_S8_UINT" ) == 0 ) {
        return VK_FORMAT_D32_SFLOAT_S8_UINT;
    }
    if ( strcmp( format, "VK_FORMAT_BC1_RGB_UNORM_BLOCK" ) == 0 ) {
        return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_BC1_RGB_SRGB_BLOCK" ) == 0 ) {
        return VK_FORMAT_BC1_RGB_SRGB_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_BC1_RGBA_UNORM_BLOCK" ) == 0 ) {
        return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_BC1_RGBA_SRGB_BLOCK" ) == 0 ) {
        return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_BC2_UNORM_BLOCK" ) == 0 ) {
        return VK_FORMAT_BC2_UNORM_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_BC2_SRGB_BLOCK" ) == 0 ) {
        return VK_FORMAT_BC2_SRGB_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_BC3_UNORM_BLOCK" ) == 0 ) {
        return VK_FORMAT_BC3_UNORM_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_BC3_SRGB_BLOCK" ) == 0 ) {
        return VK_FORMAT_BC3_SRGB_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_BC4_UNORM_BLOCK" ) == 0 ) {
        return VK_FORMAT_BC4_UNORM_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_BC4_SNORM_BLOCK" ) == 0 ) {
        return VK_FORMAT_BC4_SNORM_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_BC5_UNORM_BLOCK" ) == 0 ) {
        return VK_FORMAT_BC5_UNORM_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_BC5_SNORM_BLOCK" ) == 0 ) {
        return VK_FORMAT_BC5_SNORM_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_BC6H_UFLOAT_BLOCK" ) == 0 ) {
        return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_BC6H_SFLOAT_BLOCK" ) == 0 ) {
        return VK_FORMAT_BC6H_SFLOAT_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_BC7_UNORM_BLOCK" ) == 0 ) {
        return VK_FORMAT_BC7_UNORM_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_BC7_SRGB_BLOCK" ) == 0 ) {
        return VK_FORMAT_BC7_SRGB_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK" ) == 0 ) {
        return VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK" ) == 0 ) {
        return VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK" ) == 0 ) {
        return VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK" ) == 0 ) {
        return VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK" ) == 0 ) {
        return VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK" ) == 0 ) {
        return VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_EAC_R11_UNORM_BLOCK" ) == 0 ) {
        return VK_FORMAT_EAC_R11_UNORM_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_EAC_R11_SNORM_BLOCK" ) == 0 ) {
        return VK_FORMAT_EAC_R11_SNORM_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_EAC_R11G11_UNORM_BLOCK" ) == 0 ) {
        return VK_FORMAT_EAC_R11G11_UNORM_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_EAC_R11G11_SNORM_BLOCK" ) == 0 ) {
        return VK_FORMAT_EAC_R11G11_SNORM_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_4x4_UNORM_BLOCK" ) == 0 ) {
        return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_4x4_SRGB_BLOCK" ) == 0 ) {
        return VK_FORMAT_ASTC_4x4_SRGB_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_5x4_UNORM_BLOCK" ) == 0 ) {
        return VK_FORMAT_ASTC_5x4_UNORM_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_5x4_SRGB_BLOCK" ) == 0 ) {
        return VK_FORMAT_ASTC_5x4_SRGB_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_5x5_UNORM_BLOCK" ) == 0 ) {
        return VK_FORMAT_ASTC_5x5_UNORM_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_5x5_SRGB_BLOCK" ) == 0 ) {
        return VK_FORMAT_ASTC_5x5_SRGB_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_6x5_UNORM_BLOCK" ) == 0 ) {
        return VK_FORMAT_ASTC_6x5_UNORM_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_6x5_SRGB_BLOCK" ) == 0 ) {
        return VK_FORMAT_ASTC_6x5_SRGB_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_6x6_UNORM_BLOCK" ) == 0 ) {
        return VK_FORMAT_ASTC_6x6_UNORM_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_6x6_SRGB_BLOCK" ) == 0 ) {
        return VK_FORMAT_ASTC_6x6_SRGB_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_8x5_UNORM_BLOCK" ) == 0 ) {
        return VK_FORMAT_ASTC_8x5_UNORM_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_8x5_SRGB_BLOCK" ) == 0 ) {
        return VK_FORMAT_ASTC_8x5_SRGB_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_8x6_UNORM_BLOCK" ) == 0 ) {
        return VK_FORMAT_ASTC_8x6_UNORM_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_8x6_SRGB_BLOCK" ) == 0 ) {
        return VK_FORMAT_ASTC_8x6_SRGB_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_8x8_UNORM_BLOCK" ) == 0 ) {
        return VK_FORMAT_ASTC_8x8_UNORM_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_8x8_SRGB_BLOCK" ) == 0 ) {
        return VK_FORMAT_ASTC_8x8_SRGB_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_10x5_UNORM_BLOCK" ) == 0 ) {
        return VK_FORMAT_ASTC_10x5_UNORM_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_10x5_SRGB_BLOCK" ) == 0 ) {
        return VK_FORMAT_ASTC_10x5_SRGB_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_10x6_UNORM_BLOCK" ) == 0 ) {
        return VK_FORMAT_ASTC_10x6_UNORM_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_10x6_SRGB_BLOCK" ) == 0 ) {
        return VK_FORMAT_ASTC_10x6_SRGB_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_10x8_UNORM_BLOCK" ) == 0 ) {
        return VK_FORMAT_ASTC_10x8_UNORM_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_10x8_SRGB_BLOCK" ) == 0 ) {
        return VK_FORMAT_ASTC_10x8_SRGB_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_10x10_UNORM_BLOCK" ) == 0 ) {
        return VK_FORMAT_ASTC_10x10_UNORM_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_10x10_SRGB_BLOCK" ) == 0 ) {
        return VK_FORMAT_ASTC_10x10_SRGB_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_12x10_UNORM_BLOCK" ) == 0 ) {
        return VK_FORMAT_ASTC_12x10_UNORM_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_12x10_SRGB_BLOCK" ) == 0 ) {
        return VK_FORMAT_ASTC_12x10_SRGB_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_12x12_UNORM_BLOCK" ) == 0 ) {
        return VK_FORMAT_ASTC_12x12_UNORM_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_12x12_SRGB_BLOCK" ) == 0 ) {
        return VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
    }
    if ( strcmp( format, "VK_FORMAT_G8B8G8R8_422_UNORM" ) == 0 ) {
        return VK_FORMAT_G8B8G8R8_422_UNORM;
    }
    if ( strcmp( format, "VK_FORMAT_B8G8R8G8_422_UNORM" ) == 0 ) {
        return VK_FORMAT_B8G8R8G8_422_UNORM;
    }
    if ( strcmp( format, "VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM" ) == 0 ) {
        return VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM;
    }
    if ( strcmp( format, "VK_FORMAT_G8_B8R8_2PLANE_420_UNORM" ) == 0 ) {
        return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    }
    if ( strcmp( format, "VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM" ) == 0 ) {
        return VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM;
    }
    if ( strcmp( format, "VK_FORMAT_G8_B8R8_2PLANE_422_UNORM" ) == 0 ) {
        return VK_FORMAT_G8_B8R8_2PLANE_422_UNORM;
    }
    if ( strcmp( format, "VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM" ) == 0 ) {
        return VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM;
    }
    if ( strcmp( format, "VK_FORMAT_R10X6_UNORM_PACK16" ) == 0 ) {
        return VK_FORMAT_R10X6_UNORM_PACK16;
    }
    if ( strcmp( format, "VK_FORMAT_R10X6G10X6_UNORM_2PACK16" ) == 0 ) {
        return VK_FORMAT_R10X6G10X6_UNORM_2PACK16;
    }
    if ( strcmp( format, "VK_FORMAT_R10X6G10X6B10X6A10X6_UNORM_4PACK16" ) == 0 ) {
        return VK_FORMAT_R10X6G10X6B10X6A10X6_UNORM_4PACK16;
    }
    if ( strcmp( format, "VK_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16" ) == 0 ) {
        return VK_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16;
    }
    if ( strcmp( format, "VK_FORMAT_B10X6G10X6R10X6G10X6_422_UNORM_4PACK16" ) == 0 ) {
        return VK_FORMAT_B10X6G10X6R10X6G10X6_422_UNORM_4PACK16;
    }
    if ( strcmp( format, "VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16" ) == 0 ) {
        return VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16;
    }
    if ( strcmp( format, "VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16" ) == 0 ) {
        return VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
    }
    if ( strcmp( format, "VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16" ) == 0 ) {
        return VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16;
    }
    if ( strcmp( format, "VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16" ) == 0 ) {
        return VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16;
    }
    if ( strcmp( format, "VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16" ) == 0 ) {
        return VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16;
    }
    if ( strcmp( format, "VK_FORMAT_R12X4_UNORM_PACK16" ) == 0 ) {
        return VK_FORMAT_R12X4_UNORM_PACK16;
    }
    if ( strcmp( format, "VK_FORMAT_R12X4G12X4_UNORM_2PACK16" ) == 0 ) {
        return VK_FORMAT_R12X4G12X4_UNORM_2PACK16;
    }
    if ( strcmp( format, "VK_FORMAT_R12X4G12X4B12X4A12X4_UNORM_4PACK16" ) == 0 ) {
        return VK_FORMAT_R12X4G12X4B12X4A12X4_UNORM_4PACK16;
    }
    if ( strcmp( format, "VK_FORMAT_G12X4B12X4G12X4R12X4_422_UNORM_4PACK16" ) == 0 ) {
        return VK_FORMAT_G12X4B12X4G12X4R12X4_422_UNORM_4PACK16;
    }
    if ( strcmp( format, "VK_FORMAT_B12X4G12X4R12X4G12X4_422_UNORM_4PACK16" ) == 0 ) {
        return VK_FORMAT_B12X4G12X4R12X4G12X4_422_UNORM_4PACK16;
    }
    if ( strcmp( format, "VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16" ) == 0 ) {
        return VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16;
    }
    if ( strcmp( format, "VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16" ) == 0 ) {
        return VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16;
    }
    if ( strcmp( format, "VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16" ) == 0 ) {
        return VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16;
    }
    if ( strcmp( format, "VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16" ) == 0 ) {
        return VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16;
    }
    if ( strcmp( format, "VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16" ) == 0 ) {
        return VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16;
    }
    if ( strcmp( format, "VK_FORMAT_G16B16G16R16_422_UNORM" ) == 0 ) {
        return VK_FORMAT_G16B16G16R16_422_UNORM;
    }
    if ( strcmp( format, "VK_FORMAT_B16G16R16G16_422_UNORM" ) == 0 ) {
        return VK_FORMAT_B16G16R16G16_422_UNORM;
    }
    if ( strcmp( format, "VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM" ) == 0 ) {
        return VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM;
    }
    if ( strcmp( format, "VK_FORMAT_G16_B16R16_2PLANE_420_UNORM" ) == 0 ) {
        return VK_FORMAT_G16_B16R16_2PLANE_420_UNORM;
    }
    if ( strcmp( format, "VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM" ) == 0 ) {
        return VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM;
    }
    if ( strcmp( format, "VK_FORMAT_G16_B16R16_2PLANE_422_UNORM" ) == 0 ) {
        return VK_FORMAT_G16_B16R16_2PLANE_422_UNORM;
    }
    if ( strcmp( format, "VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM" ) == 0 ) {
        return VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM;
    }
    if ( strcmp( format, "VK_FORMAT_PVRTC1_2BPP_UNORM_BLOCK_IMG" ) == 0 ) {
        return VK_FORMAT_PVRTC1_2BPP_UNORM_BLOCK_IMG;
    }
    if ( strcmp( format, "VK_FORMAT_PVRTC1_4BPP_UNORM_BLOCK_IMG" ) == 0 ) {
        return VK_FORMAT_PVRTC1_4BPP_UNORM_BLOCK_IMG;
    }
    if ( strcmp( format, "VK_FORMAT_PVRTC2_2BPP_UNORM_BLOCK_IMG" ) == 0 ) {
        return VK_FORMAT_PVRTC2_2BPP_UNORM_BLOCK_IMG;
    }
    if ( strcmp( format, "VK_FORMAT_PVRTC2_4BPP_UNORM_BLOCK_IMG" ) == 0 ) {
        return VK_FORMAT_PVRTC2_4BPP_UNORM_BLOCK_IMG;
    }
    if ( strcmp( format, "VK_FORMAT_PVRTC1_2BPP_SRGB_BLOCK_IMG" ) == 0 ) {
        return VK_FORMAT_PVRTC1_2BPP_SRGB_BLOCK_IMG;
    }
    if ( strcmp( format, "VK_FORMAT_PVRTC1_4BPP_SRGB_BLOCK_IMG" ) == 0 ) {
        return VK_FORMAT_PVRTC1_4BPP_SRGB_BLOCK_IMG;
    }
    if ( strcmp( format, "VK_FORMAT_PVRTC2_2BPP_SRGB_BLOCK_IMG" ) == 0 ) {
        return VK_FORMAT_PVRTC2_2BPP_SRGB_BLOCK_IMG;
    }
    if ( strcmp( format, "VK_FORMAT_PVRTC2_4BPP_SRGB_BLOCK_IMG" ) == 0 ) {
        return VK_FORMAT_PVRTC2_4BPP_SRGB_BLOCK_IMG;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK_EXT" ) == 0 ) {
        return VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK_EXT;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_5x4_SFLOAT_BLOCK_EXT" ) == 0 ) {
        return VK_FORMAT_ASTC_5x4_SFLOAT_BLOCK_EXT;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_5x5_SFLOAT_BLOCK_EXT" ) == 0 ) {
        return VK_FORMAT_ASTC_5x5_SFLOAT_BLOCK_EXT;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_6x5_SFLOAT_BLOCK_EXT" ) == 0 ) {
        return VK_FORMAT_ASTC_6x5_SFLOAT_BLOCK_EXT;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_6x6_SFLOAT_BLOCK_EXT" ) == 0 ) {
        return VK_FORMAT_ASTC_6x6_SFLOAT_BLOCK_EXT;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_8x5_SFLOAT_BLOCK_EXT" ) == 0 ) {
        return VK_FORMAT_ASTC_8x5_SFLOAT_BLOCK_EXT;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_8x6_SFLOAT_BLOCK_EXT" ) == 0 ) {
        return VK_FORMAT_ASTC_8x6_SFLOAT_BLOCK_EXT;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_8x8_SFLOAT_BLOCK_EXT" ) == 0 ) {
        return VK_FORMAT_ASTC_8x8_SFLOAT_BLOCK_EXT;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_10x5_SFLOAT_BLOCK_EXT" ) == 0 ) {
        return VK_FORMAT_ASTC_10x5_SFLOAT_BLOCK_EXT;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_10x6_SFLOAT_BLOCK_EXT" ) == 0 ) {
        return VK_FORMAT_ASTC_10x6_SFLOAT_BLOCK_EXT;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_10x8_SFLOAT_BLOCK_EXT" ) == 0 ) {
        return VK_FORMAT_ASTC_10x8_SFLOAT_BLOCK_EXT;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_10x10_SFLOAT_BLOCK_EXT" ) == 0 ) {
        return VK_FORMAT_ASTC_10x10_SFLOAT_BLOCK_EXT;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_12x10_SFLOAT_BLOCK_EXT" ) == 0 ) {
        return VK_FORMAT_ASTC_12x10_SFLOAT_BLOCK_EXT;
    }
    if ( strcmp( format, "VK_FORMAT_ASTC_12x12_SFLOAT_BLOCK_EXT" ) == 0 ) {
        return VK_FORMAT_ASTC_12x12_SFLOAT_BLOCK_EXT;
    }
    if ( strcmp( format, "VK_FORMAT_G8_B8R8_2PLANE_444_UNORM_EXT" ) == 0 ) {
        return VK_FORMAT_G8_B8R8_2PLANE_444_UNORM_EXT;
    }
    if ( strcmp( format, "VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16_EXT" ) == 0 ) {
        return VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16_EXT;
    }
    if ( strcmp( format, "VK_FORMAT_G12X4_B12X4R12X4_2PLANE_444_UNORM_3PACK16_EXT" ) == 0 ) {
        return VK_FORMAT_G12X4_B12X4R12X4_2PLANE_444_UNORM_3PACK16_EXT;
    }
    if ( strcmp( format, "VK_FORMAT_G16_B16R16_2PLANE_444_UNORM_EXT" ) == 0 ) {
        return VK_FORMAT_G16_B16R16_2PLANE_444_UNORM_EXT;
    }
    if ( strcmp( format, "VK_FORMAT_A4R4G4B4_UNORM_PACK16_EXT" ) == 0 ) {
        return VK_FORMAT_A4R4G4B4_UNORM_PACK16_EXT;
    }
    if ( strcmp( format, "VK_FORMAT_A4B4G4R4_UNORM_PACK16_EXT" ) == 0 ) {
        return VK_FORMAT_A4B4G4R4_UNORM_PACK16_EXT;
    }

    RASSERT( false );
    return VK_FORMAT_UNDEFINED;
}

void add_binding_if_unique( DescriptorSetLayoutInfo& creation, DescriptorSetLayoutInfo::Binding& binding ) {
    bool found = false;
    for ( u32 i = 0; i < creation.num_bindings; ++i ) {
        const DescriptorSetLayoutInfo::Binding& b = creation.bindings[ i ];
        if ( b.type == binding.type && b.index == binding.index ) {
            found = true;
            break;
        }
    }

    if ( !found ) {
        creation.add_binding( binding );
    }
}

bool has_binding( const DescriptorSetLayout* set_layout, u32 index ) {
    bool is_set = ( set_layout->binding_bitset & ( u64(1) << (u64)index ) ) != 0;

    return is_set;
}

// Range helpers /////////////////////////////////////////////////////////
VkImageSubresourceRange range_aspect( VkImageAspectFlags aspect,
                                      u32 base_mip, u32 mip_count,
                                      u32 base_layer, u32 layer_count ) {
    VkImageSubresourceRange r{};
    r.aspectMask = aspect;
    r.baseMipLevel = base_mip;
    r.levelCount = mip_count;
    r.baseArrayLayer = base_layer;
    r.layerCount = layer_count;
    return r;
}

VkImageSubresourceRange range_color_full() {
    return range_aspect( VK_IMAGE_ASPECT_COLOR_BIT,
                         0, VK_REMAINING_MIP_LEVELS,
                         0, VK_REMAINING_ARRAY_LAYERS );
}

VkImageSubresourceRange range_color( u32 base_mip, u32 mip_count,
                                     u32 base_layer, u32 layer_count ) {
    return range_aspect( VK_IMAGE_ASPECT_COLOR_BIT,
                         base_mip, mip_count,
                         base_layer, layer_count );
}

VkImageSubresourceRange range_depth_full( bool include_stencil ) {
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    if ( include_stencil ) {
        aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }

    return range_aspect( aspect,
                         0, VK_REMAINING_MIP_LEVELS,
                         0, VK_REMAINING_ARRAY_LAYERS );
}

VkImageSubresourceRange range_depth( u32 base_mip, u32 mip_count,
                                     u32 base_layer, u32 layer_count,
                                     bool include_stencil ) {
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    if ( include_stencil ) {
        aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }

    return range_aspect( aspect,
                         base_mip, mip_count,
                         base_layer, layer_count );
}

} // namespace raptor
