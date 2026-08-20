
#include "graphics/renderer.hpp"
#include "graphics/render_scene.hpp"

#include "graphics/command_buffer.hpp"
#include "graphics/shader_compiler.hpp"
#include "graphics/shader_reflection.hpp"
#include "graphics/frame_renderer.hpp"
#include "graphics/frame_graph.hpp"
#include "graphics/asynchronous_loader.hpp"

#include "foundation/memory.hpp"
#include "foundation/file.hpp"

#include "external/imgui/imgui.h"
#include "external/vk_mem_alloc.h"
#include "external/stb_image.h"

#include <mutex>

namespace raptor {

std::mutex                  texture_update_mutex;

// Renderer /////////////////////////////////////////////////////////////////////

u64 TextureResource::k_type_hash = 0;
u64 BufferResource::k_type_hash = 0;
u64 SamplerResource::k_type_hash = 0;

static Renderer s_renderer;

Renderer* Renderer::instance() {
    return &s_renderer;
}

void Renderer::init( const RendererCreation& creation ) {

    rprint( "Renderer init\n" );

    gpu = creation.gpu;
    resident_allocator = creation.allocator;
    temporary_allocator.init( rmega( 5 ) );
    names_buffer.init( rmega( 1 ), resident_allocator );

    async_loader = rnew( AsynchronousLoader, resident_allocator );
    async_loader->init( this, resident_allocator );

    const RendererResourcePoolCreation& pool_creation = creation.resource_pool_creation;
    textures.init( creation.allocator, pool_creation.textures );
    buffers.init( creation.allocator, pool_creation.buffers );
    samplers.init( creation.allocator, pool_creation.samplers );
    shader_reflections.init( creation.allocator, pool_creation.shader_reflections );

    resource_cache.init( creation.allocator );

    // Init resource hashes
    TextureResource::k_type_hash = hash_calculate( TextureResource::k_type );
    BufferResource::k_type_hash = hash_calculate( BufferResource::k_type );
    SamplerResource::k_type_hash = hash_calculate( SamplerResource::k_type );

    const u32 gpu_heap_counts = gpu->get_memory_heap_count();
    gpu_heap_budgets.init( resident_allocator, gpu_heap_counts, gpu_heap_counts );

    ArenaScope scoped_allocator( &temporary_allocator );
    StringBuffer temporary_name_buffer;
    temporary_name_buffer.init( 1024, scoped_allocator.allocator );

    // Create binaries folders
    cstring shader_binaries_folder = temporary_name_buffer.append_use_f( "%s/shaders/", RAPTOR_DATA_FOLDER );
    if ( !directory_exists( shader_binaries_folder ) ) {
        if ( directory_create( shader_binaries_folder ) ) {
            rprint( "Created folder %s\n", shader_binaries_folder );
        } else {
            rprint( "Cannot create folder %s\n" );
        }
    }
    strcpy( resource_cache.binary_data_folder, shader_binaries_folder );
}

void Renderer::shutdown() {

    temporary_allocator.shutdown();
    names_buffer.shutdown();

    resource_cache.shutdown( this );
    gpu_heap_budgets.shutdown();

    textures.shutdown();
    buffers.shutdown();
    samplers.shutdown();
    shader_reflections.shutdown();

    async_loader->shutdown();
    rdelete( async_loader, AsynchronousLoader, resident_allocator );

    rprint( "Renderer shutdown\n" );

    gpu->shutdown();
}

void Renderer::set_loaders( raptor::ResourceManager* manager ) {

}

static void pool_imgui_draw( const ResourcePool& resource_pool, cstring resource_name ) {
    ImGui::Text( "Pool %s, indices used %u, allocated %u", resource_name, resource_pool.used_indices, resource_pool.pool_size );
}

void Renderer::imgui_draw() {

    ImGui::Text( "GPU used: %s", gpu->get_gpu_name() );
    // Print memory stats
    vmaGetHeapBudgets( gpu->vma_allocator, gpu_heap_budgets.data );

    sizet memory_used = 0;
    sizet memory_allocated = 0;
    for ( u32 i = 0; i < gpu->get_memory_heap_count(); ++i ) {
        memory_used += gpu_heap_budgets[ i ].usage;
        memory_allocated += gpu_heap_budgets[ i ].budget;
    }

    ImGui::Text( "GPU Memory Used: %lluMB, Total: %lluMB", memory_used / ( 1024 * 1024 ), memory_allocated / ( 1024 * 1024 ) );

    // Resorce pools
    ImGui::Separator();
    //pool_imgui_draw( gpu->buffers, "Buffers" );
    // TODO(gabriel)
    //pool_imgui_draw( gpu->images, "Textures" );
    //pool_imgui_draw( gpu->pipelines, "Pipelines" );
    //pool_imgui_draw( gpu->samplers, "Samplers" );
    //pool_imgui_draw( gpu->descriptor_sets, "DescriptorSets" );
    //pool_imgui_draw( gpu->descriptor_set_layouts, "DescriptorSetLayouts" );
    //pool_imgui_draw( gpu->shaders, "Shaders" );
}

void Renderer::set_presentation_mode( PresentMode::Enum value ) {
    gpu->set_present_mode( value );
    gpu->resize_swapchain();
}

f32 Renderer::aspect_ratio() const {
    return gpu->swapchain_width * 1.f / gpu->swapchain_height;
}
//
//BufferResource* Renderer::create_buffer( const BufferCreation& creation ) {
//
//    BufferResource* buffer = buffers.obtain();
//    if ( buffer ) {
//        BufferHandle handle = gpu->create_buffer( creation );
//        buffer->handle = handle;
//        buffer->name = creation.name;
//
//        if ( creation.name != nullptr ) {
//            resource_cache.buffers.insert( hash_calculate( creation.name ), buffer );
//        }
//
//        buffer->references = 1;
//
//        return buffer;
//    }
//    return nullptr;
//}
//
//BufferResource* Renderer::create_buffer( VkBufferUsageFlags type, ResourceUsageType::Enum usage, u32 size, void* data, cstring name, bool bindless ) {
//    BufferCreation creation{ type, usage, size, 0, 0, data, name };
//    BufferResource* buffer = create_buffer( creation );
//
//    if ( bindless ) {
//        gpu->add_buffer_to_bindless( buffer->handle );
//    }
//
//    return buffer;
//}

TextureResource* Renderer::create_texture( const ImageCreation& creation ) {
    TextureResource* texture = textures.obtain();

    if ( texture ) {
        ImageHandle image_handle = gpu->create_image( creation );
        texture->image = image_handle;
        texture->name = creation.name;

        texture->image_view = gpu->create_image_view( {
            .parent_image = image_handle, .view_type = VK_IMAGE_VIEW_TYPE_2D,
            .sub_resource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, creation.mip_level_count, 0, 1 }, .name = creation.name } );

        gpu->add_image_view_to_bindless( texture->image_view );

        if ( creation.name != nullptr ) {
            resource_cache.textures.insert( hash_calculate( creation.name ), texture );
        }

        texture->references = 1;

        return texture;
    }
    return nullptr;
}

