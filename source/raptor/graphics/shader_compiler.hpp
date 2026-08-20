#pragma once

#include "external/volk.h"

#include "foundation/span.hpp"
#include "foundation/hash_map.hpp"

#include <slang/slang.h>

namespace raptor
{

struct Allocator;
struct PipelineCreation;
struct ShaderCompilationStage;
struct ShaderReflection;
struct ShaderStage;
struct ShaderState;
struct ArenaAllocator;
struct StringBuffer;

struct ShaderCompiler
{
    static                  void init( Allocator* allocator );
    static                  void shutdown();

    static                  void calculate_shader_hash( ShaderCompilationStage& shader_stage );
    static                  void read_source_from_file_and_add_hashes( ShaderCompilationStage& shader_stage, StringBuffer& shader_buffer, ArenaAllocator* temp_allocator );
    static                  bool compile_and_cache_shader( ShaderCompilationStage& compilation_stage, Span<const u32>& spirv_bytecode, cstring binary_data_folder, ArenaAllocator* temp_allocator,
                                                           cstring technique_name, cstring shader_name, ShaderReflection* reflection, StringBuffer* name_buffer,
                                                           bool use_cache, bool slang_input, bool ignore_layout, bool& shader_changed );

    static                  VkShaderModuleCreateInfo compile_shader_glsl( cstring code, u32 code_size, VkShaderStageFlagBits stage, cstring name, ArenaAllocator* temp_allocator, Span<cstring> defines );
    static                  VkShaderModuleCreateInfo compile_shader_slang( cstring code, u32 code_size, VkShaderStageFlagBits stage, cstring name, ArenaAllocator* temp_allocator, slang::ProgramLayout** layout, Span<cstring> defines );

    static                  void parse_layout( VkShaderModuleCreateInfo& shader_create_info, ShaderState* shader_state, const ShaderStage& stage, StringBuffer& name_buffer );

    static                  void dump_shader_code( StringBuffer& temp_string_buffer, cstring code, VkShaderStageFlagBits stage, cstring name );

    static char             vulkan_binaries_path[ 512 ];
    static StringBuffer     string_buffer;
    static StringBuffer     path_buffer;

    static FlatHashMap<u64, u64> shader_file_hash_cache;
};

} // namespace raptor
