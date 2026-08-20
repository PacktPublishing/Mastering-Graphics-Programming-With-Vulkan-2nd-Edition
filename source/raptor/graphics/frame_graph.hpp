#pragma once

#include "foundation/array.hpp"
#include "foundation/static_array.hpp"
#include "foundation/data_structures.hpp"
#include "foundation/hash_map.hpp"
#include "foundation/service.hpp"
#include "foundation/string.hpp"

#include "graphics/gpu_resources.hpp"

namespace raptor {

struct Allocator;
struct CommandBuffer;
struct FrameGraph;
struct GpuDevice;
struct RenderScene;
struct RenderView;
struct Renderer;
struct RenderResourcesLoader;
struct FrameGraphBuilder;
struct RenderBlackboard;
struct RenderConfig;

typedef u32                         FrameGraphHandle;

struct FrameGraphResourceHandle {
    FrameGraphHandle                index;
};

struct FrameGraphNodeHandle {
    FrameGraphHandle                index;
};

enum FrameGraphResourceType {
    FrameGraphResourceType_Invalid         = -1,

    FrameGraphResourceType_Buffer          = 0,
    FrameGraphResourceType_Texture         = 1,
    FrameGraphResourceType_Attachment      = 2,
    FrameGraphResourceType_Reference       = 3,
    FrameGraphResourceType_ShadingRate     = 4,
    FrameGraphResourceType_Present         = 5
};

struct FrameGraphResourceInfo {
    bool                                    external = false;

    union {
        struct {
            sizet                           size;
            VkBufferUsageFlags              flags;

            BufferHandle                    handle;
        } buffer;

        struct {
            u32                             width = 0;
            u32                             height = 0;
            u32                             depth = 1;
            f32                             scale_width = 1.0f;
            f32                             scale_height = 1.0f;

            VkFormat                        format;
            VkImageUsageFlags               flags;

            VkAttachmentLoadOp              load_op;

            ImageHandle                     image;
            ImageViewHandle                 image_view;
            ImageHandle                     previous_image;
            ImageViewHandle                 previous_image_view;
            f32                             clear_values[ 4 ];  // Reused between color or depth/stencil.

            bool                            compute;
            bool                            disable_memory_aliasing;
            bool                            persistent; // If true, resource will not be used for memory aliasing
                                                        // and have more than one texture to be queried. Ideally k_max_frames.
        } texture;
    };

    FrameGraphResourceInfo&                 set_external( bool value );
    FrameGraphResourceInfo&                 set_buffer( sizet size, VkBufferUsageFlags flags, BufferHandle handle );

    FrameGraphResourceInfo&                 set_external_texture_2d( u32 width, u32 height, VkFormat format, VkImageUsageFlags flags, ImageHandle image, ImageViewHandle image_view );
    FrameGraphResourceInfo&                 set_external_texture_3d( u32 width, u32 height, u32 depth, VkFormat format, VkImageUsageFlags flags, ImageHandle image, ImageViewHandle image_view );

}; // struct FrameGraphResourceInfo

// NOTE(marco): an input could be used as a texture or as an attachment.
// If it's an attachment we want to control whether to discard previous
// content - for instance the first time we use it - or to load the data
// from a previous pass
// NOTE(marco): an output always implies an attachment and a store op
struct FrameGraphResource {
    FrameGraphResourceType                  type = FrameGraphResourceType_Invalid;
    FrameGraphResourceInfo                  resource_info{};

    FrameGraphNodeHandle                    producer{ k_invalid_index };
    FrameGraphResourceHandle                output_handle{ k_invalid_index }; // TODO(marco): rename this reference

    i32                                     ref_count = 0; // TODO(marco): we can probably remove this and use an array during compilation
    u16                                     rename_count = 0; // Number of times this resource has been used as an output

    cstring                                 name = nullptr;
}; // struct FrameGraphResource

//
struct FrameGraphResourceCreation {
    FrameGraphResourceType                  type;
    FrameGraphResourceInfo                  resource_info;

    cstring                                 name;
}; // struct FrameGraphResourceCreation

//
struct FrameGraphResourceCreation_v2 {
    FrameGraphResourceType                  type;
    FrameGraphResourceHandle                handle;
}; // struct FrameGraphResourceCreation_v2

//
struct FrameGraphScheduling {

    CommandQueueType                        queue_type  = CommandQueueType::Graphics;
    u8                                      phase       = 0;
}; // struct FrameGraphScheduling

//
struct FrameGraphNodeCreation {
    Span<const FrameGraphResourceCreation>  inputs;
    Span<const FrameGraphResourceCreation>  outputs;

    bool                                    enabled;

    const char*                             name;
    bool                                    compute;
    bool                                    ray_tracing;
}; // struct FrameGraphNodeCreation

//
struct FrameGraphNodeCreation_v2 {
    Span<const FrameGraphResourceCreation_v2> inputs;
    Span<const FrameGraphResourceHandle>    outputs;