TextureResource* Renderer::create_texture_from_file( cstring full_filename, bool generate_mips, bool srgb ) {

    int comp, width, height;

    cstring path = names_buffer.append_use_f( "%s", full_filename );

    stbi_info( path, &width, &height, &comp );

    u32 mip_levels = 1;
    if ( generate_mips ) {
        u32 w = width;
        u32 h = height;

        while ( w > 1 && h > 1 ) {
            w /= 2;
            h /= 2;

            ++mip_levels;
        }
    }


    // Create texture
    ImageCreation tc;
    tc.set_data( nullptr ).set_format_type( VK_FORMAT_R8G8B8A8_UNORM, TextureType::Texture2D ).set_flags( 0 )
      .set_size( ( u16 )width, ( u16 )height, 1 ).set_name( path ).set_mips( mip_levels );
    TextureResource* tr = create_texture( tc );
    // Request async loading
    async_loader->request_texture_load_from_file( path, tr->image );

    return tr;
}

SamplerResource* Renderer::create_sampler( const SamplerCreation& creation ) {
    SamplerResource* sampler = samplers.obtain();
    if ( sampler ) {
        SamplerHandle handle = gpu->create_sampler( creation );
        sampler->handle = handle;
        sampler->name = creation.name;

        if ( creation.name != nullptr ) {
            resource_cache.samplers.insert( hash_calculate( creation.name ), sampler );
        }

        sampler->references = 1;

        return sampler;
    }
    return nullptr;
}

bool Renderer::create_graphics_pipeline_state( const ShaderCompilationCreation& shader_creation,
                                               const PipelineCreation& pipeline_creation, cstring name,
                                               FrameGraph* frame_graph,
                                               GraphicsPipelineState& out_pipeline_state ) {

    ShaderReflection shader_reflection;

    out_pipeline_state.shader = create_shader_state( shader_creation, name, &shader_reflection );
    if ( out_pipeline_state.shader.is_invalid() ) {
        rprint( "Error creating shader state %s\n", name );
        return false;
    }

    out_pipeline_state.layout = create_pipeline_layout( shader_reflection );
    if ( out_pipeline_state.layout.is_invalid() ) {
        rprint( "Error creating pipeline layout %s\n", name );
        return false;
    }

    PipelineCreation pipeline_creation_write = pipeline_creation;
    pipeline_creation_write.shader = out_pipeline_state.shader;
    pipeline_creation_write.layout = out_pipeline_state.layout;

    frame_graph->cache_render_pass_output( pipeline_creation_write.render_pass_name, gpu, pipeline_creation_write.render_pass_output, false );
    out_pipeline_state.pipeline = create_pipeline( shader_reflection, pipeline_creation_write );

    if ( out_pipeline_state.pipeline.is_invalid() ) {
        rprint( "Error creating pipeline %s\n", name );
        return false;
    }

    return true;
}

bool Renderer::create_compute_pipeline_state( const ShaderCompilationCreation& shader_creation,
                                              const PipelineCreation& pipeline_creation, cstring name,
                                              FrameGraph* frame_graph, ComputePipelineState& out_pipeline_state ) {
    ShaderReflection shader_reflection;

    out_pipeline_state.shader = create_shader_state( shader_creation, name, &shader_reflection );
    if ( out_pipeline_state.shader.is_invalid() ) {
        rprint( "Error creating shader state %s\n", name );
        return false;
    }

    out_pipeline_state.layout = create_pipeline_layout( shader_reflection );
    if ( out_pipeline_state.layout.is_invalid() ) {
        rprint( "Error creating pipeline layout %s\n", name );
        return false;
    }

    PipelineCreation pipeline_creation_write = pipeline_creation;
    pipeline_creation_write.shader = out_pipeline_state.shader;
    pipeline_creation_write.layout = out_pipeline_state.layout;

    frame_graph->cache_render_pass_output( pipeline_creation_write.render_pass_name, gpu, pipeline_creation_write.render_pass_output, true );
    out_pipeline_state.pipeline = create_pipeline( shader_reflection, pipeline_creation_write );

    if ( out_pipeline_state.pipeline.is_invalid() ) {
        rprint( "Error creating pipeline %s\n", name );
        return false;
    }

    return true;
}

bool Renderer::create_raytracing_pipeline_state( const ShaderCompilationCreation& shader_creation, const PipelineCreation& pipeline_creation, cstring name, FrameGraph* frame_graph, RayTracingPipelineState& out_pipeline_state ) {
    
    ShaderReflection shader_reflection;

    out_pipeline_state.shader = create_shader_state( shader_creation, name, &shader_reflection );
    if ( out_pipeline_state.shader.is_invalid() ) {
        rprint( "Error creating shader state %s\n", name );
        return false;
    }

    out_pipeline_state.layout = create_pipeline_layout( shader_reflection );
    if ( out_pipeline_state.layout.is_invalid() ) {
        rprint( "Error creating pipeline layout %s\n", name );
        return false;
    }

    PipelineCreation pipeline_creation_write = pipeline_creation;
    pipeline_creation_write.shader = out_pipeline_state.shader;
    pipeline_creation_write.layout = out_pipeline_state.layout;

    frame_graph->cache_render_pass_output( pipeline_creation_write.render_pass_name, gpu, pipeline_creation_write.render_pass_output, true );
    out_pipeline_state.pipeline = create_pipeline( shader_reflection, pipeline_creation_write );

    if ( out_pipeline_state.pipeline.is_invalid() ) {
        rprint( "Error creating pipeline %s\n", name );
        return false;
    }

    return true;
}

