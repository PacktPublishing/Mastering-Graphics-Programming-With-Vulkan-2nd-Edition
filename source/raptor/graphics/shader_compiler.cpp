#include "shader_compiler.hpp"

#include "foundation/file.hpp"
#include "foundation/memory.hpp"
#include "foundation/numerics.hpp"
#include "foundation/process.hpp"
#include "foundation/string.hpp"

#include "graphics/gpu_resources.hpp"
#include "graphics/renderer.hpp"
#include "graphics/spirv_parser.hpp"
#include "graphics/shader_reflection.hpp"

#include "external/json.hpp"

#include <slang/slang.h>
#include <slang/slang-com-ptr.h>

#include "external/SPIRV-Reflect/spirv_reflect.h"

#if defined(_MSC_VER)
#include <windows.h>
#endif

namespace raptor
{

// Slang support //////////////////////////////////////////////////////////
static slang::IGlobalSession* s_slang_global_session = nullptr;
static slang::ISession* s_slang_session = nullptr;

static cstring to_slang_stage( VkShaderStageFlagBits value ) {
    switch ( value ) {
        case VK_SHADER_STAGE_VERTEX_BIT:
            return "vertex";
        case VK_SHADER_STAGE_FRAGMENT_BIT:
            return "fragment";
        case VK_SHADER_STAGE_COMPUTE_BIT:
            return "compute";
        case VK_SHADER_STAGE_MESH_BIT_EXT:
            return "mesh";
        case VK_SHADER_STAGE_TASK_BIT_EXT:
            return "amplification";
        case VK_SHADER_STAGE_RAYGEN_BIT_KHR:
            return "raygeneration";
        case VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR:
            return "closesthit";
        case VK_SHADER_STAGE_ANY_HIT_BIT_KHR:
            return "anyhit";
        case VK_SHADER_STAGE_MISS_BIT_KHR:
            return "miss";
        default:
            return "";
    }
}

static cstring to_slang_shader_profiler( VkShaderStageFlagBits stage ) {
    switch ( stage ) {
        case VK_SHADER_STAGE_VERTEX_BIT:        return "vs_6_5";
        case VK_SHADER_STAGE_FRAGMENT_BIT:      return "ps_6_5";
        case VK_SHADER_STAGE_COMPUTE_BIT:       return "cs_6_5";
        case VK_SHADER_STAGE_GEOMETRY_BIT:      return "gs_6_5";
        case VK_SHADER_STAGE_TASK_BIT_EXT:      return "task_6_5";
        case VK_SHADER_STAGE_MESH_BIT_EXT:      return "mesh_6_5";
        case VK_SHADER_STAGE_RAYGEN_BIT_KHR:        return "rgs_6_5";
        case VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR:    return "chs_6_5";
        case VK_SHADER_STAGE_ANY_HIT_BIT_KHR:        return "anyhit_6_5";
        case VK_SHADER_STAGE_MISS_BIT_KHR:          return "miss_6_5";
        case VK_SHADER_STAGE_INTERSECTION_BIT_KHR:  return "intersection_6_5";
        case VK_SHADER_STAGE_CALLABLE_BIT_KHR:      return "callable_6_5";
        default:                         return nullptr;
    }
}

// helper method
static bool is_end_of_line( char c ) {
    bool result = ( ( c == '\n' ) || ( c == '\r' ) );
    return( result );
}

char             ShaderCompiler::vulkan_binaries_path[ 512 ];
StringBuffer     ShaderCompiler::string_buffer;
StringBuffer     ShaderCompiler::path_buffer;
FlatHashMap<u64, u64> ShaderCompiler::shader_file_hash_cache;

void ShaderCompiler::init( Allocator* allocator ) {
    string_buffer.init( rmega( 1 ), allocator );
    path_buffer.init( rmega( 1 ), allocator );

    // Get binaries path
#if defined(_MSC_VER)
    char* vulkan_env = string_buffer.reserve( 512 );
    ExpandEnvironmentStringsA( "%VULKAN_SDK%", vulkan_env, 512 );
    char* compiler_path = string_buffer.append_use_f( "%s\\Bin\\", vulkan_env );
#else
    char* vulkan_env = getenv ("VULKAN_SDK");
    char* compiler_path = string_buffer.append_use_f( "%s/bin/", vulkan_env );
#endif

    strcpy( vulkan_binaries_path, compiler_path );
    string_buffer.clear();

    shader_file_hash_cache.init( allocator, rkilo( 16) );
}

void ShaderCompiler::shutdown() {
    string_buffer.shutdown();
    path_buffer.shutdown();
    shader_file_hash_cache.shutdown();
}

static u64 shader_concatenate_from_file( cstring filename, StringBuffer& path_buffer, StringBuffer& shader_buffer, ArenaAllocator* temp_allocator ) {

    sizet current_marker = temp_allocator->get_marker();

    u64 hashed_memory = 0;
    // Read file and concatenate it
    path_buffer.clear();
    cstring shader_path = path_buffer.append_use_f( "%s%s", RAPTOR_SHADER_FOLDER, filename );
    FileReadResult shader_read_result = file_read_text( shader_path, temp_allocator );
    if ( shader_read_result.data ) {
        // Append without null termination and add termination later.
        shader_buffer.append_m( shader_read_result.data, strlen( shader_read_result.data ) );
        // Using strlne because file can contain impurities after the end, causing different hashes when using shader_read_result.size.
        hashed_memory = hash_bytes( shader_read_result.data, strlen( shader_read_result.data ) );
    } else {
        rprint( "Cannot read file %s\n", shader_path );
    }

    temp_allocator->free_marker( current_marker );

    return hashed_memory;
}

static void compute_slang_hash( ShaderCompilationStage& shader_stage, StringBuffer& path_buffer, StringBuffer& shader_buffer, ArenaAllocator* temp_allocator ) {
    path_buffer.clear();

    u64 hashed_memory = 0;

    cstring shader_path = path_buffer.append_use_f( "%s%s", RAPTOR_SHADER_FOLDER, shader_stage.source_file_path );
    FileReadResult shader_read_result = file_read_text( shader_path, temp_allocator );
    if ( shader_read_result.data ) {
        // NOTE(marco): do this here as strtok messes with the data
        shader_buffer.append_m( shader_read_result.data, strlen( shader_read_result.data ) );
        hashed_memory = hash_bytes( shader_read_result.data, strlen( shader_read_result.data ) );
        shader_stage.shader_file_hashes.push( hashed_memory );

        cstring line = strtok( shader_read_result.data, "\n" );
        while ( line ) {
            cstring include = strstr( line, "#include" );
            if ( include ) {
                cstring quote = strstr( include, "\"");
                cstring next_quote = strstr( quote + 1, "\"" );

                StringView header_file( quote + 1, next_quote - ( quote + 1 ) );
                // TODO(marco): hack, read folder relative path from source_file
                // TODO(marco): don't parse the same header multiple times
                cstring header_path = path_buffer.data + path_buffer.current_size;
                path_buffer.append_f( "%s%s/", RAPTOR_SHADER_FOLDER, "slang" );
                path_buffer.append_use( header_file );

                FileReadResult header_read_result = file_read_text( header_path, temp_allocator );
                if ( header_read_result.data ) {
                    hashed_memory = hash_bytes( header_read_result.data, strlen( header_read_result.data ) );

                    shader_stage.shader_file_hashes.push( hashed_memory );
                }
            }

            line = strtok( NULL, "\n" );
        }
    } else {
        rprint( "Cannot read file %s\n", shader_path );
    }
}

static void reflect_slang_shader( slang::ProgramLayout* layout, ShaderReflection* reflection,
                                  ArenaAllocator* temp_allocator, StringBuffer* name_buffer ) {

    u32 par_count = layout->getParameterCount();
    for ( u32 p = 0; p < par_count; p++ ) {
        slang::VariableLayoutReflection* var_reflection = layout->getParameterByIndex( p );
        slang::TypeLayoutReflection* type_layout = var_reflection->getTypeLayout();

        u32 set_count = ( u32 )type_layout->getDescriptorSetCount();
        cstring name = var_reflection->getName();
        VkDescriptorType descriptor_type = VK_DESCRIPTOR_TYPE_MAX_ENUM;

        for ( size_t i = 0; i < ( size_t )type_layout->getBindingRangeCount(); i++ ) {

            slang::BindingType binding_type = type_layout->getBindingRangeType( i );
            //rprint( " Found binding type %d for variable %s\n", (int)binding_type, name );

            switch ( binding_type ) {
                case slang::BindingType::ConstantBuffer:
                {
                    descriptor_type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                    break;
                }
                case slang::BindingType::ParameterBlock:
                {
                    break;
                }
                case slang::BindingType::Sampler:
                {
                    descriptor_type = VK_DESCRIPTOR_TYPE_SAMPLER;
                    break;
                }
                case slang::BindingType::Texture:
                {
                    descriptor_type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                    break;
                }
                case slang::BindingType::RayTracingAccelerationStructure:
                {
                    descriptor_type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
                    break;
                }
                case slang::BindingType::MutableTexture:
                {
                    descriptor_type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                    break;
                }
                case slang::BindingType::RawBuffer:
                {
                    descriptor_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    break;
                }
                default:
                {
                    break;
                }
            }
        }


        int used_layout_unit_count = var_reflection->getCategoryCount();
        u32 set_index = 0;
        for ( int i = 0; i < used_layout_unit_count; ++i ) {
            auto layout_unit = var_reflection->getCategoryByIndex( i );
            size_t space_offset = var_reflection->getBindingSpace( layout_unit );

            switch ( layout_unit ) {
                case slang::ParameterCategory::DescriptorTableSlot:
                {
                    set_index = ( u32 )space_offset;

                    u32 set = static_cast<u32>( var_reflection->getBindingSpace( layout_unit ) );
                    u32 binding_index = static_cast<u32>( var_reflection->getOffset( layout_unit ) );

                    if ( set_index != set ) {
                        rprint( "DescriptorTableSlot mismatch found: %s set %u, binding %u\n", name, set, binding_index );
                    }

                    if ( set > 0 ) {
                        // Search for set
                        i32 set_array_index = -1;
                        for ( u32 s = 0; s < reflection->sets.size; ++s ) {
                            if ( reflection->sets[ s ].set_index == set_index ) {
                                set_array_index = s;
                                break;
                            }
                        }
                        // Set not found, add a new one
                        if ( set_array_index < 0 ) {
                            set_array_index = reflection->sets.size;
                            DescriptorSetReflection& set_reflection = reflection->sets.push_use();
                            set_reflection.set_index = set_index;
                        }

                        DescriptorSetReflection& set_reflection = reflection->sets[ set_array_index ];

                        DescriptorBinding2 binding{ };
                        binding.index = binding_index;
                        binding.name = name_buffer->append_use( name );
                        binding.type = descriptor_type;

                        bool found = false;
                        for ( u32 i = 0; i < set_reflection.bindings.size; ++i ) {
                            const DescriptorBinding2& b = set_reflection.bindings[ i ];
                            if ( b.type == binding.type && b.index == binding.index ) {
                                found = true;
                                break;
                            }
                        }

                        if ( !found ) {
                            set_reflection.bindings.push( binding );
                        }
                    }

                    break;
                }

                case slang::ParameterCategory::PushConstantBuffer:
                {
                    slang::TypeLayoutReflection* element_tl = type_layout->getElementTypeLayout();

                    u32 push_constant_stride = (u32)element_tl->getStride();
                    u32 push_constant_size = (u32)element_tl->getSize();

                    reflection->push_constants_stride = raptor::max( reflection->push_constants_stride, push_constant_stride );

                    //int field_count = element_tl->getFieldCount();
                    //for ( int f = 0; f < field_count; ++f ) {
                    //    slang::VariableLayoutReflection* field_vl = element_tl->getFieldByIndex( f );
                    //    const char* field_name = field_vl->getName();

                    //    size_t byte_offset = field_vl->getOffset( slang::ParameterCategory::PushConstantBuffer );
                    //    size_t byte_size = field_vl->getTypeLayout()->getSize();

                    //    // Record: field_name, byte_offset, byte_size
                    //    rprint( "" );
                    //}
                    break;
                }

                case slang::ParameterCategory::SpecializationConstant:
                {
                    slang::TypeReflection* type_reflection = type_layout->getType();

                    StaticString64& spec_name = reflection->specialization_names.push_use();
                    spec_name.append( "%s", name );
                    const u32 binding_index = var_reflection->getBindingIndex();

                    bool found = false;
                    // Search for existing specialization constant
                    for ( u32 s = 0; s < reflection->specialization_constants.size; ++s ) {
                        SpecializationConstant& existing_spec_constant = reflection->specialization_constants[ s ];
                        if ( existing_spec_constant.binding == binding_index ) {
                            // Already recorded
                            found = true;
                            break;
                        }
                    }

                    if ( found ) {
                        break;
                    }

                    SpecializationConstant& specialization_constant = reflection->specialization_constants.push_use();
                    specialization_constant.binding = (u16)binding_index;
                    //specialization_constant.byte_stride = (u16)type_layout->getSize( slang::ParameterCategory::SpecializationConstant );
                    specialization_constant.byte_stride = 4;

                    switch ( type_reflection->getScalarType() ) {

                        case slang::TypeReflection::ScalarType::Int32:
                        {
                            specialization_constant.type = SpecializationConstant::Type::Type_i32;
                            break;
                        }
                        case slang::TypeReflection::ScalarType::UInt32:
                        {
                            specialization_constant.type = SpecializationConstant::Type::Type_u32;
                            break;
                        }
                        case slang::TypeReflection::ScalarType::Float32:
                        {
                            specialization_constant.type = SpecializationConstant::Type::Type_f32;
                            break;
                        }
                        // Unsupported types
                        case slang::TypeReflection::ScalarType::Bool:
                        case slang::TypeReflection::ScalarType::Int64:
                        case slang::TypeReflection::ScalarType::UInt64:
                        case slang::TypeReflection::ScalarType::Float64:
                        default:
                        {
                            RASSERT( false );
                            break;
                        }
                    }

                    break;
                }

                default:
                {
                    break;
                }
            }
        }

        if ( set_index == 0 ) {
            // NOTE(marco): ignore bindless set parsing, but mark it for add
            reflection->add_global_set = 1;
            continue;
        }

#if 0
        for ( u32 set = 0; set < set_count; ++set ) {
            u32 offset = type_layout->getDescriptorSetSpaceOffset( set );
            i64 range = type_layout->getDescriptorSetDescriptorRangeCount( set + offset );
            u32 binding_index = type_layout->getDescriptorSetDescriptorRangeIndexOffset( set + offset, 0 );
            slang::BindingType binding_type = type_layout->getDescriptorSetDescriptorRangeType( set + offset, 0 );

            // Search for set
            i32 set_array_index = -1;
            for ( u32 s = 0; s < reflection->sets.size; ++s ) {
                if ( reflection->sets[ s ].set_index == set_index ) {
                    set_array_index = s;
                    break;
                }
            }
            // Set not found, add a new one
            if ( set_array_index < 0 ) {
                set_array_index = reflection->sets.size;
                DescriptorSetReflection& set_reflection = reflection->sets.push_use();
                set_reflection.set_index = set_index;
            }

            DescriptorSetReflection& set_reflection = reflection->sets[ set_array_index ];

            DescriptorBinding2 binding{ };
            binding.index = binding_index;
            binding.name = name_buffer->append_use( name );

            bool valid = false;
            switch ( binding_type ) {
                case slang::BindingType::MutableTypedBuffer:
                case slang::BindingType::MutableRawBuffer:
                    binding.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    valid = true;
                    break;
                case slang::BindingType::TypedBuffer:
                case slang::BindingType::RawBuffer:
                case slang::BindingType::ConstantBuffer:
                    binding.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                    valid = true;
                    break;
                case slang::BindingType::CombinedTextureSampler:
                    binding.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    valid = true;
                    break;
                case slang::BindingType::MutableTexture:
                    binding.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                    valid = true;
                    break;
                case slang::BindingType::RayTracingAccelerationStructure:
                    binding.type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
                    valid = true;
                    break;
                case slang::BindingType::PushConstant:
                    // TODO(marco)
                    // parse_result->push_constants_stride = push_constants_type.width;
                    valid = true;
                    break;
                case slang::BindingType::InlineUniformData:
                    // TODO(marco)
                    // Cache specialization value
                    // SpecializationConstant& specialization_constant = parse_result->specialization_constants[ parse_result->specialization_constants_count ];
                    // specialization_constant.binding = id_spec_binding.binding;
                    // specialization_constant.byte_stride = id.width / 8;
                    // specialization_constant.default_value = id.value;

                    // Cache specialization name to lookup
                    // SpecializationName& specialization_name = parse_result->specialization_names[ parse_result->specialization_constants_count ];
                    // raptor::StringView::copy_to( id_spec_binding.name, specialization_name.name, 32 );

                    // ++parse_result->specialization_constants_count;
                    valid = true;
                    break;
                default:
                    break;
            }

            if ( valid ) {
                bool found = false;
                for ( u32 i = 0; i < set_reflection.bindings.size; ++i ) {
                    const DescriptorBinding2& b = set_reflection.bindings[ i ];
                    if ( b.type == binding.type && b.index == binding.index ) {
                        found = true;
                        break;
                    }
                }

                if ( !found ) {
                    set_reflection.bindings.push( binding );
                }
            }
        }
#endif // 0
    }
}

static void reflect_glsl_shader( VkShaderModuleCreateInfo& shader_create_info, ShaderReflection* reflection,
                                 ArenaAllocator* temp_allocator, StringBuffer* name_buffer ) {

    SpvReflectShaderModule module = {};

    auto check_and_error = [ & ]( SpvReflectResult result, StringView& name, cstring message, SpvReflectShaderModule& module ) -> bool {
        if ( result != SPV_REFLECT_RESULT_SUCCESS ) {
            rprint( "%s from %s\n", message, name.data );

            spvReflectDestroyShaderModule( &module );
        }

        return true;
    };

    auto add_unique_binding = [ & ]( StaticArray<DescriptorBinding2, k_max_descriptors_per_set>& bindings,
                                     cstring name, SpvReflectDescriptorBinding* binding, VkDescriptorType type ) {
        bool found = false;
        for ( u32 i = 0; i < bindings.size; ++i ) {
            const DescriptorBinding2& b = bindings[ i ];
            if ( b.type == type && b.index == binding->binding ) {
                found = true;
                break;
            }
        }

        if ( !found ) {
            DescriptorBinding2& new_binding = bindings.push_use();
            new_binding.type = type;
            new_binding.index = binding->binding;
            new_binding.count = binding->count;
            new_binding.name = name_buffer->append_use( name );
        }
    };

    StringView name = "not";

    SpvReflectResult result = spvReflectCreateShaderModule( shader_create_info.codeSize,
                                                            shader_create_info.pCode, &module );

    if ( !check_and_error( result, name, "Error reading spirv module", module ) ) {
        return;
    }

    uint32_t count = 0;

    result = spvReflectEnumerateDescriptorSets( &module, &count, NULL );
    if ( !check_and_error( result, name, "Error reading spirv module count", module ) ) {
        return;
    }

    std::vector<SpvReflectDescriptorSet*> sets( count );
    result = spvReflectEnumerateDescriptorSets( &module, &count, sets.data() );

    if ( !check_and_error( result, name, "Error reading descriptor set from spirv module", module ) ) {
        return;
    }

    // Read all bindings
    for ( u32 i = 0; i < sets.size(); ++i ) {
        const SpvReflectDescriptorSet* ds = sets[ i ];

        // Skip global set
        if ( ds->set == 0 ) {
            reflection->add_global_set = 1;
            continue;
        }

        // Search for set
        i32 set_array_index = -1;
        for ( u32 s = 0; s < reflection->sets.size; ++s ) {
            if ( reflection->sets[ s ].set_index == ds->set ) {
                set_array_index = s;
                break;
            }
        }
        // Set not found, add a new one
        if ( set_array_index < 0 ) {
            set_array_index = reflection->sets.size;
            DescriptorSetReflection& set_reflection = reflection->sets.push_use();
            set_reflection.set_index = ds->set;
        }

        DescriptorSetReflection& set_reflection = reflection->sets[ set_array_index ];

        for ( u32 b = 0; b < ds->binding_count; ++b ) {
            SpvReflectDescriptorBinding* binding = ds->bindings[ b ];

            cstring name = strlen( binding->name ) > 0 ? binding->name : binding->type_description->type_name;
            u64 name_hash = hash_calculate( (cstring)name );

            switch ( binding->descriptor_type ) {
                case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER:
                {
                    add_unique_binding( set_reflection.bindings, name, binding, VK_DESCRIPTOR_TYPE_SAMPLER );

                    break;
                }

                case SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                {
                    add_unique_binding( set_reflection.bindings, name, binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER );
                    break;
                }

                case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                {

                    break;
                }

                case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                {
                    add_unique_binding( set_reflection.bindings, name, binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE );
                    break;
                }

                case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                {
                    rprint( "Invalid descriptor type %u", binding->descriptor_type );
                    break;
                }

                case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                {
                    rprint( "Invalid descriptor type %u", binding->descriptor_type );
                    break;
                }

                case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                {
                    add_unique_binding( set_reflection.bindings, name, binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER );
                    break;
                }

                case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                {
                    add_unique_binding( set_reflection.bindings, name, binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER );

                    break;
                }

                case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
                {
                    rprint( "Invalid descriptor type %u", binding->descriptor_type );
                    break;
                }

                case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                {
                    rprint( "Invalid descriptor type %u", binding->descriptor_type );
                    break;
                }

                case SPV_REFLECT_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                {
                    rprint( "Invalid descriptor type %u", binding->descriptor_type );
                    break;
                }

                case SPV_REFLECT_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
                {
                    add_unique_binding( set_reflection.bindings, name, binding, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR );
                    break;
                }

                default:
                {
                    rprint( "Invalid descriptor type %u", binding->descriptor_type );
                }
            }
        }
    }

    // Read push constants
    for ( u32 i = 0; i < module.push_constant_block_count; ++i ) {
        SpvReflectBlockVariable& push_constant = module.push_constant_blocks[ i ];

        reflection->push_constants_stride = raptor::max(push_constant.size, reflection->push_constants_stride);
    }

    // Read specialization constants
    for ( u32 i = 0; i < module.spec_constant_count; ++i ) {
        SpvReflectSpecializationConstant& spec_constant = module.spec_constants[ i ];

        // Search for existing specialization constant
        for ( u32 s = 0; s < reflection->specialization_constants.size; ++s ) {
            SpecializationConstant& existing_spec_constant = reflection->specialization_constants[ s ];
            if ( existing_spec_constant.binding == spec_constant.constant_id ) {
                // Already recorded
                continue;
            }
        }

        StaticString64& spec_name = reflection->specialization_names.push_use();
        spec_name.append( "%s", spec_constant.name );

        SpecializationConstant& specialization_constant = reflection->specialization_constants.push_use();
        specialization_constant.binding = u16( spec_constant.constant_id );
        specialization_constant.byte_stride = 0;
        switch ( spec_constant.type_description->op ) {
            case SpvOpTypeInt:
            {
                if ( spec_constant.type_description->traits.numeric.scalar.signedness ) {
                    specialization_constant.type = SpecializationConstant::Type::Type_i32;
                    specialization_constant.value.value_i = *( ( i32* )spec_constant.default_value );
                } else {
                    specialization_constant.type = SpecializationConstant::Type::Type_u32;
                    specialization_constant.value.value_u = *( ( u32* )spec_constant.default_value );
                }
                specialization_constant.byte_stride = 4;
                break;
            }
            case SpvOpTypeFloat:
            {
                specialization_constant.type = SpecializationConstant::Type::Type_f32;
                specialization_constant.value.value_f = *( ( f32* )spec_constant.default_value );
                specialization_constant.byte_stride = 4;
                break;
            }
            default:
            {
                rprint( "Unsupported specialization constant type %u\n", spec_constant.type_description->op );
            }
        }
    }

    // Can set a breakpoint here and explorer the various variables enumerated.
    spvReflectDestroyShaderModule( &module );

#if 0

#endif
}

void ShaderCompiler::calculate_shader_hash( ShaderCompilationStage& shader_stage ) {
    u64 hashed_memory = hash_bytes( (void*)shader_stage.source_code.data, strlen( shader_stage.source_code.data ) );

    shader_stage.shader_file_hashes.push( hashed_memory );
}

static i32 parse_line( cstring input, u32 input_size, u32 offset, u32* out_next_offset, char* out_line, u32 max_line_size ) {
    u32 line_offset = 0;
    u32 old_offset = offset;
    while ( offset < input_size && !is_end_of_line( input[ offset ] ) && line_offset < max_line_size - 1 ) {
        out_line[ line_offset++ ] = input[ offset++ ];
    }

    out_line[ line_offset ] = '\0';

    // Skip end of line characters
    while ( offset < input_size && is_end_of_line( input[ offset ] ) ) {
        offset++;
    }

    *out_next_offset = offset;

    if ( old_offset == offset ) {
        return -1;
    }

    return line_offset;
}

void ShaderCompiler::read_source_from_file_and_add_hashes( ShaderCompilationStage& shader_stage, StringBuffer& shader_buffer, ArenaAllocator* temp_allocator ) {
    // Read file and concatenate it
    // Cache current shader code beginning
    cstring source_code = shader_buffer.current();

    for ( u32 i = 0; i < shader_stage.headers.size; ++i ) {
        u64 shader_file_hash = shader_concatenate_from_file( shader_stage.headers[ i ], path_buffer, shader_buffer, temp_allocator );
        // Cache file hashes
        shader_stage.shader_file_hashes.push( shader_file_hash );
    }

    if ( shader_stage.source_file_path != nullptr ) {
        if ( strstr( shader_stage.source_file_path, ".slang" ) != nullptr ) {
            compute_slang_hash( shader_stage, path_buffer, shader_buffer, temp_allocator );
        } else {
            // Concatenate main shader code
            u64 shader_file_hash = shader_concatenate_from_file( shader_stage.source_file_path, path_buffer, shader_buffer, temp_allocator );
            // Cache main shader code hash
            shader_stage.shader_file_hashes.push( shader_file_hash );
            //rprint( " %016llx", shader_file_hash );
        }
    }
    // Add terminator for final string.
    shader_buffer.close_current_string();

    u32 source_code_size = ( u32 )strlen( source_code );

    // If we use includes, we need to parse them to add their hashes. The current implementation is not recursive and
    // not optimized
    u32 read = 0;
    u32 offset = 0;
    const u32 buffer_size = 256;
    char line[ buffer_size ]{ };

    char relative_path_buffer[ 512 ]{ };
    const char* relative_path = strrchr( shader_stage.source_file_path, '/' );
    if ( relative_path ) {
        strncpy( relative_path_buffer, shader_stage.source_file_path, relative_path - shader_stage.source_file_path + 1 );
    }

    while ( ( read = parse_line( source_code, source_code_size, offset, &offset, line, buffer_size ) ) != -1) {
        if (  strstr( line, "#include" ) != nullptr ) {
            cstring quote = strstr( line, "\"");
            cstring next_quote = strstr( quote + 1, "\"" );

            StringView header_file( quote + 1, next_quote - ( quote + 1 ) );

            path_buffer.clear();

            cstring header_path = path_buffer.current();
            path_buffer.append_f( "%s%s", RAPTOR_SHADER_FOLDER, relative_path_buffer );
            path_buffer.append( header_file );
            path_buffer.close_current_string();

            FileReadResult header_read_result = file_read_text( header_path, temp_allocator );
            if ( header_read_result.data ) {
                u64 shader_file_hash = hash_bytes( header_read_result.data, strlen( header_read_result.data ) );

                u64* end = shader_stage.shader_file_hashes.data + shader_stage.shader_file_hashes.size;
                u64* entry = std::find( shader_stage.shader_file_hashes.data, end, shader_file_hash );
                if ( entry == end ) {
                    shader_stage.shader_file_hashes.push( shader_file_hash );
                }
            }
        }
    }

    // Debug print of final code if needed.
    //rprint( "\n\n%s\n\n\n", code );

    shader_stage.source_code.data = source_code;
    shader_stage.source_code.size = source_code_size;
}

// TODO: move
// Helpers: write/read primitives as little-endian (host is assumed little-endian on PC)
template <class T>
static bool write_pod( FILE* f, const T& v ) { return std::fwrite( &v, sizeof( T ), 1, f ) == 1; }
template <class T>
static bool read_pod( FILE* f, T& v ) { return std::fread( &v, sizeof( T ), 1, f ) == 1; }

static bool write_bytes( FILE* f, const void* p, size_t n ) { return std::fwrite( p, 1, n, f ) == n; }
static bool read_bytes( FILE* f, void* p, size_t n ) { return std::fread( p, 1, n, f ) == n; }

// Strings: serialize as length + bytes (no pointer)
static bool write_string( FILE* f, const char* s ) {
    u16 len = s ? (u16)std::min<size_t>( std::strlen( s ), 0xFFFF ) : 0;
    if ( !write_pod( f, len ) ) return false;
    return len ? write_bytes( f, s, len ) : true;
}

static bool read_string( FILE* f, std::string& out ) {
    u16 len = 0; if ( !read_pod( f, len ) ) return false;
    out.resize( len );
    return len ? read_bytes( f, out.data(), len ) : true;
}

inline bool save_shader_reflection_binary( const ShaderReflection& sr, const char* path ) {
    FILE* f = std::fopen( path, "wb" );
    if ( !f ) {
        return false;
    }

    const u32 k_magic = 0x53485246; // 'SHRF'
    const u32 k_version = 1;

    // Header
    if ( !write_pod( f, k_magic ) ) {
        std::fclose( f );
        return false;
    }
    if ( !write_pod( f, k_version ) ) {
        std::fclose( f );
        return false;
    }

    // Scalars
    if ( !write_pod( f, sr.push_constants_stride ) ) {
        std::fclose( f );
        return false;
    }
    if ( !write_pod( f, sr.add_global_set ) ) {
        std::fclose( f );
        return false;
    }

    // Descriptor sets
    if ( !write_pod( f, sr.sets.size ) ) {
        std::fclose( f );
        return false;
    }

    for ( u32 si = 0; si < sr.sets.size; ++si ) {
        const DescriptorSetReflection& set = sr.sets.data[ si ];

        if ( !write_pod( f, set.set_index ) ) {
            std::fclose( f );
            return false;
        }

        if ( !write_pod( f, set.bindings.size ) ) {
            std::fclose( f );
            return false;
        }

        for ( u32 bi = 0; bi < set.bindings.size; ++bi ) {
            const DescriptorBinding2& b = set.bindings.data[ bi ];

            u32 type_u32 = static_cast<u32>( b.type );
            if ( !write_pod( f, type_u32 ) ) {
                std::fclose( f );
                return false;
            }

            if ( !write_pod( f, b.index ) ) {
                std::fclose( f );
                return false;
            }

            if ( !write_pod( f, b.count ) ) {
                std::fclose( f );
                return false;
            }

            if ( !write_string( f, b.name ) ) {
                std::fclose( f );
                return false;
            }
        }
    }

    // Specialization constants
    if ( !write_pod( f, sr.specialization_constants.size ) ) {
        std::fclose( f );
        return false;
    }

    for ( u32 i = 0; i < sr.specialization_constants.size; ++i ) {
        const SpecializationConstant& sc = sr.specialization_constants.data[ i ];

        u8 type_u8 = static_cast<u8>( sc.type );
        if ( !write_pod( f, type_u8 ) ) {
            std::fclose( f );
            return false;
        }

        if ( !write_pod( f, sc.binding ) ) {
            std::fclose( f );
            return false;
        }

        if ( !write_pod( f, sc.byte_stride ) ) {
            std::fclose( f );
            return false;
        }

        if ( sc.type == SpecializationConstant::Type::Type_i32 ) {
            if ( !write_pod( f, sc.value.value_i ) ) {
                std::fclose( f );
                return false;
            }
        }
        else if ( sc.type == SpecializationConstant::Type::Type_u32 ) {
            if ( !write_pod( f, sc.value.value_u ) ) {
                std::fclose( f );
                return false;
            }
        }
        else if ( sc.type == SpecializationConstant::Type::Type_f32 ) {
            if ( !write_pod( f, sc.value.value_f ) ) {
                std::fclose( f );
                return false;
            }
        }
        else {
            std::fclose( f );
            return false;
        }
    }

    // Specialization names
    if ( !write_pod( f, sr.specialization_names.size ) ) {
        std::fclose( f );
        return false;
    }

    for ( u32 i = 0; i < sr.specialization_names.size; ++i ) {
        char out_fixed[ 64 ];
        std::memset( out_fixed, 0, sizeof( out_fixed ) );
        std::memcpy( out_fixed, sr.specialization_names.data[ i ].data_, 63 );

        if ( !write_bytes( f, out_fixed, sizeof( out_fixed ) ) ) {
            std::fclose( f );
            return false;
        }
    }

    std::fclose( f );
    return true;
}

inline bool load_shader_reflection_binary( ShaderReflection& sr, cstring path,
                                           StringBuffer* name_buffer ) {


    FILE* f = std::fopen( path, "rb" );
    if ( !f ) {
        return false;
    }

    // Header
    u32 magic = 0;
    if ( !read_pod( f, magic ) ) {
        std::fclose( f );
        return false;
    }

    u32 version = 0;
    if ( !read_pod( f, version ) ) {
        std::fclose( f );
        return false;
    }

    if ( magic != 0x53485246 ) {
        std::fclose( f );
        return false;
    }

    if ( version != 1 ) {
        std::fclose( f );
        return false;
    }

    // Scalars
    if ( !read_pod( f, sr.push_constants_stride ) ) {
        std::fclose( f );
        return false;
    }

    if ( !read_pod( f, sr.add_global_set ) ) {
        std::fclose( f );
        return false;
    }

    // Sets
    sr.sets.size = 0;

    u32 sets_n = 0;
    if ( !read_pod( f, sets_n ) ) {
        std::fclose( f );
        return false;
    }

    if ( sets_n > k_max_descriptor_set_layouts ) {
        std::fclose( f );
        return false;
    }

    for ( u32 si = 0; si < sets_n; ++si ) {
        DescriptorSetReflection& set = sr.sets.data[ si ];

        if ( !read_pod( f, set.set_index ) ) {
            std::fclose( f );
            return false;
        }

        set.bindings.size = 0;

        u32 bind_n = 0;
        if ( !read_pod( f, bind_n ) ) {
            std::fclose( f );
            return false;
        }

        if ( bind_n > k_max_descriptors_per_set ) {
            std::fclose( f );
            return false;
        }

        for ( u32 bi = 0; bi < bind_n; ++bi ) {
            DescriptorBinding2& b = set.bindings.data[ bi ];

            u32 type_u32 = 0;
            if ( !read_pod( f, type_u32 ) ) {
                std::fclose( f );
                return false;
            }
            b.type = static_cast<VkDescriptorType>( type_u32 );

            if ( !read_pod( f, b.index ) ) {
                std::fclose( f );
                return false;
            }

            if ( !read_pod( f, b.count ) ) {
                std::fclose( f );
                return false;
            }

            std::string name;
            if ( !read_string( f, name ) ) {
                std::fclose( f );
                return false;
            }

            b.name = name_buffer->append_use( name.c_str() );

            set.bindings.size++;
        }

        sr.sets.size++;
    }

    // Specialization constants
    sr.specialization_constants.size = 0;

    u32 sc_n = 0;
    if ( !read_pod( f, sc_n ) ) {
        std::fclose( f );
        return false;
    }

    if ( sc_n > k_max_specialization_constants ) {
        std::fclose( f );
        return false;
    }

    for ( u32 i = 0; i < sc_n; ++i ) {
        SpecializationConstant& sc = sr.specialization_constants.data[ i ];

        u8 type_u8 = 0;
        if ( !read_pod( f, type_u8 ) ) {
            std::fclose( f );
            return false;
        }
        sc.type = static_cast<SpecializationConstant::Type>( type_u8 );

        if ( !read_pod( f, sc.binding ) ) {
            std::fclose( f );
            return false;
        }

        if ( !read_pod( f, sc.byte_stride ) ) {
            std::fclose( f );
            return false;
        }

        if ( sc.type == SpecializationConstant::Type::Type_i32 ) {
            if ( !read_pod( f, sc.value.value_i ) ) {
                std::fclose( f );
                return false;
            }
        }
        else if ( sc.type == SpecializationConstant::Type::Type_u32 ) {
            if ( !read_pod( f, sc.value.value_u ) ) {
                std::fclose( f );
                return false;
            }
        }
        else if ( sc.type == SpecializationConstant::Type::Type_f32 ) {
            if ( !read_pod( f, sc.value.value_f ) ) {
                std::fclose( f );
                return false;
            }
        }
        else {
            std::fclose( f );
            return false;
        }

        sr.specialization_constants.size++;
    }

    // Specialization names
    sr.specialization_names.size = 0;

    u32 sn_n = 0;
    if ( !read_pod( f, sn_n ) ) {
        std::fclose( f );
        return false;
    }

    if ( sn_n > k_max_specialization_constants ) {
        std::fclose( f );
        return false;
    }

    for ( u32 i = 0; i < sn_n; ++i ) {
        char buf[ 64 ];

        if ( !read_bytes( f, buf, sizeof( buf ) ) ) {
            std::fclose( f );
            return false;
        }

        buf[ 63 ] = '\0';

        std::memset( sr.specialization_names.data[ i ].data_, 0, 64 );
        std::memcpy( sr.specialization_names.data[ i ].data_, buf, 63 );

        sr.specialization_names.size++;
    }

    std::fclose( f );
    return true;
}


bool ShaderCompiler::compile_and_cache_shader( ShaderCompilationStage& compilation_stage, Span<const u32>& spirv_bytecode, cstring binary_data_folder,
                                               ArenaAllocator* temp_allocator, cstring technique_name, cstring shader_name,
                                               ShaderReflection* reflection, StringBuffer* name_buffer, bool use_cache,
                                               bool slang_input, bool ignore_layout, bool& shader_changed ) {
    // Do not compile shaders when parsing the parent technique
    bool compile_shader = true;
    cstring shader_spirv_path = nullptr;
    cstring shader_hash_path = nullptr;
    cstring shader_layout_path = nullptr;
    cstring shader_reflection_path = nullptr;

    //shader_stage.type = compilation_stage.type;

    if ( use_cache ) {
        // Check shader cache and eventually compile the code.
        path_buffer.clear();
        shader_spirv_path = path_buffer.append_use_f( "%s/%s_%s_%s.spv",
                                                        binary_data_folder, technique_name, shader_name,
                                                        to_compiler_extension( compilation_stage.type ) );
        shader_hash_path = path_buffer.append_use_f( "%s/%s_%s_%s.hash.cache",
                                                        binary_data_folder, technique_name, shader_name,
                                                        to_compiler_extension( compilation_stage.type ) );
        shader_layout_path = path_buffer.append_use_f( "%s/%s_%s_%s_layout.json",
                                                        binary_data_folder, technique_name, shader_name,
                                                        to_compiler_extension( compilation_stage.type ) );
        shader_reflection_path = path_buffer.append_use_f( "%s/%s_%s_%s_reflection.bin",
                                                        binary_data_folder, technique_name, shader_name,
                                                        to_compiler_extension( compilation_stage.type ) );

        bool cache_exists = file_exists( shader_hash_path );
		cache_exists = cache_exists && file_exists( shader_spirv_path );

        if ( slang_input ) {
            cache_exists = cache_exists && file_exists( shader_layout_path );
        }
        cache_exists = cache_exists && file_exists( shader_reflection_path );

        //rprint( "\nfile %s\n", shader_hash_path );
        // TODO: still not working
        if ( cache_exists ) {

            FileReadResult frr = file_read_binary( shader_hash_path, temp_allocator );
            if ( frr.data ) {

                u32 file_entries = u32( frr.size / sizeof( u64 ) );
                const u32 file_hashes_count = compilation_stage.shader_file_hashes.size;
                if ( file_entries == file_hashes_count ) {
                    u64* cached_file_hashes = ( u64* )frr.data;
                    u32 i = 0;
                    for ( ; i < file_hashes_count; ++i ) {
                        const u64 a = compilation_stage.shader_file_hashes[ i ];
                        const u64 b = cached_file_hashes[ i ];
                        if ( a != b ) {
                            break;
                        }
                    }

                    compile_shader = i != file_hashes_count;
                }
            }
        }
    }

    // Cache is not present or shader has changed, compile shaders.
    if ( compile_shader ) {
        slang::ProgramLayout* layout = nullptr;
        VkShaderModuleCreateInfo shader_create_info{ };
        if ( slang_input ) {
            shader_create_info = ShaderCompiler::compile_shader_slang(
                compilation_stage.source_code.data, ( u32 )compilation_stage.source_code.size,
                compilation_stage.type, shader_name, temp_allocator, &layout, compilation_stage.defines.as_span() );
        }
        else {
            shader_create_info = ShaderCompiler::compile_shader_glsl(
                compilation_stage.source_code.data, ( u32 )compilation_stage.source_code.size,
                compilation_stage.type, shader_name, temp_allocator, compilation_stage.defines.as_span() );
        }

        // Cache spirv bytecode
        if ( shader_create_info.pCode ) {
            spirv_bytecode.data = shader_create_info.pCode;
            spirv_bytecode.size = shader_create_info.codeSize;

            if ( use_cache ) {
                // Write hashes
                file_write_binary( shader_hash_path, &compilation_stage.shader_file_hashes[ 0 ], sizeof( u64 ) * compilation_stage.shader_file_hashes.size );
                /*rprint( "Hashes\n%016llx ", shader_file_hashes[0] );
                for (u32 i = 1; i < shader_file_hashes_count; ++i ) {
                    rprint( "%016llx ", shader_file_hashes[ i ] );
                }
                rprint( "\n\n" );*/
                // Write spirv
                file_write_binary( shader_spirv_path, ( void* )shader_create_info.pCode, sizeof( u32 ) * shader_create_info.codeSize );
            }

            // Reflect shader and write reflection to file
            if ( reflection && name_buffer ) {
                if ( slang_input ) {
                    reflect_slang_shader( layout, reflection, temp_allocator, name_buffer );
                } else {
                    reflect_glsl_shader( shader_create_info, reflection, temp_allocator, name_buffer );
                }

                // Write reflection to file
                save_shader_reflection_binary( *reflection, shader_reflection_path );
            }

            // Only when compiling a shader we can say that it is changed.
            shader_changed = true;
        }
        else {
            rprint( "Error compiling shader %s stage %s\n", shader_name, to_compiler_extension( compilation_stage.type ) );
            __debugbreak();
            return false;
        }
    }
    else {
        // Shader is the same, read cached SpirV
        FileReadResult frr = file_read_binary( shader_spirv_path, temp_allocator );

        spirv_bytecode.data = reinterpret_cast< const u32* >( frr.data );
        spirv_bytecode.size = ( u32 )frr.size / sizeof( u32 );

        load_shader_reflection_binary( *reflection, shader_reflection_path, name_buffer );
    }

    return true;
}

VkShaderModuleCreateInfo ShaderCompiler::compile_shader_glsl( cstring code, u32 code_size, VkShaderStageFlagBits stage,
                                         cstring name, ArenaAllocator* temporary_allocator, Span<cstring> defines ) {

    VkShaderModuleCreateInfo shader_create_info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };

    // Compile from glsl to SpirV.
    // TODO: detect if input is HLSL.
    const char* temp_filename = "temp.shader";

    // Write current shader to file.
    FILE* temp_shader_file = fopen( temp_filename, "w" );
    fwrite( code, code_size, 1, temp_shader_file );
    fclose( temp_shader_file );

    sizet current_marker = temporary_allocator->get_marker();
    StringBuffer temp_string_buffer;
    temp_string_buffer.init( rkilo( 1 ), temporary_allocator );

    // Add uppercase define as STAGE_NAME
    char* stage_define = temp_string_buffer.append_use_f( "%s_%s", to_stage_defines( stage ), name );
    sizet stage_define_length = strlen( stage_define );
    for ( u32 i = 0; i < stage_define_length; ++i ) {
        stage_define[ i ] = toupper( stage_define[ i ] );
    }

    cstring final_spirv_filename = "shader_final.spv";

    /*Directory df;
    directory_current( &df );
    rprint( "Current directory: %s\n", df.path );*/

    // Compile to SpirV

    // Cache compiler path (notice the .exe missing on non-windows platforms)
#if defined(_MSC_VER)
    char* glsl_compiler_path = temp_string_buffer.append_use_f( "%sglslangValidator.exe", vulkan_binaries_path );
#else
    char* glsl_compiler_path = temp_string_buffer.append_use_f( "%sglslangValidator", vulkan_binaries_path );
#endif // _MSC_VER

