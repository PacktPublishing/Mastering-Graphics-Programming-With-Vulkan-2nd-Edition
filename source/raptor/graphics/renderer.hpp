#pragma once

#include "graphics/gpu_device.hpp"
#include "graphics/gpu_resources.hpp"
#include "graphics/shader_reflection.hpp"

#include "foundation/static_array.hpp"
#include "foundation/resource_manager.hpp"
#include "foundation/string.hpp"

namespace raptor {

struct AsynchronousLoader;
struct DescriptorSetBinder;
struct FrameGraph;
struct Renderer;
struct RenderBlackboard;
struct ShaderReflection;

//
//
struct BufferResource : public raptor::Resource {

    BufferHandle                    handle;
    u32                             pool_index;

    static constexpr cstring        k_type = "raptor_buffer_type";
    static u64                      k_type_hash;

}; // struct Buffer

//
//
struct TextureResource : public raptor::Resource {

    ImageHandle                     image;
    ImageViewHandle                 image_view;
    u32                             pool_index;

    static constexpr cstring        k_type = "raptor_texture_type";
    static u64                      k_type_hash;

}; // struct Texture

//
//
struct SamplerResource : public raptor::Resource {

    SamplerHandle                   handle;
    u32                             pool_index;

    static constexpr cstring        k_type = "raptor_sampler_type";
    static u64                      k_type_hash;

}; // struct Sampler

// Material/Shaders ///////////////////////////////////////////////////////

//
//
struct ShaderReflectionInfo {

    FlatHashMap<u64, u16>           name_hash_to_index;

    cstring                         name    = nullptr;
    u32                             pool_index;


    u16                             get_binding_index( cstring name );
    u16                             get_binding_index_from_hash( u64 name_hash );
    
    void                            dump_bindings();
}; // struct ShaderReflectionInfo

// PipelineStates ////////////////////////////////////////////////////////
//
//
struct GraphicsPipelineState {

    ShaderStateHandle               shader;
    PipelineLayoutHandle            layout;
    PipelineHandle                  pipeline;

}; // struct GraphicsPipelineState

//
//
struct ComputePipelineState {

    ShaderStateHandle               shader;
    PipelineLayoutHandle            layout;
    PipelineHandle                  pipeline;

}; // struct GraphicsPipelineState

//
//
struct RayTracingPipelineState {

    ShaderStateHandle               shader;
    PipelineLayoutHandle            layout;
    PipelineHandle                  pipeline;

}; // struct GraphicsPipelineState

// PipelineStatesTransaction /////////////////////////////////////////////
//
// Reload helpers:
// Add the current state to be updated, build with the pending state returned
// by Add method, then commit only if successfull.
struct GraphicsPipelineTransaction {

    GraphicsPipelineTransaction( Renderer* renderer );
    ~GraphicsPipelineTransaction();

    GraphicsPipelineState&          add( GraphicsPipelineState& current );
    void                            commit_or_rollback();

    StaticArray<GraphicsPipelineState*, 8>  current_states;
    StaticArray<GraphicsPipelineState, 8>   pending_states;

    Renderer*                       renderer    = nullptr;
    bool                            committed   = false;

}; // struct GraphicsPipelineTransaction

struct ComputePipelineTransaction {

    ComputePipelineTransaction( Renderer* renderer );
    ~ComputePipelineTransaction();

    ComputePipelineState&           add( ComputePipelineState& current );
    void                            commit_or_rollback();

    StaticArray<ComputePipelineState*, 16>  current_states;
    StaticArray<ComputePipelineState, 16>   pending_states;

    Renderer*                       renderer    = nullptr;
    bool                            committed   = false;

}; // struct ComputePipelineTransaction

struct RayTracingPipelineTransaction {

    RayTracingPipelineTransaction( Renderer* renderer );
    ~RayTracingPipelineTransaction();

    RayTracingPipelineState&        add( RayTracingPipelineState& current );
    void                            commit_or_rollback();