ShaderStateHandle Renderer::create_shader_state( const ShaderCompilationCreation& creation,
                                                 cstring technique_name, ShaderReflection* out_reflection ) {

    ArenaScope marker( &temporary_allocator );

    StringBuffer shader_code_buffer;
    shader_code_buffer.init( rmega( 3 ), &temporary_allocator );

    bool use_cache = true;

    //ShaderCompilationCreation& shader_compilation = const_cast<ShaderCompilationCreation&>( creation );

    ShaderCompilationCreation shader_compilation = ( creation );
    bool valid = true;
    bool compute_shader_pass = false;

    ShaderStateCreation ssc;
    ssc.names_buffer = &names_buffer;

    // Reset reflection output
    if ( out_reflection ) {
        *out_reflection = {};
    }

    // Parse and accumulate all bindings from all stages
    for ( u32 s = 0; s < shader_compilation.stages.size; ++s ) {
        ShaderCompilationStage& shader_compilation_stage = shader_compilation.stages[ s ];

        if ( shader_compilation_stage.type == VK_SHADER_STAGE_COMPUTE_BIT ) {
            compute_shader_pass = true;
        }

        shader_code_buffer.clear();
        shader_code_buffer.data[ 0 ] = 0;

        if ( shader_compilation_stage.source_code.size == 0 ) {
            ShaderCompiler::read_source_from_file_and_add_hashes( shader_compilation_stage, shader_code_buffer, &temporary_allocator );
        }
        else if ( shader_compilation_stage.shader_file_hashes.size == 0 ) {
            ShaderCompiler::calculate_shader_hash( shader_compilation_stage );
        }

        VkShaderModuleCreateInfo shader_stage = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        Span<const u32> spirv_bytecode;
        bool shader_changed = false; // TODO(marco): ignored for now
        bool result = ShaderCompiler::compile_and_cache_shader( shader_compilation_stage, spirv_bytecode, resource_cache.binary_data_folder,
                                                                &temporary_allocator, technique_name,
                                                                shader_compilation.name.data, out_reflection, ssc.names_buffer,
                                                                use_cache, shader_compilation.slang_input, false, shader_changed );
        if ( !result ) {
            valid = false;
            break;
        }

        shader_stage.pCode = spirv_bytecode.data;
        shader_stage.codeSize = spirv_bytecode.size;

        ssc.add_stage( shader_stage, shader_compilation_stage.type );
    }

    if ( valid ) {

        ShaderStateHandle shader_state = gpu->create_shader_state( ssc );

        ShaderReflectionInfo* shader_reflection_info = resource_cache.shader_reflections.get( shader_state.id );

        if ( shader_reflection_info == nullptr ) {

            shader_reflection_info = shader_reflections.obtain();
            shader_reflection_info->name_hash_to_index.init( resident_allocator, 64 );
            shader_reflection_info->name_hash_to_index.set_default_value( u16_max );
            shader_reflection_info->name = names_buffer.append_use( shader_compilation.name.data );

            resource_cache.shader_reflections.insert( shader_state.id, shader_reflection_info );
        }

        out_reflection->name = creation.name.data;

        // Prepare reflection output
        // Sort bindings by index
        for ( u32 i = 0; i < out_reflection->sets.size; ++i ) {

            DescriptorSetReflection& layout_info = out_reflection->sets[ i ];

            auto sorting_func = []( const void* a, const void* b ) -> i32 {
                const DescriptorBinding2* b0 = (const DescriptorBinding2*)a;
                const DescriptorBinding2* b1 = (const DescriptorBinding2*)b;

                if ( b0->index > b1->index ) {
                    return 1;
                }

                if ( b0->index < b1->index ) {
                    return -1;
                }

                return 0;
                };

            qsort( layout_info.bindings.data, layout_info.bindings.size, sizeof( DescriptorBinding2 ), sorting_func );

            for ( u32 b = 0; b < layout_info.bindings.size; ++b ) {
                const DescriptorBinding2& binding = layout_info.bindings[ b ];
                RASSERT( binding.name );
                shader_reflection_info->name_hash_to_index.insert( hash_calculate( binding.name ), binding.index );
            }
        }

        return shader_state;
    }

    return ShaderStateHandle();
}

PipelineLayoutHandle Renderer::create_pipeline_layout( const ShaderReflection& reflection ) {

    Array<DescriptorSetLayoutHandle> layout_handles;
    layout_handles.init( &temporary_allocator, 4 );

    if ( reflection.add_global_set ) {
        layout_handles.push( gpu->bindless_descriptor_set_layout );
    }

    // Create descriptor set layouts
    for ( u32 i = 0; i < reflection.sets.size; ++i ) {

        const DescriptorSetReflection& layout_info = reflection.sets[ i ];

        //pass_creation.reflection_sets.push( layout_info );

        Array<VkDescriptorSetLayoutBinding> bindings;
        bindings.init( &temporary_allocator, layout_info.bindings.size );

        for ( u32 b = 0; b < layout_info.bindings.size; ++b ) {
            VkDescriptorSetLayoutBinding& binding = bindings.push_use();
            binding = {};

            const DescriptorBinding2& info_binding = layout_info.bindings[ b ];
            binding.binding = info_binding.index;
            binding.descriptorType = info_binding.type;
            binding.descriptorCount = info_binding.count;
            binding.stageFlags = VK_SHADER_STAGE_ALL;
            binding.pImmutableSamplers = nullptr;
        }

        DescriptorSetLayoutHandle dsl = gpu->create_descriptor_set_layout( {
            .bindings = { bindings.data, bindings.size },
            .set_index = layout_info.set_index, .bindless = false, .dynamic = false,
            .name = reflection.name } );
        layout_handles.push( dsl );
    }

    // Create pipeline layout
    PipelineLayoutCreation pipeline_layout_creation = {
        .layouts = {layout_handles.data, layout_handles.size},
        .name = reflection.name };

    // Cache push constant
    if ( reflection.push_constants_stride ) {

        pipeline_layout_creation.push_constant.offset = 0;
        pipeline_layout_creation.push_constant.size = reflection.push_constants_stride;
        pipeline_layout_creation.push_constant.stageFlags = VK_SHADER_STAGE_ALL;
    } else {
        pipeline_layout_creation.push_constant.size = 0;
    }

    PipelineLayoutHandle pipeline_layout = gpu->create_pipeline_layout( pipeline_layout_creation );
    return pipeline_layout;
}

PipelineHandle Renderer::create_pipeline( const ShaderReflection& reflection, PipelineCreation& creation ) {

    RASSERT( creation.layout.is_valid() );
    RASSERT( creation.shader.is_valid() );

    creation.num_specialization_constants = 0;
    for ( u32 i = 0; i < reflection.specialization_constants.size; ++i ) {

        const SpecializationConstant& specialization_constant = reflection.specialization_constants[ i ];
        cstring specialization_name = reflection.specialization_names[ i ].c_str();

        if ( strcmp( specialization_name, "SUBGROUP_SIZE" ) == 0 ) {

            VkSpecializationMapEntry& specialization_entry = creation.specialization_entries[ creation.num_specialization_constants++ ];
            specialization_entry.constantID = specialization_constant.binding;
            specialization_entry.size = sizeof( u32 );
            specialization_entry.offset = i * sizeof( u32 );

            creation.specialization_data[ i ] = gpu->subgroup_size;
        }
    }

    PipelineHandle pipeline = gpu->create_pipeline( creation );

    if ( creation.name != nullptr ) {
        resource_cache.pipelines.insert( hash_calculate( creation.name ), pipeline );
    }

    return pipeline;
}