    // Compose argument string based on headers and options
    char* arguments = temp_string_buffer.current();
    // Add compiler executable, windows only
#if defined(_MSC_VER)
    temp_string_buffer.append_f( "glslangValidator.exe" );
#endif // _MSC_VER

    temp_string_buffer.append_f( " %s -V --target-env vulkan1.3 -o %s -S %s -I../source/shaders/glsl -I../../../source/shaders/glsl --D %s --D %s --D RAPTOR_GLSL", temp_filename, final_spirv_filename, to_compiler_extension( stage ), stage_define, to_stage_defines( stage ) );

    // Append defines
    for ( u32 i = 0; i < defines.size; ++i ) {
        temp_string_buffer.append_f( " --D %s", defines[ i ] );
    }

    // Add debug info
    temp_string_buffer.append( " -g" );

    // Add null terminator
    temp_string_buffer.close_current_string();

    process_execute( ".", glsl_compiler_path, arguments, "" );

    bool optimize_shaders = false;

    if ( optimize_shaders ) {
        // TODO: add optional optimization stage
        //"spirv-opt -O input -o output
        char* spirv_optimizer_path = temp_string_buffer.append_use_f( "%sspirv-opt.exe", vulkan_binaries_path );
        char* optimized_spirv_filename = temp_string_buffer.append_use_f( "shader_opt.spv" );
        char* spirv_opt_arguments = temp_string_buffer.append_use_f( "spirv-opt.exe -O --preserve-bindings %s -o %s", final_spirv_filename, optimized_spirv_filename );

        process_execute( ".", spirv_optimizer_path, spirv_opt_arguments, "" );

        // Read back SPV file.
        shader_create_info.pCode = reinterpret_cast< const u32* >( file_read_binary( optimized_spirv_filename, temporary_allocator, &shader_create_info.codeSize ) );

        file_delete( optimized_spirv_filename );
    } else {
        // Read back SPV file.
        shader_create_info.pCode = reinterpret_cast< const u32* >( file_read_binary( final_spirv_filename, temporary_allocator, &shader_create_info.codeSize ) );
    }