    FrameGraphScheduling                    scheduling;
    bool                                    enabled;
    bool                                    compute;
    bool                                    ray_tracing;

    const char*                             name;
}; // struct FrameGraphNodeCreation_v2

//
struct FrameGraphRenderContext {
    Renderer*                               renderer;
    CommandBuffer*                          gpu_commands;
    FrameGraph*                             frame_graph;
    RenderView*                             render_view;
    RenderBlackboard*                       render_blackboard;
    RenderConfig*                           render_config;
    u32                                     current_frame_index;
}; // struct FrameGraphRenderContext

//
struct FrameGraphResourceContext {
    Renderer*                               renderer;
    FrameGraph*                             frame_graph;
    RenderBlackboard*                       render_blackboard;
    RenderConfig*                           render_config;
    RenderScene*                            render_scene;
}; // struct FrameGraphResourceContext

//
enum class PipelineUpdatePhase : u8 {
    Create,
    Destroy,
    Reload
}; // enum class PipelineUpdatePhase

//
struct FrameGraphRenderPass {
    virtual void                            add_ui() { }

    // Graph declaration
    virtual void                            declare_frame_graph_node( FrameGraphResourceContext& context ) {}

    // Pipeline management
    virtual void                            update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) {}

    // Resource management
    virtual void                            create_gpu_resources( FrameGraphResourceContext& context ) {}
    virtual void                            destroy_gpu_resources( FrameGraphResourceContext& context ) {}
    virtual void                            update_dependent_resources( FrameGraphResourceContext& context ) {}

    // Resize handling
    virtual void                            on_resize( FrameGraphResourceContext& context, u32 new_width, u32 new_height ) {}

    // Per frame data update
    virtual void                            upload_gpu_data( FrameGraphResourceContext& context ) {}

    // Rendering
    virtual void                            pre_render( FrameGraphRenderContext& context ) {}
    virtual void                            render( FrameGraphRenderContext& context ) {}
    virtual void                            post_render( FrameGraphRenderContext& context ) {}

    cstring                                 name        = nullptr;
    bool                                    enabled     = true;
}; // struct FrameGraphRenderPass

//
struct FrameGraphNode {
    u8                                      version = 0;
    FrameGraphScheduling                    scheduling;
    i32                                     ref_count = 0;

    RenderPassOutput                        render_pass_output;
    StaticArray<ImageViewHandle, 8>         output_image_views;
    ImageViewHandle                         output_depth_image_view;
    ImageViewHandle                         output_shading_rate;

    FrameGraphRenderPass*                   graph_render_pass;

    // TODO(marco): we should avoid duplicating the whole FrameGraphResource object and store just a reference here
    Array<FrameGraphResourceHandle>         inputs;
    Array<FrameGraphResourceType>           temp_inputs_type;
    Array<FrameGraphResourceHandle>         outputs;

    Array<FrameGraphNodeHandle>             edges;

    f32                                     resolution_scale_width = 0.f;
    f32                                     resolution_scale_height = 0.f;
    bool                                    compute = false;
    bool                                    ray_tracing = false;
    bool                                    enabled = true;
    bool                                    render_pass_output_cached = false;
    bool                                    output_textures_cached = false;

    const char*                             name    = nullptr;
}; // struct FrameGraphNode

//
struct FrameGraphRenderPassCache {
    void                                    init( Allocator* allocator );
    void                                    shutdown( );

    FlatHashMap<u64, FrameGraphRenderPass*> render_pass_map;
}; // struct FrameGraphRenderPassCache

//
struct FrameGraphResourceCache {
    void                                    init( Allocator* allocator, GpuDevice* device );
    void                                    shutdown( );

    GpuDevice*                              device;

    FlatHashMap<u64, u32>                   resource_map;
    ResourcePoolTyped<FrameGraphResource>   resources;
}; // struct FrameGraphResourceCache

//
struct FrameGraphNodeCache {
    void                                    init( Allocator* allocator, GpuDevice* device );
    void                                    shutdown( );

    GpuDevice*                              device;

    FlatHashMap<u64, u32>                   node_map;
    ResourcePool                            nodes;
}; // struct FrameGraphNodeCache

//
//
struct FrameGraphBuilder : public Service {
    void                            init( GpuDevice* device );
    void                            shutdown();

    void                            register_render_pass( cstring name, FrameGraphRenderPass* render_pass );
    template<typename T>
    T*                              get_render_pass( cstring name );

    [[deprecated("Use create_output_handle instead")]]
    FrameGraphResourceHandle        create_node_output( const FrameGraphResourceCreation& creation, FrameGraphNodeHandle producer );
    [[deprecated("Use add_node_input instead")]]
    FrameGraphResourceHandle        create_node_input( const FrameGraphResourceCreation& creation );
    FrameGraphResourceHandle        create_output_handle( const FrameGraphResourceCreation& creation );
    FrameGraphResourceHandle        create_output_reference( FrameGraphResourceHandle handle, FrameGraphResourceType type );
    FrameGraphNodeHandle            create_node( const FrameGraphNodeCreation& creation );
    FrameGraphNodeHandle            create_node_v2( const FrameGraphNodeCreation_v2& creation );