    StaticArray<RayTracingPipelineState*, 8>  current_states;
    StaticArray<RayTracingPipelineState, 8>   pending_states;

    Renderer*                       renderer    = nullptr;
    bool                            committed   = false;
}; // struct RayTracingPipelineTransaction

// ResourceCache /////////////////////////////////////////////////////////

//
//
struct ResourceCache {

    void                            init( Allocator* allocator );
    void                            shutdown( Renderer* renderer );

    FlatHashMap<u64, TextureResource*> textures;
    FlatHashMap<u64, BufferResource*>  buffers;
    FlatHashMap<u64, SamplerResource*> samplers;
    FlatHashMap<u64, ShaderReflectionInfo*> shader_reflections;
    FlatHashMap<u64, PipelineHandle>   pipelines;

    char                            binary_data_folder[512];

}; // struct ResourceCache

///////////////////////////////////////////////////////////////////////////

//
//
enum class BufferUploadPolicy : u8 {
    Best,
    HostWrite,
    Transfer
}; // enum class BufferUploadPolicy

//
//
struct BufferUpload {
    const void*             data    = nullptr;
    BufferUploadPolicy      policy  = BufferUploadPolicy::Best;
}; // struct BufferUpload

//
//
struct DescriptorSetBinder {

    StaticArray<TextureDescriptor, 4> textures;
    StaticArray<TextureDescriptor, 4> images;
    StaticArray<BufferDescriptor, 4> buffers;
    StaticArray<BufferDescriptor, 24> ssbos;
    StaticArray<DynamicBufferBinding, 4> dynamic_buffers;
    TLASDescriptor          tlas;
    cstring                 name;

    void                    reset() {
        textures.clear();
        images.clear();
        buffers.clear();
        ssbos.clear();
        dynamic_buffers.clear();
        tlas.tlas = {};
        name = nullptr;
    }

    void                    bind_ssbo( BufferHandle handle, u16 binding ) {
        ssbos.push( { handle, binding } );
    }

    void                    bind_ssbo( BufferDescriptor& descriptor ) {
        ssbos.push( descriptor );
    }

    void                    bind_dynamic_buffer( u32 binding, u32 size ) {
        dynamic_buffers.push( { binding, size } );
    }

}; // struct RenderSceneDescriptors

// Renderer ///////////////////////////////////////////////////////////////
//
//
struct RendererResourcePoolCreation {

    u16                         textures    = 256;
    u16                         buffers     = 256;
    u16                         samplers    = 64;
    u16                         materials   = 256;
    u16                         techniques  = 128;
    u16                         shader_reflections = 256;

}; // struct RendererResourcePoolCreation
//
//
struct RendererCreation {

    raptor::GpuDevice*          gpu;
    Allocator*                  allocator;

    RendererResourcePoolCreation resource_pool_creation;

}; // struct RendererCreation

// 
struct ImageViewDebugger {

    void            init( Allocator* resident_allocator, GpuDevice* gpu );
    void            shutdown();

    void            debug_ui();

    GpuDevice*      gpu;

    u32             image_view_to_debug = 0;
    Array<u32>      image_view_indices;
    Array<cstring>  image_names;

    StringBuffer    texture_names_pool;
};

//
// Main class responsible for handling all high level resources
//
struct Renderer : public Service {

    RAPTOR_DECLARE_SERVICE( Renderer );

    void                        init( const RendererCreation& creation );
    void                        shutdown();

    void                        set_loaders( raptor::ResourceManager* manager );

    void                        imgui_draw();

    void                        set_presentation_mode( PresentMode::Enum value );

    f32                         aspect_ratio() const;

    // Creation/destruction
    //BufferResource*             create_buffer( const BufferCreation& creation );
    //BufferResource*             create_buffer( VkBufferUsageFlags type, ResourceUsageType::Enum usage, u32 size, void* data, cstring name, bool bindless = false );

    TextureResource*            create_texture( const ImageCreation& creation );
    TextureResource*            create_texture_from_file( cstring full_filename, bool generate_mips, bool srgb );