    // Handling compilation error
    if ( shader_create_info.pCode == nullptr ) {
        dump_shader_code( temp_string_buffer, code, stage, name );
    }

    static bool save_spirv_file = false;

    if ( save_spirv_file ) {
        char* spirv_filename = temp_string_buffer.append_use_f( "%s_%s.spv", stage_define, "glsl" );

        file_copy( final_spirv_filename, spirv_filename );
    }

    // Temporary files cleanup
    file_delete( temp_filename );
    file_delete( final_spirv_filename );

    return shader_create_info;
}

VkShaderModuleCreateInfo ShaderCompiler::compile_shader_slang( cstring code, u32 code_size, VkShaderStageFlagBits stage,
                                         cstring name, ArenaAllocator* temporary_allocator, slang::ProgramLayout** layout,
                                         Span<cstring> defines ) {

    VkShaderModuleCreateInfo shader_create_info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };

    // Compile from glsl to SpirV.
    // TODO: detect if input is HLSL.
    const char* temp_filename = "temp.slang";

    // Write current shader to file.
    FILE* temp_shader_file = fopen( temp_filename, "w" );
    fwrite( code, code_size, 1, temp_shader_file );
    fclose( temp_shader_file );

    sizet current_marker = temporary_allocator->get_marker();
    StringBuffer temp_string_buffer;
    temp_string_buffer.init( rkilo( 1 ), temporary_allocator );

    // Add uppercase define as STAGE_NAME
    char* stage_define = temp_string_buffer.append_use_f( "%s_%s", to_stage_defines( stage ), name );
    sizet stage_define_length = strlen( stage_define );
    for ( u32 i = 0; i < stage_define_length; ++i ) {
        stage_define[ i ] = toupper( stage_define[ i ] );
    }

    cstring final_spirv_filename = "shader_final.spv";

    if ( !s_slang_global_session ) {
        slang::createGlobalSession( &s_slang_global_session );
    }

    if ( s_slang_session ) {
        s_slang_session->release();
    }

    cstring shader_profile = to_slang_shader_profiler( stage );

    // Create a compilation session to generate SPIRV from Slang code
    slang::TargetDesc  target_desc = {
        .format = SLANG_SPIRV,
        .profile = s_slang_global_session->findProfile( shader_profile ),
        .flags = SLANG_TARGET_FLAG_GENERATE_SPIRV_DIRECTLY,
    };

    slang::SessionDesc session_desc = {
        .targets = &target_desc,
        .targetCount = 1,
        .defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR,
    };
    // sessionDesc.allowGLSLSyntax    = true;

    // Search paths
    Array<cstring> search_paths;
    search_paths.init( temporary_allocator, 4 );
    search_paths.push( vulkan_binaries_path );
    search_paths.push( "../source/shaders/slang" );
    search_paths.push( "../../../source/shaders/slang" );

    //for ( u32 i = 0; i < includes.size; ++i ) {
    //    search_paths.push_back( includes[ i ].data );
    //}

    session_desc.searchPaths = search_paths.data;
    session_desc.searchPathCount = search_paths.size;

    // Pre-processor
    Array<slang::PreprocessorMacroDesc> preprocessor_defines;
    preprocessor_defines.init( temporary_allocator, 4 );

    preprocessor_defines.push( { "RAPTOR_SLANG", "1"});
    preprocessor_defines.push( { stage_define, "1" } );
    preprocessor_defines.push( { to_stage_defines( stage ), "1" } );

    // Append defines
    for ( u32 i = 0; i < defines.size; ++i ) {
        preprocessor_defines.push( { defines[ i ], "1" } );
    }

    session_desc.preprocessorMacros = preprocessor_defines.data;
    session_desc.preprocessorMacroCount = preprocessor_defines.size;

    s_slang_global_session->createSession( session_desc, &s_slang_session );

    //
    slang::IModule* slangModule = nullptr;
    Slang::ComPtr<slang::IBlob> diagnosticBlob;
    slangModule = s_slang_session->loadModule( temp_filename, diagnosticBlob.writeRef() );
    if ( diagnosticBlob != nullptr ) {
        rprint( "%s\n", ( const char* )diagnosticBlob->getBufferPointer() );
    }

    Slang::ComPtr<slang::IEntryPoint> entryPoint;
    SlangResult result = slangModule->findEntryPointByName( "main", entryPoint.writeRef());
    if ( SLANG_FAILED( result ) ) {

    } else {

        if ( layout != nullptr ) {
            *layout = slangModule->getLayout();
        }

        Array<slang::IComponentType*> componentTypes;
        componentTypes.init( temporary_allocator, 4 );
        componentTypes.push( slangModule );
        componentTypes.push( entryPoint );  // index 0

        Slang::ComPtr<slang::IComponentType> composedProgram;
        Slang::ComPtr<slang::IBlob> diagnosticsBlob;
        SlangResult result = s_slang_session->createCompositeComponentType( componentTypes.data, componentTypes.size,
                                                                            composedProgram.writeRef(), diagnosticsBlob.writeRef() );
        if ( diagnosticBlob != nullptr ) {
            rprint( "%s\n", ( const char* )diagnosticBlob->getBufferPointer() );
        }

        Slang::ComPtr<slang::IBlob> outCode;
        result = composedProgram->getEntryPointCode( 0, 0, outCode.writeRef(), diagnosticsBlob.writeRef() );

        if ( diagnosticBlob != nullptr ) {
            rprint( "%s\n", ( const char* )diagnosticBlob->getBufferPointer() );
        }

        // Print SpirV file
        FILE* temp_spirv_file = fopen( final_spirv_filename, "wb" );
        fwrite( outCode->getBufferPointer(), outCode->getBufferSize(), 1, temp_spirv_file );
        fclose( temp_spirv_file );
    }

    bool optimize_shaders = false;

    if ( optimize_shaders ) {
        // TODO: add optional optimization stage
        //"spirv-opt -O input -o output
        char* spirv_optimizer_path = temp_string_buffer.append_use_f( "%sspirv-opt.exe", vulkan_binaries_path );
        char* optimized_spirv_filename = temp_string_buffer.append_use_f( "shader_opt.spv" );
        char* spirv_opt_arguments = temp_string_buffer.append_use_f( "spirv-opt.exe -O --preserve-bindings %s -o %s", final_spirv_filename, optimized_spirv_filename );

        process_execute( ".", spirv_optimizer_path, spirv_opt_arguments, "" );

        // Read back SPV file.
        shader_create_info.pCode = reinterpret_cast< const u32* >( file_read_binary( optimized_spirv_filename, temporary_allocator, &shader_create_info.codeSize ) );

        file_delete( optimized_spirv_filename );
    } else {
        // Read back SPV file.
        shader_create_info.pCode = reinterpret_cast< const u32* >( file_read_binary( final_spirv_filename, temporary_allocator, &shader_create_info.codeSize ) );
    }

    // Handling compilation error
    if ( shader_create_info.pCode == nullptr ) {
        dump_shader_code( temp_string_buffer, code, stage, name );
    }

    static bool save_spirv_file = false;

    if ( save_spirv_file ) {
        char* spirv_filename = temp_string_buffer.append_use_f( "%s_%s.spv", stage_define, "slang" );

        file_copy( final_spirv_filename, spirv_filename );
    }

    // Temporary files cleanup
    file_delete( temp_filename );
    file_delete( final_spirv_filename );

    return shader_create_info;
}