DescriptorSetHandle Renderer::create_descriptor_set( DescriptorSetBinder& descriptors, ShaderReflectionInfo* reflection_info,
                                                     PipelineHandle pipeline, u32 frame_index,
                                                     RenderBlackboard& render_blackboard ) {
    RASSERT( reflection_info );
    DescriptorSetLayoutHandle layout_handle = gpu->get_descriptor_set_layout( pipeline, k_material_descriptor_set_index );

    // Search for render scene bindings
    u16 binding = get_binding_index( reflection_info, "FrameConstants" );
    if ( binding != u16_max ) {
        descriptors.bind_dynamic_buffer( binding, sizeof( GpuFrameData ) );
    } else {
        binding = get_binding_index( reflection_info, "frame" );
        if ( binding != u16_max ) {
            descriptors.bind_dynamic_buffer( binding, sizeof( GpuFrameData ) );
        }
    }

    // Search for mesh bindings
    binding = get_binding_index( reflection_info, "MeshDraws" );
    if ( binding != u16_max ) {
        descriptors.bind_ssbo( render_blackboard.meshes_sb, binding );
    }

    binding = get_binding_index( reflection_info, "mesh_draws" );
    if ( binding != u16_max ) {
        descriptors.bind_ssbo( render_blackboard.meshes_sb, binding );
    }

    binding = get_binding_index( reflection_info, "MeshInstanceDraws" );
    if ( binding != u16_max ) {
        descriptors.bind_ssbo( render_blackboard.mesh_instances_sb, binding );
    }

    binding = get_binding_index( reflection_info, "mesh_instance_draws" );
    if ( binding != u16_max ) {
        descriptors.bind_ssbo( render_blackboard.mesh_instances_sb, binding );
    }

    binding = get_binding_index( reflection_info, "MeshBounds" );
    if ( binding != u16_max ) {
        descriptors.bind_ssbo( render_blackboard.mesh_bounds_sb, binding );
    }

    binding = get_binding_index( reflection_info, "mesh_bounds" );
    if ( binding != u16_max ) {
        descriptors.bind_ssbo( render_blackboard.mesh_bounds_sb, binding );
    }

    binding = get_binding_index( reflection_info, "mesh_aabb_sb" );
    if ( binding != u16_max ) {
        descriptors.bind_ssbo( render_blackboard.mesh_aabbs_sb, binding );
    }

    // Meshlet bindings
    MeshletsRuntimeData& meshlets = render_blackboard.meshlets;
    binding = get_binding_index( reflection_info, "Meshlets" );
    if ( binding != u16_max ) {
        descriptors.bind_ssbo( meshlets.meshlets_sb_gpu[ frame_index ], binding );
    }

    binding = get_binding_index( reflection_info, "meshlets" );
    if ( binding != u16_max ) {
        descriptors.bind_ssbo( meshlets.meshlets_sb_gpu[ frame_index ], binding );
    }

    binding = get_binding_index( reflection_info, "MeshletData" );
    if ( binding != u16_max ) {
        descriptors.bind_ssbo( meshlets.meshlets_connectivity_data_sb_gpu, binding );
    }

    binding = get_binding_index( reflection_info, "meshletData" );
    if ( binding != u16_max ) {
        descriptors.bind_ssbo( meshlets.meshlets_connectivity_data_sb_gpu, binding );
    }

    binding = get_binding_index( reflection_info, "MeshletPositionOnlyData" );
    if ( binding != u16_max ) {
        descriptors.bind_ssbo( meshlets.meshlets_position_only_data_sb_gpu, binding );
    }

    binding = get_binding_index( reflection_info, "meshletPositionOnlyData" );
    if ( binding != u16_max ) {
        descriptors.bind_ssbo( meshlets.meshlets_position_only_data_sb_gpu, binding );
    }

    binding = get_binding_index( reflection_info, "VertexPositions" );
    if ( binding != u16_max ) {
        descriptors.bind_ssbo( meshlets.meshlets_vertex_pos_sb_gpu[ frame_index ], binding );
    }

    binding = get_binding_index( reflection_info, "vertex_positions" );
    if ( binding != u16_max ) {
        descriptors.bind_ssbo( meshlets.meshlets_vertex_pos_sb_gpu[ frame_index ], binding );
    }

    binding = get_binding_index( reflection_info, "VertexData" );
    if ( binding != u16_max ) {
        descriptors.bind_ssbo( meshlets.meshlets_vertex_data_sb_gpu[ frame_index ], binding );
    }

    binding = get_binding_index( reflection_info, "vertex_data" );
    if ( binding != u16_max ) {
        descriptors.bind_ssbo( meshlets.meshlets_vertex_data_sb_gpu[ frame_index ], binding );
    }

    // Debug bindings
    DebugDrawRuntimeData& debug = render_blackboard.debug_draw;

    binding = get_binding_index( reflection_info, "debug_lines_sb" );
    if ( binding != u16_max ) {
        descriptors.bind_ssbo( debug.gpu_line_sb, binding );
    }

    binding = get_binding_index( reflection_info, "debug_line_counts_sb" );
    if ( binding != u16_max ) {
        descriptors.bind_ssbo( debug.gpu_line_count_sb, binding );
    }

    binding = get_binding_index( reflection_info, "debug_lines_commands_sb" );
    if ( binding != u16_max ) {
        descriptors.bind_ssbo( debug.gpu_line_commands_sb, binding );
    }

    // Lighting descriptors
    LightingRuntimeData& lighting = render_blackboard.lighting;
    binding = get_binding_index( reflection_info, "ZBins" );
    //RASSERT( binding == u16_max || binding == k_z_bins );
    if ( binding != u16_max ) {
        descriptors.bind_ssbo( lighting.light_z_bins_sb[ frame_index ], binding );
    }

    binding = get_binding_index( reflection_info, "z_bins" );
    //RASSERT( binding == u16_max || binding == k_z_bins );
    if ( binding != u16_max ) {
        descriptors.bind_ssbo( lighting.light_z_bins_sb[ frame_index ], binding );
    }
    /*if ( has_binding( set_layout, k_z_bins ) ) {
        descriptors.bind_ssbo( render_blackboard.light_z_bins_sb[ frame_index ], k_z_bins );
    }*/

    binding = get_binding_index( reflection_info, "Lights" );
    //RASSERT( binding == u16_max || binding == k_lights );
    if ( binding != u16_max ) {
        descriptors.bind_ssbo( lighting.lights_list_sb[ frame_index ], binding );
    }

    binding = get_binding_index( reflection_info, "lights" );
    //RASSERT( binding == u16_max || binding == k_lights );
    if ( binding != u16_max ) {
        descriptors.bind_ssbo( lighting.lights_list_sb[ frame_index ], binding );
    }
    /*if ( has_binding( set_layout, k_lights ) ) {
        descriptors.bind_ssbo( render_blackboard.lights_list_sb, k_lights );
    }*/

    binding = get_binding_index( reflection_info, "Tiles" );
    //RASSERT( binding == u16_max || binding == k_tiles );
    if ( binding != u16_max ) {
        descriptors.bind_ssbo( lighting.lights_tiles_sb[ frame_index ], binding );
    }

    binding = get_binding_index( reflection_info, "tiles" );
    //RASSERT( binding == u16_max || binding == k_tiles );
    if ( binding != u16_max ) {
        descriptors.bind_ssbo( lighting.lights_tiles_sb[ frame_index ], binding );
    }
    /*if ( has_binding( set_layout, k_tiles ) ) {
        descriptors.bind_ssbo( render_blackboard.lights_tiles_sb[ frame_index ], k_tiles );
    }*/

    binding = get_binding_index( reflection_info, "LightIndices" );
    //RASSERT( binding == u16_max || binding == k_light_indices );
    if ( binding != u16_max ) {
        descriptors.bind_ssbo( lighting.lights_indices_sb[ frame_index ], binding );
    }

    binding = get_binding_index( reflection_info, "light_indices" );
    //RASSERT( binding == u16_max || binding == k_light_indices );
    if ( binding != u16_max ) {
        descriptors.bind_ssbo( lighting.lights_indices_sb[ frame_index ], binding );
    }
    /*if ( has_binding( set_layout, k_light_indices ) ) {
        descriptors.bind_ssbo( render_blackboard.lights_indices_sb[ frame_index ], k_light_indices );
    }*/

    binding = get_binding_index( reflection_info, "LightConstants" );
    //RASSERT( binding == u16_max || binding == k_light_constants );
    if ( binding != u16_max ) {
        descriptors.bind_dynamic_buffer( binding, sizeof( GpuLightingData ) );
    }

    binding = get_binding_index( reflection_info, "light_consts" );
    //RASSERT( binding == u16_max || binding == k_light_constants );
    if ( binding != u16_max ) {
        descriptors.bind_dynamic_buffer( binding, sizeof( GpuLightingData ) );
    }
    /*if ( has_binding( set_layout, k_light_constants ) ) {
        descriptors.bind_dynamic_buffer( k_light_constants, sizeof( GpuLightingData ) );
    }*/

    binding = get_binding_index( reflection_info, "as" );
    //RASSERT( binding == u16_max || binding == k_as );
    if ( binding != u16_max ) {
        descriptors.tlas.tlas = render_blackboard.tlas;
        descriptors.tlas.binding = binding;
    }
    /*if ( has_binding( set_layout, k_as ) ) {
        descriptors.tlas.tlas = render_blackboard.tlas;
        descriptors.tlas.binding = k_as;
    }*/

    return gpu->create_descriptor_set( {
        .textures = { descriptors.textures.data, descriptors.textures.size },
        .images = { descriptors.images.data, descriptors.images.size },
        .buffers = { descriptors.buffers.data, descriptors.buffers.size },
        .ssbos = { descriptors.ssbos.data, descriptors.ssbos.size},
        .dynamic_buffers = { descriptors.dynamic_buffers.data, descriptors.dynamic_buffers.size },
        .tlas = descriptors.tlas,
        .layout = layout_handle,
        .name = descriptors.name } );
}
BufferHandle Renderer::create_buffer_with_upload( const BufferCreation& creation, const BufferUpload& upload ) {

    RASSERT( upload.data );
    RASSERT( creation.size > 0 );

    BufferUploadPolicy resolved_policy = upload.policy;

    // TODO: Check if the GPU supports direct upload, otherwise fallback to staging

    BufferCreation actual_creation = creation;

    switch ( resolved_policy ) {
        case BufferUploadPolicy::HostWrite:
        {
            //RASSERTM( gpu->supports_rebar, "Direct buffer upload requires host-visible device-local memory like REBar." );
            actual_creation.memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            actual_creation.allocation_flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

            BufferHandle buffer = gpu->create_buffer( actual_creation );

            void* mapped_data = gpu->map_buffer( { buffer, 0, ( u32 )creation.size } );

            memcpy( mapped_data, upload.data, creation.size );

            gpu->flush_buffer( buffer, 0, ( u32 )creation.size );
            gpu->unmap_buffer( { buffer, 0, ( u32 )creation.size } );

            return buffer;
        }

        case BufferUploadPolicy::Transfer:
        {
            actual_creation.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;

            BufferHandle buffer = gpu->create_buffer( actual_creation );
            async_loader->request_buffer_upload( buffer, upload.data, creation.size, 0 );

            return buffer;
        }

        default:
        {
            RASSERTM( false, "Invalid BufferUploadPolicy." );
            return {};
        }
    }
}