    SamplerResource*            create_sampler( const SamplerCreation& creation );

    bool                        create_graphics_pipeline_state( const ShaderCompilationCreation& shader_creation,
                                                                const PipelineCreation& pipeline_creation, cstring name,
                                                                FrameGraph* frame_graph,
                                                                GraphicsPipelineState& out_pipeline_state );

    bool                        create_compute_pipeline_state( const ShaderCompilationCreation& shader_creation,
                                                               const PipelineCreation& pipeline_creation, cstring name,
                                                               FrameGraph* frame_graph,
                                                               ComputePipelineState& out_pipeline_state );

    bool                        create_raytracing_pipeline_state( const ShaderCompilationCreation& shader_creation,
                                                               const PipelineCreation& pipeline_creation, cstring name,
                                                               FrameGraph* frame_graph,
                                                               RayTracingPipelineState& out_pipeline_state );

    // Lower-level methods to create GpuResources
    ShaderStateHandle           create_shader_state( const ShaderCompilationCreation& creation,
                                                     cstring technique_name, ShaderReflection* out_reflection );
    PipelineLayoutHandle        create_pipeline_layout( const ShaderReflection& reflection );
    PipelineHandle              create_pipeline( const ShaderReflection& reflection, PipelineCreation& creation );

    DescriptorSetHandle         create_descriptor_set( DescriptorSetBinder& binder, ShaderReflectionInfo* reflection_info, 
                                                       PipelineHandle pipeline, u32 frame_index, RenderBlackboard& render_blackboard );
    BufferHandle                create_buffer_with_upload( const BufferCreation& creation, const BufferUpload& upload );

    void                        destroy_buffer( BufferResource* buffer );
    void                        destroy_texture( TextureResource* texture );
    void                        destroy_sampler( SamplerResource* sampler );

    void                        destroy_graphics_pipeline_state( GraphicsPipelineState& state );
    void                        destroy_compute_pipeline_state( ComputePipelineState& state );
    void                        destroy_ray_tracing_pipeline_state( RayTracingPipelineState& state );

    void                        destroy_shader_state( ShaderStateHandle shader_state );


    // Reflection
    ShaderReflectionInfo*       get_shader_reflection( PipelineHandle pipeline );
    u16                         get_binding_index( ShaderReflectionInfo* reflection_info, cstring name );
    u16                         get_binding_index_from_hash( ShaderReflectionInfo* reflection_info, u64 name_hash );

    // Update resources
    void*                       map_buffer( BufferResource* buffer, u32 offset = 0, u32 size = 0 );
    void                        unmap_buffer( BufferResource* buffer );

    CommandBuffer*              allocate_command_buffer( u32 thread_index, u32 current_frame_index, CommandQueueType queue_type )  { return gpu->allocate_command_buffer( thread_index, current_frame_index, queue_type ); }

    // Add image to finalize upload list, multithread friendly
    void                        add_image_to_finalize_upload( ImageHandle texture );

    // Finalize upload by changing ownership between transfer and graphics queue and
    // generating mipmaps if needed.
    CommandBuffer*              add_image_finalize_commands( u32 thread_id );

    ResourcePoolTyped<TextureResource>  textures;
    ResourcePoolTyped<BufferResource>   buffers;
    ResourcePoolTyped<SamplerResource>  samplers;
    ResourcePoolTyped<ShaderReflectionInfo> shader_reflections;

    ResourceCache               resource_cache;

    StaticArray<ImageHandle, 128> images_to_update;

    AsynchronousLoader*         async_loader    = nullptr;
    GpuDevice*                  gpu             = nullptr;
    Allocator*                  resident_allocator = nullptr;
    ArenaAllocator              temporary_allocator;
    StringBuffer                names_buffer;

    Array<VmaBudget>            gpu_heap_budgets;

    static constexpr cstring    k_name = "raptor_rendering_service";

}; // struct Renderer

} // namespace raptor