void ShaderCompiler::dump_shader_code( StringBuffer& temp_string_buffer, cstring code, VkShaderStageFlagBits stage, cstring name ) {
    rprint( "Error in creation of shader %s, stage %s. Writing shader:\n", name, to_stage_defines( stage ) );

    cstring current_code = code;
    u32 line_index = 1;
    while ( current_code ) {

        cstring end_of_line = current_code;
        if ( !end_of_line || *end_of_line == 0 ) {
            break;
        }
        while ( !is_end_of_line( *end_of_line ) ) {
            ++end_of_line;
        }
        if ( *end_of_line == '\r' ) {
            ++end_of_line;
        }
        if ( *end_of_line == '\n' ) {
            ++end_of_line;
        }

        temp_string_buffer.clear();
        char* line = temp_string_buffer.append_use_substring( current_code, 0, u32( end_of_line - current_code ) );
        rprint( "%u: %s", line_index++, line );

        current_code = end_of_line;
    }
}

void ShaderCompiler::parse_layout( VkShaderModuleCreateInfo& shader_create_info, ShaderState* shader_state, const ShaderStage& stage, StringBuffer& name_buffer ) {

    RASSERT( false );
#if 0
    if ( shader_state->slang_layout != nullptr ) {
        u32 par_count = shader_state->slang_layout->getParameterCount();

        for ( u32 p = 0; p < par_count; p++ ) {
            slang::VariableLayoutReflection* var_reflection = shader_state->slang_layout->getParameterByIndex( p );
            slang::TypeLayoutReflection* type_layout = var_reflection->getTypeLayout();

            u32 set_count = type_layout->getDescriptorSetCount();
            cstring name = var_reflection->getName();

            // slang::TypeParameterReflection* type_reflection = shader_state->slang_layout->getTypeParameterByIndex( p );
            // u32 binding = type_reflection->getIndex();

            int used_layout_unit_count = var_reflection->getCategoryCount();
            u32 set_index = 0;
            for (int i = 0; i < used_layout_unit_count; ++i)
            {
                auto layout_unit = var_reflection->getCategoryByIndex( i );
                size_t space_offset = var_reflection->getBindingSpace( layout_unit );

                switch( layout_unit )
                {
                default:
                    break;

                case slang::ParameterCategory::ConstantBuffer:
                case slang::ParameterCategory::ShaderResource:
                case slang::ParameterCategory::UnorderedAccess:
                case slang::ParameterCategory::SamplerState:
                case slang::ParameterCategory::DescriptorTableSlot:
                    set_index = space_offset;
                }
            }

            if ( set_index == 0 ) {
                // NOTE(marco): ignore bindless set
                continue;
            }

            for ( u32 set = 0; set < set_count; ++set ) {
                u32 offset = type_layout->getDescriptorSetSpaceOffset( set );

                i64 range = type_layout->getDescriptorSetDescriptorRangeCount( set + offset );

                u32 binding_index = type_layout->getDescriptorSetDescriptorRangeIndexOffset( set + offset, 0 );

                slang::BindingType binding_type = type_layout->getDescriptorSetDescriptorRangeType( set + offset, 0 );

                DescriptorSetLayoutCreation& setLayout = shader_state->parse_result->sets[ set_index ];
                setLayout.set_set_index( set_index );

                DescriptorSetLayoutCreation::Binding binding{ };
                binding.index = binding_index;
                binding.count = 1;
                binding.name = name_buffer.append_use( name );

                bool valid = false;
                switch ( binding_type )
                {
                    case slang::BindingType::MutableTypedBuffer:
                    case slang::BindingType::MutableRawBuffer:
                        binding.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                        valid = true;
                        break;
                    case slang::BindingType::TypedBuffer:
                    case slang::BindingType::RawBuffer:
                    case slang::BindingType::ConstantBuffer:
                        binding.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                        valid = true;
                        break;
                    case slang::BindingType::CombinedTextureSampler:
                        binding.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                        valid = true;
                        break;
                    case slang::BindingType::MutableTexture:
                        binding.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                        valid = true;
                        break;
                    case slang::BindingType::RayTracingAccelerationStructure:
                        binding.type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
                        valid = true;
                        break;
                    case slang::BindingType::PushConstant:
                        // TODO(marco)
                        // parse_result->push_constants_stride = push_constants_type.width;
                        valid = true;
                        break;
                    case slang::BindingType::InlineUniformData:
                        // TODO(marco)
                        // Cache specialization value
                        // SpecializationConstant& specialization_constant = parse_result->specialization_constants[ parse_result->specialization_constants_count ];
                        // specialization_constant.binding = id_spec_binding.binding;
                        // specialization_constant.byte_stride = id.width / 8;
                        // specialization_constant.default_value = id.value;

                        // Cache specialization name to lookup
                        // SpecializationName& specialization_name = parse_result->specialization_names[ parse_result->specialization_constants_count ];
                        // raptor::StringView::copy_to( id_spec_binding.name, specialization_name.name, 32 );

                        // ++parse_result->specialization_constants_count;
                        valid = true;
                        break;
                    default:
                        break;
                }

                if ( valid ) {
                    add_binding_if_unique( setLayout, binding );

                    shader_state->parse_result->set_count = max( shader_state->parse_result->set_count, set_index + 1 );
                }
            }
        }
    }
    else {
        using json = nlohmann::json;

        std::string json_string( ( char* )stage.layout, stage.layout_size );
        json json_data = json::parse( json_string.c_str() );

        json parameters = json_data[ "parameters" ];

        for ( u32 p = 0; p < parameters.size(); ++p ) {
            DescriptorSetLayoutCreation::Binding binding{ };
            binding.count = 1;

            json parameter = parameters[ p ];

            json binding_entry = parameter[ "binding" ];
            json type = parameter[ "type" ];

            std::string binding_kind;
            binding_entry[ "kind" ].get_to( binding_kind );

            if ( binding_kind == "pushConstantBuffer" ) {
                json fields = type[ "elementType" ][ "fields" ];

                u32 width = 0;
                for ( u32 f = 0; f < fields.size(); f++ ) {
                    json field = fields[ f ];

                    // TODO(marco): use offset to account for padding
                    u32 field_size = 0;
                    field[ "binding" ][ "size" ].get_to( field_size );

                    width += field_size;
                }

                shader_state->parse_result->push_constants_stride = width;

                continue;
            }

            u32 set_index = 0;
            if ( binding_entry.contains( "space" ) ) {
                binding_entry[ "space" ].get_to( set_index );
            }

            if ( set_index == 0 ) {
                // NOTE(marco): ignore bindless set
                continue;
            }

            std::string name;
            parameter[ "name" ].get_to( name );
            binding.name = name_buffer.append_use( name.c_str() );

            DescriptorSetLayoutCreation& setLayout = shader_state->parse_result->sets[ set_index ];
            setLayout.set_set_index( set_index );

            u32 slot = 0;
            binding_entry[ "index" ].get_to( binding.index );

            std::string resource_kind;
            type[ "kind" ].get_to( resource_kind );

            bool valid = false;
            if ( resource_kind == "constantBuffer" ) {
                binding.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                valid = true;
            }
            else {
                std::string resource_type;
                type[ "baseShape" ].get_to( resource_type );

                if ( resource_type == "structuredBuffer" ) {
                    binding.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    valid = true;
                }
                else if ( resource_type == "accelerationStructure" ) {
                    binding.type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
                    valid = true;
                }
                else if ( resource_type == "byteAddressBuffer" ) {
                    binding.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    valid = true;
                }
                else {
                    RASSERT( false );
                }
            }

            if ( valid ) {
                add_binding_if_unique( setLayout, binding );

                shader_state->parse_result->set_count = max( shader_state->parse_result->set_count, set_index + 1 );
            }
        }
    }
#endif
}

} // namespace raptor