ShaderReflectionInfo* Renderer::get_shader_reflection( PipelineHandle pipeline_handle ) {
    Pipeline* pipeline = gpu->get_pipeline( pipeline_handle );
    RASSERT( pipeline );

    ShaderReflectionInfo* reflection_info = resource_cache.shader_reflections.get( pipeline->shader_state.id );
    RASSERT( reflection_info );
    return reflection_info;
}

u16 Renderer::get_binding_index( ShaderReflectionInfo* reflection_info, cstring name ) {
    u64 name_hash = hash_calculate( name );
    return reflection_info->name_hash_to_index.get( name_hash );
}

u16 Renderer::get_binding_index_from_hash( ShaderReflectionInfo* reflection_info, u64 name_hash ) {
    return reflection_info->name_hash_to_index.get( name_hash );
}

void Renderer::destroy_buffer( BufferResource* buffer ) {
    if ( !buffer ) {
        return;
    }

    buffer->remove_reference();
    if ( buffer->references ) {
        return;
    }

    Buffer* vk_buffer = gpu->get_buffer( buffer->handle );

    if ( vk_buffer->name ) {
        resource_cache.buffers.remove( hash_calculate( vk_buffer->name ) );
    }

    gpu->destroy_buffer( buffer->handle );
    buffers.release( buffer );
}