    FrameGraphResourceHandle        get_output_handle( cstring node_name, cstring resource_name );

    FrameGraphNode*                 get_node( cstring name );
    FrameGraphNode*                 access_node( FrameGraphNodeHandle handle );

    void                            add_resource( cstring name, FrameGraphResourceType type, FrameGraphResourceInfo resource_info );
    FrameGraphResource*             get_resource( cstring name );
    FrameGraphResource*             access_resource( FrameGraphResourceHandle handle );

    void                            set_resolution_info( ResolutionInfo* info ) { resolution_info = info; }

    FrameGraphResourceCache         resource_cache;
    FrameGraphNodeCache             node_cache;
    FrameGraphRenderPassCache       render_pass_cache;

    Allocator*                      allocator                           = nullptr;
    StringBuffer                    rename_buffer;
    GpuDevice*                      device                              = nullptr;
    ResolutionInfo*                 resolution_info                     = nullptr;

    static constexpr u32            k_max_render_pass_count             = 256;
    static constexpr u32            k_max_resources_count               = 1024;
    static constexpr u32            k_max_nodes_count                   = 1024;

    static constexpr cstring        k_name                              = "raptor_frame_graph_builder_service";
}; // struct FrameGraphBuilder

template<typename T>
T* FrameGraphBuilder::get_render_pass( cstring name ) {
    u64 key = hash_calculate( name );

    FlatHashMapIterator it = render_pass_cache.render_pass_map.find( key );
    if ( !it.is_valid() ) {
        return nullptr;
    }

    FrameGraphRenderPass* rp = render_pass_cache.render_pass_map.get( it );
    return dynamic_cast<T*>( rp );
}

//
//
struct FrameGraphExecutionBatch {
    CommandBuffer*                  cb;
    Array<FrameGraphNodeHandle>     nodes;
    FrameGraphScheduling            scheduling;
}; // struct FrameGraphExecutionBatch

//
//
struct FrameGraph {
    void                            init( FrameGraphBuilder* builder );
    void                            shutdown();

    void                            parse( cstring file_path, ArenaAllocator* temp_allocator );

    // NOTE(marco): each frame we rebuild the graph so that we can enable only
    // the nodes we are interested in
    void                            reset();
    void                            enable_render_pass( cstring render_pass_name );
    void                            disable_render_pass( cstring render_pass_name );

    void                            compile();
    void                            add_ui();

    void                            render( u32 frame_index, u32 thread_index, Renderer* renderer, 
                                            RenderView* render_view, RenderBlackboard* render_blackboard, RenderConfig* render_config );
    void                            on_resize( Renderer* renderer, RenderBlackboard* render_blackboard, RenderConfig* render_config, u32 new_width, u32 new_height );
    void                            reload_shaders( RenderScene& scene, RenderBlackboard* render_blackboard, RenderConfig* render_config );
    void                            update_persistent_resources_handles();

    CommandBuffer*                  get_command_buffer_from_batch( CommandQueueType queue, u32 phase );

    void                            debug_ui();

    [[deprecated("Use add_node_v2 instead")]]
    void                            add_node( FrameGraphNodeCreation& creation );
    void                            add_node_v2( const FrameGraphNodeCreation_v2& creation );
    FrameGraphNode*                 get_node( cstring name );
    FrameGraphNode*                 access_node( FrameGraphNodeHandle handle );

    void                            cache_render_pass_output( cstring node_name, GpuDevice* gpu, RenderPassOutput& render_pass_output, bool compute_node );

    void                            add_resource( cstring name, FrameGraphResourceType type, FrameGraphResourceInfo resource_info );
    FrameGraphResource*             get_resource( cstring name );
    FrameGraphResource*             access_resource( FrameGraphResourceHandle handle );
    FrameGraphResource*             access_output_resource( FrameGraphResourceHandle handle );

    // NOTE(marco): nodes sorted in topological order
    Array<FrameGraphNodeHandle>     nodes;
    Array<FrameGraphNodeHandle>     all_nodes;
    Array<FrameGraphExecutionBatch> batches;
    Array<FrameGraphNodeHandle>     persistent_update_nodes;   // Cache of persistent nodes to flip current/previous handles 
                                                               // and update outputs

    FrameGraphBuilder*              builder;
    Allocator*                      allocator;

    ArenaAllocator                  local_allocator;
    bool                            per_frame_persistent_update_called = true; // Flag to catch persistent update bugs

    cstring                         name = nullptr;
}; // struct FrameGraph

} // namespace raptor