void Renderer::destroy_texture( TextureResource* texture ) {
    if ( !texture ) {
        return;
    }

    texture->remove_reference();
    if ( texture->references ) {
        return;
    }

    Image* vk_image = gpu->get_image( texture->image );
    if ( vk_image->name ) {
        resource_cache.textures.remove( hash_calculate( vk_image->name ) );
    }

    gpu->destroy_image( texture->image );
    gpu->destroy_image_view( texture->image_view );
    gpu->remove_image_view_from_bindless( texture->image_view );
    textures.release( texture );
}

void Renderer::destroy_sampler( SamplerResource* sampler ) {
    if ( !sampler ) {
        return;
    }

    sampler->remove_reference();
    if ( sampler->references ) {
        return;
    }

    Sampler* vk_sampler = gpu->get_sampler( sampler->handle );

    if ( vk_sampler->name ) {
        resource_cache.samplers.remove( hash_calculate( vk_sampler->name ) );
    }

    gpu->destroy_sampler( sampler->handle );
    samplers.release( sampler );
}

void Renderer::destroy_graphics_pipeline_state( GraphicsPipelineState& state ) {

    if ( state.shader.is_valid() ) {
        destroy_shader_state( state.shader );
    }

    if ( state.layout.is_valid() ) {
        gpu->destroy_pipeline_layout( state.layout );
    }

    if ( state.pipeline.is_valid() ) {
        gpu->destroy_pipeline( state.pipeline );
    }
}

void Renderer::destroy_compute_pipeline_state( ComputePipelineState& state ) {

    if ( state.shader.is_valid() ) {
        destroy_shader_state( state.shader );
    }

    if ( state.layout.is_valid() ) {
        gpu->destroy_pipeline_layout( state.layout );
    }

    if ( state.pipeline.is_valid() ) {
        gpu->destroy_pipeline( state.pipeline );
    }
}

void Renderer::destroy_ray_tracing_pipeline_state( RayTracingPipelineState& state ) {

    if ( state.shader.is_valid() ) {
        destroy_shader_state( state.shader );
    }

    if ( state.layout.is_valid() ) {
        gpu->destroy_pipeline_layout( state.layout );
    }

    if ( state.pipeline.is_valid() ) {
        gpu->destroy_pipeline( state.pipeline );
    }
}

void Renderer::destroy_shader_state( ShaderStateHandle shader_state ) {
    FlatHashMapIterator it = resource_cache.shader_reflections.find( shader_state.id );
    if ( !it.is_valid() ) {
        rprint( "Warning: trying to destroy a shader state %u that doesn't exist!\n", shader_state.id );
        return;
    }
    ShaderReflectionInfo* reflection_info = resource_cache.shader_reflections.get( it );
    reflection_info->name_hash_to_index.shutdown();
    shader_reflections.release( reflection_info );

    resource_cache.shader_reflections.remove( shader_state.id );
    //rprint( "Shader size %u, %u\n", resource_cache.shader_reflections.size, shader_reflections.used_indices );
    gpu->destroy_shader_state( shader_state );
}

void* Renderer::map_buffer( BufferResource* buffer, u32 offset, u32 size ) {

    MapBufferParameters cb_map = { buffer->handle, offset, size };
    return gpu->map_buffer( cb_map );
}

void Renderer::unmap_buffer( BufferResource* buffer ) {

    MapBufferParameters cb_map = { buffer->handle, 0, 0 };
    gpu->unmap_buffer( cb_map );
}

void Renderer::add_image_to_finalize_upload( raptor::ImageHandle texture ) {
    std::lock_guard<std::mutex> guard( texture_update_mutex );

    images_to_update.push( texture );
}

//TODO:
static void generate_mipmaps( raptor::ImageHandle texture_h, raptor::CommandBuffer* cb,
                              const raptor::ImageSyncState& final_state ) {

    using namespace raptor;

    RASSERT( cb );
    RASSERT( cb->vk_command_buffer != VK_NULL_HANDLE );

    Image* texture = cb->gpu_device->get_image( texture_h );
    RASSERT( texture && texture->vk_image != VK_NULL_HANDLE );
    RASSERT( texture->mip_level_count > 0 );

    // This mipmap path assumes color.
    RASSERT( !TextureFormat::has_depth_or_stencil( texture->vk_format ) );

    if ( texture->mip_level_count == 1 ) {
        // No mip generation. Just transition to final.
        const VkImageSubresourceRange full_range = raptor::range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 );
        cb->add_image_barrier( texture_h, full_range, final_state );
        cb->flush_barriers();
        return;
    }

    // Precondition: mip 0 must be TRANSFER_SRC_OPTIMAL before the first blit.
    // In your upload flow (transfer->graphics), acquire should set mip0 to TRANSFER_SRC_OPTIMAL.

#if defined(RAPTOR_BARRIER_DEBUG)
    // If you want, assert tracking state is already TRANSFER_SRC here.
    // RASSERT( texture->sync_state.layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL );
#endif

    int32_t w = texture->width;
    int32_t h = texture->height;

    for ( uint32_t mip_index = 1; mip_index < texture->mip_level_count; ++mip_index ) {

        const VkImageSubresourceRange mip_dst_range =
            raptor::range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, mip_index, 1, 0, 1 );

        // Transition mip N to TRANSFER_DST for the blit destination.
        // NOTE: for mip > 0 the previous layout is UNDEFINED (first time we touch it).
        cb->add_image_barrier( texture_h, mip_dst_range,
                               ImageSyncState {
                                .stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                                .access = 0,
                                .layout = VK_IMAGE_LAYOUT_UNDEFINED},
                                ImageSyncState{
                                    .stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                    .access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                    .layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                                } );

        cb->flush_barriers();

        VkImageBlit blit_region{};
        blit_region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit_region.srcSubresource.mipLevel = mip_index - 1;
        blit_region.srcSubresource.baseArrayLayer = 0;
        blit_region.srcSubresource.layerCount = 1;

        blit_region.srcOffsets[ 0 ] = { 0, 0, 0 };
        blit_region.srcOffsets[ 1 ] = { w, h, 1 };

        // Clamp to avoid reaching 0.
        const int32_t next_w = ( w > 1 ) ? ( w / 2 ) : 1;
        const int32_t next_h = ( h > 1 ) ? ( h / 2 ) : 1;

        blit_region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit_region.dstSubresource.mipLevel = mip_index;
        blit_region.dstSubresource.baseArrayLayer = 0;
        blit_region.dstSubresource.layerCount = 1;

        blit_region.dstOffsets[ 0 ] = { 0, 0, 0 };
        blit_region.dstOffsets[ 1 ] = { next_w, next_h, 1 };

        vkCmdBlitImage( cb->vk_command_buffer,
                        texture->vk_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        texture->vk_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        1, &blit_region, VK_FILTER_LINEAR );

        // After blit, mip N is in TRANSFER_DST_OPTIMAL (written).
        // Transition mip N to TRANSFER_SRC_OPTIMAL for the next iteration.
        cb->add_image_barrier( texture_h, mip_dst_range,
                               ImageSyncState{
                                .stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                .access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                .layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL},
                               ImageSyncState{
                                .stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                .access = VK_ACCESS_2_TRANSFER_READ_BIT,
                                .layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL} );

        // Only flush if another iteration will use this mip as SRC.
        if ( mip_index + 1 < texture->mip_level_count ) {
            cb->flush_barriers();
        }

        w = next_w;
        h = next_h;
    }

    // Transition last mip:
    const uint32_t last_mip = texture->mip_level_count - 1;
    const VkImageSubresourceRange last_range =
        range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, last_mip, 1, 0, 1 );

    cb->add_image_barrier( texture_h, last_range,
                           ImageSyncState{
                             .stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                             .access = VK_ACCESS_2_TRANSFER_READ_BIT,
                             .layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                           } );

    cb->flush_barriers();


    // Transition all mips to the final state (typically shader read).
    {
        const VkImageSubresourceRange full_range =
            raptor::range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, texture->mip_level_count, 0, 1 );

        // Ensure the barrier waits all the blits, otherwise Write-After-Write occurs.
        cb->add_image_barrier( texture_h, full_range,
                               ImageSyncState{
                                    .stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                    .access = VK_ACCESS_2_TRANSFER_READ_BIT,
                                    .layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL }, final_state );
        cb->flush_barriers();
    }
}

CommandBuffer* Renderer::add_image_finalize_commands( u32 thread_id ) {
    std::lock_guard<std::mutex> guard( texture_update_mutex );

    if ( images_to_update.size == 0 ) {
        return nullptr;
    }

    CommandBuffer* cb = gpu->allocate_command_buffer( thread_id, gpu->current_frame, CommandQueueType::Graphics );
    cb->begin();

    for ( u32 i = 0; i < images_to_update.size; ++i ) {

        ImageHandle image_handle = images_to_update[ i ];
        Image* image = gpu->get_image( image_handle );
        // Transfer queue -> main queue barrier
        //util_add_image_barrier_ext( cb->gpu_device, cb->vk_command_buffer, texture->vk_image, RESOURCE_STATE_COPY_DEST, RESOURCE_STATE_COPY_SOURCE,/*
        //                            0, 1, 0, 1, false, gpu->vulkan_transfer_queue_family, gpu->vulkan_main_queue_family, QueueType::CopyTransfer, QueueType::Graphics );*/

        // NOTE: asynchronous loader MUST release ownership from the transfer queue.
        VkImageSubresourceRange range = range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 );
        if ( gpu->vulkan_transfer_queue_family != gpu->vulkan_main_queue_family ) {
            cb->acquire_image_ownership( image_handle,
                                         range,
                                         gpu->vulkan_transfer_queue_family,
                                         gpu->vulkan_main_queue_family,
                                         { VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                           VK_ACCESS_2_TRANSFER_READ_BIT,
                                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL } );
        }
        else {
            cb->add_image_barrier( image_handle, range,
                                   { VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                     VK_ACCESS_2_TRANSFER_READ_BIT,
                                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL } );
        }

        cb->flush_barriers();
        generate_mipmaps( image_handle, cb,
                          { VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL } );
    }

    cb->end();

    images_to_update.clear();

    return cb;
}


// ResourceCache
void ResourceCache::init( Allocator* allocator ) {
    // Init resources caching
    textures.init( allocator, 16 );
    buffers.init( allocator, 16 );
    samplers.init( allocator, 16 );
    shader_reflections.init( allocator, 16 );
    pipelines.init( allocator, 16 );
}

void ResourceCache::shutdown( Renderer* renderer ) {

    raptor::FlatHashMapIterator it = textures.iterator_begin();

    while ( it.is_valid() ) {
        raptor::TextureResource* texture = textures.get( it );
        renderer->destroy_texture( texture );

        textures.iterator_advance( it );
    }

    it = buffers.iterator_begin();

    while ( it.is_valid() ) {
        raptor::BufferResource* buffer = buffers.get( it );
        renderer->destroy_buffer( buffer );

        buffers.iterator_advance( it );
    }

    it = samplers.iterator_begin();

    while ( it.is_valid() ) {
        raptor::SamplerResource* sampler = samplers.get( it );
        renderer->destroy_sampler( sampler );

        samplers.iterator_advance( it );
    }

    it = shader_reflections.iterator_begin();

    while ( it.is_valid() ) {
        raptor::ShaderReflectionInfo* info = shader_reflections.get( it );
        info->name_hash_to_index.shutdown();

        shader_reflections.iterator_advance( it );
    }

    it = pipelines.iterator_begin();

    while ( it.is_valid() ) {
        raptor::PipelineHandle pipeline_handle = pipelines.get( it );
        renderer->gpu->destroy_pipeline( pipeline_handle );

        pipelines.iterator_advance( it );
    }

    textures.shutdown();
    buffers.shutdown();
    samplers.shutdown();
    shader_reflections.shutdown();
    pipelines.shutdown();
}

// ImageViewDebugger /////////////////////////////////////////////////////
void ImageViewDebugger::init( Allocator* resident_allocator, GpuDevice* gpu_ ) {
    gpu = gpu_;

    image_view_indices.init( resident_allocator, gpu->images.size, gpu->images.size );
    image_names.init( resident_allocator, gpu->images.size, gpu->images.size );
    texture_names_pool.init( rkilo( 32 ), resident_allocator );

    image_view_to_debug = gpu->dummy_image_view.index();
}

void ImageViewDebugger::shutdown() {

    image_view_indices.shutdown();
    texture_names_pool.shutdown();
    image_names.shutdown();
}

void ImageViewDebugger::debug_ui() {

    u32 max_image_views = gpu->image_views.size;
    u32 active_texture_count = 0;
    u32 active_texture_index = 0;
    texture_names_pool.clear();
    for ( u32 t = gpu->dummy_image_view.index(); t < max_image_views; ++t ) {

        if ( gpu->image_views.active_elements.get_bit( t ) == 0 ) {
            continue;
        }

        ImageView* image_view = &gpu->image_views.elements[ t ];

        Image* image = gpu->get_image( image_view->parent_image );
        if ( image != nullptr && image->name != nullptr ) {
            image_names[ active_texture_count ] = texture_names_pool.append_use_f( "%s (%d)", image->name, t );
            image_view_indices[ active_texture_count ] = t;

            if ( t == image_view_to_debug ) {
                active_texture_index = active_texture_count;
            }

            ++active_texture_count;
        }
    }

    ImVec2 window_size = ImGui::GetWindowSize();
    window_size.y += 50;

    cstring combo_preview_value = image_names[ active_texture_index ];
    if ( ImGui::BeginCombo( "Image ID", combo_preview_value ) ) {
        for ( u32 t = 0; t < active_texture_count; ++t ) {
            cstring image_name = image_names[ t ];
            if ( strlen( image_name ) == 0 ) {
                continue;
            }

            // TODO(gabriel): fix image view
            const bool is_selected = ( image_view_to_debug == image_view_indices[ t ] );
            if ( ImGui::Selectable( image_name, is_selected ) ) {
                image_view_to_debug = image_view_indices[ t ];
            }

            if ( is_selected )
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // TODO(gabriel): fix image view
    ImGui::Image( (ImTextureID)&image_view_to_debug, window_size );
}


// GraphicsPipelineTransaction ///////////////////////////////////////////
GraphicsPipelineTransaction::GraphicsPipelineTransaction( Renderer* renderer_ ) {
    renderer = renderer_;
}

GraphicsPipelineTransaction::~GraphicsPipelineTransaction() {
    if ( committed ) {
        return;
    }

    for ( u32 i = 0; i < pending_states.size; i++ ) {
        renderer->destroy_graphics_pipeline_state( pending_states[ i ] );
    }

    pending_states.clear();
    current_states.clear();
}

GraphicsPipelineState& GraphicsPipelineTransaction::add( GraphicsPipelineState& current ) {

    current_states.push( &current );

    GraphicsPipelineState& pending = pending_states.push_use();
    pending = {};
    return pending;
}

void GraphicsPipelineTransaction::commit_or_rollback() {

    RASSERT( committed == false );

    // Check for invalid handles, return if found one
    for ( u32 i = 0; i < pending_states.size; i++ ) {
        GraphicsPipelineState& p = pending_states[ i ];

        if ( p.shader.is_invalid() || p.layout.is_invalid() || p.pipeline.is_invalid() ) {
            return;
        }
    }

    for ( u32 i = 0; i < pending_states.size; i++ ) {
        renderer->destroy_graphics_pipeline_state( *current_states[ i ] );

        *current_states[ i ] = pending_states[ i ];
        pending_states[ i ] = {};
    }

    committed = true;

    pending_states.clear();
    current_states.clear();
}

// ComputePipelineTransaction ////////////////////////////////////////////
ComputePipelineTransaction::ComputePipelineTransaction( Renderer* renderer_ ) {
    renderer = renderer_;
}

ComputePipelineTransaction::~ComputePipelineTransaction() {
    if ( committed ) {
        return;
    }

    for ( u32 i = 0; i < pending_states.size; i++ ) {
        renderer->destroy_compute_pipeline_state( pending_states[ i ] );
    }

    pending_states.clear();
    current_states.clear();
}

ComputePipelineState& ComputePipelineTransaction::add( ComputePipelineState& current ) {

    current_states.push( &current );

    ComputePipelineState& pending = pending_states.push_use();
    pending = {};
    return pending;
}

void ComputePipelineTransaction::commit_or_rollback() {

    RASSERT( committed == false );

    // Check for invalid handles, return if found one
    for ( u32 i = 0; i < pending_states.size; i++ ) {
        ComputePipelineState& p = pending_states[ i ];

        if ( p.shader.is_invalid() || p.layout.is_invalid() || p.pipeline.is_invalid() ) {
            return;
        }
    }

    for ( u32 i = 0; i < pending_states.size; i++ ) {
        renderer->destroy_compute_pipeline_state( *current_states[ i ] );

        *current_states[ i ] = pending_states[ i ];
        pending_states[ i ] = {};
    }

    committed = true;

    pending_states.clear();
    current_states.clear();
}

// RayTracingPipelineTransaction /////////////////////////////////////////
RayTracingPipelineTransaction::RayTracingPipelineTransaction( Renderer* renderer_ ) {
    renderer = renderer_;
}

RayTracingPipelineTransaction::~RayTracingPipelineTransaction() {
    if ( committed ) {
        return;
    }
    for ( u32 i = 0; i < pending_states.size; i++ ) {
        renderer->destroy_ray_tracing_pipeline_state( pending_states[ i ] );
    }
    pending_states.clear();
    current_states.clear();
}

RayTracingPipelineState& RayTracingPipelineTransaction::add( RayTracingPipelineState& current ) {
    current_states.push( &current );

    RayTracingPipelineState& pending = pending_states.push_use();
    pending = {};
    return pending;
}

void RayTracingPipelineTransaction::commit_or_rollback() {

    RASSERT( committed == false );

    // Check for invalid handles, return if found one
    for ( u32 i = 0; i < pending_states.size; i++ ) {
        RayTracingPipelineState& p = pending_states[ i ];

        if ( p.shader.is_invalid() || p.layout.is_invalid() || p.pipeline.is_invalid() ) {
            return;
        }
    }

    for ( u32 i = 0; i < pending_states.size; i++ ) {
        renderer->destroy_ray_tracing_pipeline_state( *current_states[ i ] );

        *current_states[ i ] = pending_states[ i ];
        pending_states[ i ] = {};
    }

    committed = true;

    pending_states.clear();
    current_states.clear();
}

// ShaderReflectionInfo //////////////////////////////////////////////////
u16 ShaderReflectionInfo::get_binding_index( cstring name ) {
    u64 name_hash = hash_calculate( name );
    return name_hash_to_index.get( name_hash );
}

u16 ShaderReflectionInfo::get_binding_index_from_hash( u64 name_hash ) {
    return name_hash_to_index.get( name_hash );
}

void ShaderReflectionInfo::dump_bindings() {
    FlatHashMapIterator it = name_hash_to_index.iterator_begin();

    rprint( "%s bindings:\n", name );
    while ( it.is_valid() ) {
        u16 binding_index = name_hash_to_index.get( it );

        rprint( " %d, ", binding_index );

        name_hash_to_index.iterator_advance( it );
    }

    rprint( "\n" );
}

} // namespace raptor
