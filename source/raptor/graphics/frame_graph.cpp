#include "frame_graph.hpp"

#include "foundation/file.hpp"
#include "foundation/memory.hpp"
#include "foundation/string.hpp"

#include "graphics/command_buffer.hpp"
#include "graphics/gpu_device.hpp"
#include "graphics/gpu_resources.hpp"
#include "graphics/frame_renderer.hpp"
#include "graphics/gpu_profiler.hpp"
#include "graphics/render_scene.hpp"
#include "graphics/renderer.hpp"

#include "external/json.hpp"
#include "external/imgui/imgui.h"
#include "external/tracy/tracy/Tracy.hpp"

#include <string>

#define FRAME_GRAPH_DEBUG 0

namespace raptor
{

static FrameGraphResourceType string_to_resource_type( cstring input_type ) {
    if ( strcmp( input_type, "texture" ) == 0 ) {
        return FrameGraphResourceType_Texture;
    }

    if ( strcmp( input_type, "attachment" ) == 0 ) {
        return FrameGraphResourceType_Attachment;
    }

    if ( strcmp( input_type, "buffer" ) == 0 ) {
        return FrameGraphResourceType_Buffer;
    }

    if ( strcmp( input_type, "reference" ) == 0 ) {
        // This is used for resources that need to create an edge but are not actually
        // used by the render pass
        return FrameGraphResourceType_Reference;
    }

    if ( strcmp( input_type, "shading_rate" ) == 0 ) {
        return FrameGraphResourceType_ShadingRate;
    }

    RASSERT( false );
    return FrameGraphResourceType_Invalid;
}

VkAttachmentLoadOp string_to_render_pass_operation( cstring op ) {
    if ( strcmp( op, "clear" ) == 0 ) {
        return VK_ATTACHMENT_LOAD_OP_CLEAR;
    } else if ( strcmp( op, "load" ) == 0 ) {
        return VK_ATTACHMENT_LOAD_OP_LOAD;
    }

    RASSERT( false );
    return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
}

// FrameGraph /////////////////////////////////////////////////////////////

void FrameGraph::init( FrameGraphBuilder* builder_ ) {
    allocator = &MemoryService::instance()->system_allocator;

    local_allocator.init( rmega( 1 ) );

    builder = builder_;

    nodes.init( allocator, FrameGraphBuilder::k_max_nodes_count );
    all_nodes.init( allocator, FrameGraphBuilder::k_max_nodes_count );
    persistent_update_nodes.init( allocator, 8 );
    batches.init( allocator, 8 );
}

void FrameGraph::shutdown() {
    for ( u32 i = 0; i < all_nodes.size; ++i ) {
        FrameGraphNodeHandle handle = all_nodes[ i ];
        FrameGraphNode* node = builder->access_node( handle );

        node->inputs.shutdown();
        node->temp_inputs_type.shutdown();
        node->outputs.shutdown();
        node->edges.shutdown();
    }

    for ( u32 i = 0; i < batches.size; i++ ) {
        FrameGraphExecutionBatch& batch = batches[ i ];
        batch.nodes.shutdown();
    }

    batches.shutdown();
    persistent_update_nodes.shutdown();
    all_nodes.shutdown();
    nodes.shutdown();

    local_allocator.shutdown();
}

void FrameGraph::parse( cstring file_path, ArenaAllocator* temp_allocator ) {
    using json = nlohmann::json;

    if ( !file_exists( file_path ) ) {
        rprint( "Cannot find file %s\n", file_path );
        return;
    }

    sizet current_allocator_marker = temp_allocator->get_marker();

    FileReadResult read_result = file_read_text( file_path, temp_allocator );

    json graph_data = json::parse( read_result.data );

    StringBuffer string_buffer;
    string_buffer.init( 2048, &local_allocator );

    std::string name_value = graph_data.value( "name", "" );
    name = string_buffer.append_use_f( "%s", name_value.c_str() );

    json passes = graph_data[ "passes" ];
    for ( sizet i = 0; i < passes.size(); ++i ) {
        json pass = passes[ i ];

        json pass_inputs = pass[ "inputs" ];
        json pass_outputs = pass[ "outputs" ];

        FrameGraphNodeCreation node_creation{ };
        Array<FrameGraphResourceCreation> node_inputs{ };
        node_inputs.init( temp_allocator, (u32)pass_inputs.size() );
        Array<FrameGraphResourceCreation> node_outputs{ };
        node_outputs.init( temp_allocator, (u32)pass_outputs.size() );

        std::string node_type = pass.value( "type", "" );
        node_creation.compute = node_type.compare( "compute" ) == 0;
        node_creation.ray_tracing = node_type.compare( "ray_tracing" ) == 0;

        for ( sizet ii = 0; ii < pass_inputs.size(); ++ii ) {
            json pass_input = pass_inputs[ ii ];

            FrameGraphResourceCreation input_creation{ };

            std::string input_type = pass_input.value( "type", "" );
            RASSERT( !input_type.empty() );

            std::string input_name = pass_input.value( "name", "" );
            RASSERT( !input_name.empty() );

            input_creation.type = string_to_resource_type( input_type.c_str() );
            input_creation.resource_info.external = false;

            input_creation.name = string_buffer.append_use_f( "%s", input_name.c_str() );

            node_inputs.push( input_creation );
        }

        for ( sizet oi = 0; oi < pass_outputs.size(); ++oi ) {
            json pass_output = pass_outputs[ oi ];

            FrameGraphResourceCreation output_creation{ };

            std::string output_type = pass_output.value( "type", "" );
            RASSERT( !output_type.empty() );

            std::string output_name = pass_output.value( "name", "" );
            RASSERT( !output_name.empty() );

            bool external = pass_output.value( "external", false );
            output_creation.resource_info.external = external;

            output_creation.type = string_to_resource_type( output_type.c_str() );
            output_creation.name = string_buffer.append_use_f( "%s", output_name.c_str() );

            switch ( output_creation.type ) {
                case FrameGraphResourceType_Texture:
                {
                    // NOTE(marco): for now output textures are all managed manually. We add them to the graph
                    // to make sure they are considered when performing the topological sort
                } break;
                case FrameGraphResourceType_Attachment:
                {
                    std::string format = pass_output.value( "format", "" );
                    RASSERT( !format.empty() );

                    output_creation.resource_info.texture.format = util_string_to_vk_format( format.c_str() );

                    std::string load_op = pass_output.value( "load_operation", "" );
                    RASSERT( !load_op.empty() );

                    output_creation.resource_info.texture.load_op = string_to_render_pass_operation( load_op.c_str() );

                    json resolution = pass_output[ "resolution" ];
                    json scaling = pass_output[ "resolution_scale" ];

                    if ( resolution.is_array() ) {
                        output_creation.resource_info.texture.width = resolution[ 0 ];
                        output_creation.resource_info.texture.height = resolution[ 1 ];
                        output_creation.resource_info.texture.depth = 1;
                        output_creation.resource_info.texture.scale_width = 0.f;
                        output_creation.resource_info.texture.scale_height = 0.f;
                    }
                    else if ( scaling.is_array() ) {
                        output_creation.resource_info.texture.width = 0;
                        output_creation.resource_info.texture.height = 0;
                        output_creation.resource_info.texture.depth = 1;
                        output_creation.resource_info.texture.scale_width = scaling[ 0 ];
                        output_creation.resource_info.texture.scale_height = scaling[ 1 ];
                    }
                    else {
                        // Defaults
                        output_creation.resource_info.texture.width = 0;
                        output_creation.resource_info.texture.height = 0;
                        output_creation.resource_info.texture.depth = 1;
                        output_creation.resource_info.texture.scale_width = 1.f;
                        output_creation.resource_info.texture.scale_height = 1.f;
                    }

                    output_creation.resource_info.texture.compute = node_creation.compute;

                    // Parse depth/stencil values
                    if ( TextureFormat::has_depth( output_creation.resource_info.texture.format ) ) {
                        output_creation.resource_info.texture.clear_values[ 0 ] = pass_output.value( "clear_depth", 1.0f );
                        output_creation.resource_info.texture.clear_values[ 1 ] = pass_output.value( "clear_stencil", 0.0f );
                    }
                    else {
                        // Parse color array
                        json clear_color_array = pass_output[ "clear_color" ];
                        if ( clear_color_array.is_array() ) {
                            for ( u32 c = 0; c < clear_color_array.size(); ++c) {
                                output_creation.resource_info.texture.clear_values[ c ] = clear_color_array[ c ];
                            }
                        }
                        else {
                            if ( output_creation.resource_info.texture.load_op == RenderPassOperation::Clear ) {
                                rprint( "Error parsing output texture %s: load operation is clear, but clear color not specified. Defaulting to 0,0,0,0.\n", output_creation.name );
                            }
                            output_creation.resource_info.texture.clear_values[ 0 ] = 0.0f;
                            output_creation.resource_info.texture.clear_values[ 1 ] = 0.0f;
                            output_creation.resource_info.texture.clear_values[ 2 ] = 0.0f;
                            output_creation.resource_info.texture.clear_values[ 3 ] = 0.0f;
                        }
                    }

                } break;
                case FrameGraphResourceType_Buffer:
                {
                    // NOTE(marco): for now buffers are all managed manually. We add them to the graph
                    // to make sure they are considered when performing the topological sort
                } break;
            }

            node_outputs.push( output_creation );
        }

        node_creation.inputs = Span<const FrameGraphResourceCreation>( node_inputs.data, node_inputs.size );
        node_creation.outputs = Span<const FrameGraphResourceCreation>( node_outputs.data, node_outputs.size );

        name_value = pass.value( "name", "" );
        RASSERT( !name_value.empty() );

        bool enabled = pass.value( "enabled", true );

        node_creation.name = string_buffer.append_use_f( "%s", name_value.c_str() );
        node_creation.enabled = enabled;

        FrameGraphNodeHandle node_handle = builder->create_node( node_creation );
        all_nodes.push( node_handle );
    }

    temp_allocator->free_marker( current_allocator_marker );
}

static void compute_edges( FrameGraph* frame_graph, FrameGraphNode* node, u32 node_index ) {

    FrameGraphNodeHandle node_handle = frame_graph->all_nodes[ node_index ];

    for ( u32 r = 0; r < node->inputs.size; ++r ) {
        FrameGraphResource* resource = frame_graph->access_resource( node->inputs[ r ] );

        {
            FrameGraphResource* output_resource = frame_graph->get_resource( resource->name );
            if ( output_resource == nullptr && !resource->resource_info.external ) {
                // TODO(marco): external resources
                rprint( "Requested resource %s is not produced by any node and is not external.\n", resource->name );
                continue;
            }

            resource->producer = output_resource->producer;
            resource->resource_info = output_resource->resource_info;
            resource->output_handle = output_resource->output_handle;
        }

        for ( u32 n = 0; n < frame_graph->all_nodes.size; ++n ) {
            if ( n == node_index ) {
                continue;
            }

            FrameGraphNodeHandle parent_handle = frame_graph->all_nodes[ n ];
            FrameGraphNode* producer_node = frame_graph->access_node( parent_handle );

            for ( u32 o = 0; o < producer_node->outputs.size; ++o ) {
                FrameGraphResource* output_resource = frame_graph->access_resource( producer_node->outputs[ o ] );

                if ( strcmp( resource->name, output_resource->name ) != 0 ) {
                    continue;
                }

#if FRAME_GRAPH_DEBUG
                rprint( "Adding edge for resource %s from %s [%d] to %s [%d]\n", output_resource->name, producer_node->name, n, node->name, node_index )
#endif

                producer_node->edges.push( node_handle );
            }
        }
    }
}

static void compute_edges_v2( FrameGraph* frame_graph, FrameGraphNodeHandle node_handle ) {
    FrameGraphNode* node = frame_graph->access_node( node_handle );

    for ( u32 r = 0; r < node->inputs.size; ++r ) {
        FrameGraphResource* resource = frame_graph->access_resource( node->inputs[ r ] );
        RASSERT( resource );

        FrameGraphNode* producer_node = frame_graph->access_node( resource->producer );
        RASSERTM( producer_node != node, "Node %s reads resource %s that it also produces", node->name, resource->name );

#if FRAME_GRAPH_DEBUG
        rprint( "Adding edge for resource %s from %s [%d] to %s [%d]\n", resource->name, producer_node->name, r, node->name, node_handle )
#endif

        producer_node->edges.push( node_handle );
    }
}

static void cache_node_output_textures( FrameGraph* frame_graph, FrameGraphNode* node ) {

    u32 width = 0;
    u32 height = 0;
    f32 scale_width = 0.f;
    f32 scale_height = 0.f;

    node->output_image_views.clear();
    node->output_depth_image_view = ImageViewHandle();
    node->output_shading_rate = ImageViewHandle();

    for ( u32 r = 0; r < node->outputs.size; ++r ) {
        FrameGraphResource* resource = frame_graph->access_resource( node->outputs[ r ] );
        FrameGraphResourceType output_type = resource->type;
        if ( node->version == 2 ) {
            resource = frame_graph->access_output_resource( node->outputs[ r ] );
        }

        FrameGraphResourceInfo& info = resource->resource_info;

        if ( output_type != FrameGraphResourceType_Attachment ) {
            continue;
        }

        if ( width == 0 ) {
            width = info.texture.width;
            scale_width = info.texture.scale_width > 0.f ? info.texture.scale_width : 1.f;
        } else {
            RASSERT( width == info.texture.width );
        }

        if ( height == 0 ) {
            height = info.texture.height;
            scale_height = info.texture.scale_height > 0.f ? info.texture.scale_height: 1.f;
        } else {
            RASSERT( height == info.texture.height );
        }

        if ( TextureFormat::has_depth( info.texture.format ) ) {
            node->output_depth_image_view = info.texture.image_view;
        } else {
            node->output_image_views.push( info.texture.image_view );
        }
    }

    for ( u32 r = 0; r < node->inputs.size; ++r ) {
        FrameGraphResource* input_resource = frame_graph->access_output_resource( node->inputs[ r ] );

        FrameGraphResourceType input_type = node->version == 2 ? node->temp_inputs_type[ r ] : input_resource->type;

        if ( input_type != FrameGraphResourceType_Attachment && input_type != FrameGraphResourceType_ShadingRate ) {
            continue;
        }

        FrameGraphResource* resource = frame_graph->get_resource( input_resource->name );
        if ( node->version == 2 ) {
            resource = frame_graph->access_output_resource( resource->output_handle );
        }

        if ( resource == nullptr ) {
            continue;
        }

        FrameGraphResourceInfo& info = resource->resource_info;

        input_resource->resource_info.texture.image_view = info.texture.image_view;

        if ( width == 0 ) {
            width = info.texture.width;
            scale_width = info.texture.scale_width > 0.f ? info.texture.scale_width : 1.f;
        } else if ( input_type != FrameGraphResourceType_ShadingRate ) {
            RASSERT( width == info.texture.width );
        }

        if ( height == 0 ) {
            height = info.texture.height;
            scale_height = info.texture.scale_height > 0.f ? info.texture.scale_height : 1.f;
        } else if ( input_type != FrameGraphResourceType_ShadingRate ) {
            RASSERT( height == info.texture.height );
        }

        if ( input_type == FrameGraphResourceType_Texture ) {
            continue;
        }

        if ( resource->type == FrameGraphResourceType_ShadingRate ) {
            node->output_shading_rate = info.texture.image_view;

            continue;
        }

        if ( TextureFormat::has_depth( info.texture.format ) ) {
            node->output_depth_image_view = info.texture.image_view;
        } else {
            node->output_image_views.push( info.texture.image_view );
        }
    }

    node->resolution_scale_width = scale_width;
    node->resolution_scale_height = scale_height;
}

static void cache_node_render_pass_output( FrameGraph* frame_graph, FrameGraphNode* node ) {
    RenderPassOutput& render_pass_output = node->render_pass_output;
    render_pass_output = {};

    // NOTE(marco): first create the outputs, then we can patch the input resources
    // with the right handles
    for ( u32 i = 0; i < node->outputs.size; ++i ) {
        FrameGraphResource* output_resource = frame_graph->access_resource( node->outputs[ i ] );
        FrameGraphResourceType output_type = output_resource->type;

        if ( node->version == 2 ) {
            output_resource = frame_graph->access_output_resource( node->outputs[ i ] );
        }

        FrameGraphResourceInfo& info = output_resource->resource_info;

        if ( output_type == FrameGraphResourceType_Attachment ) {
            if ( TextureFormat::has_depth( info.texture.format ) ) {
                render_pass_output.depth_stencil_format = info.texture.format;
                render_pass_output.depth_stencil_final_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                render_pass_output.depth_operation = VK_ATTACHMENT_LOAD_OP_CLEAR;
            } else {
                u32 rt_index = render_pass_output.num_color_formats;
                ++render_pass_output.num_color_formats;

                render_pass_output.color_formats[ rt_index ] = info.texture.format;
                render_pass_output.color_operations[ rt_index ] = info.texture.load_op;
                render_pass_output.color_final_layouts[ rt_index ] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }
        }
    }

    for ( u32 i = 0; i < node->inputs.size; ++i ) {
        FrameGraphResource* input_resource = frame_graph->access_resource( node->inputs[ i ] );
        if ( input_resource->type == FrameGraphResourceType_Reference ) {
            input_resource = frame_graph->access_resource( input_resource->output_handle );
        }

        FrameGraphResourceInfo& info = input_resource->resource_info;

        FrameGraphResourceType input_type = node->version == 2 ? node->temp_inputs_type[ i ] : input_resource->type;

        if ( input_type == FrameGraphResourceType_Attachment ) {
            if ( TextureFormat::has_depth( info.texture.format ) ) {
                render_pass_output.depth_stencil_format = info.texture.format;
                render_pass_output.depth_stencil_final_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                render_pass_output.depth_operation = VK_ATTACHMENT_LOAD_OP_LOAD;
            } else {
                u32 rt_index = render_pass_output.num_color_formats;
                ++render_pass_output.num_color_formats;

                render_pass_output.color_formats[ rt_index ] = info.texture.format;
                render_pass_output.color_operations[ rt_index ] = VK_ATTACHMENT_LOAD_OP_LOAD;
                render_pass_output.color_final_layouts[ rt_index ] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }
        }
    }

    // TODO(marco): make sure formats are valid for attachment
    // TODO(gabriel): see what is needed here.
    //node->render_pass_c = render_pass_creation;
}

void FrameGraph::enable_render_pass( cstring render_pass_name ) {
    FrameGraphNode* node = builder->get_node( render_pass_name );
    node->enabled = true;
}

void FrameGraph::disable_render_pass( cstring render_pass_name ) {
    FrameGraphNode* node = builder->get_node( render_pass_name );
    node->enabled = false;
}

namespace FrameGraphNodeVisitStatus {
    enum Enum {
        New = 0, Visited, Added, Count
    }; // enum Enum
}; // namespace FrameGraphNodeVisitStatus

void FrameGraph::compile() {
    // TODO(marco)
    // - cull inactive nodes

    for ( u32 i = 0; i < all_nodes.size; ++i ) {
        FrameGraphNode* node = builder->access_node( all_nodes[ i ] );

        // NOTE(marco): we want to clear all edges first, then populate them. If we clear them inside the loop
        // below we risk clearing the list after it has already been used by one of the child nodes
        node->edges.clear();
    }

    for ( u32 i = 0; i < all_nodes.size; ++i ) {
        FrameGraphNode* node = builder->access_node( all_nodes[ i ] );
        if ( !node->enabled ) {
            continue;
        }

        if ( node->version == 2 ) {
            compute_edges_v2( this, all_nodes[ i ] );
        } else {
            compute_edges( this, node, i );
        }
    }

    Array<FrameGraphNodeHandle> sorted_nodes;
    sorted_nodes.init( &local_allocator, all_nodes.size );

    Array<u8> node_status;
    node_status.init( &local_allocator, all_nodes.size, all_nodes.size );
    memset( node_status.data, 0, sizeof( u8 ) * all_nodes.size );

    Array<FrameGraphNodeHandle> stack;
    stack.init( &local_allocator, nodes.size );

    // Topological sorting
    for ( u32 n = 0; n < all_nodes.size; ++n ) {
        FrameGraphNode* node = builder->access_node( all_nodes[ n ] );
        if ( !node->enabled ) {
            continue;
        }

        stack.push( all_nodes[ n ] );

        while ( stack.size > 0 ) {
            FrameGraphNodeHandle node_handle = stack.back();

            if ( node_status[ node_handle.index ] == FrameGraphNodeVisitStatus::Added ) {
                stack.pop();

                continue;
            }

            if ( node_status[ node_handle.index ]  == FrameGraphNodeVisitStatus::Visited ) {
                node_status[ node_handle.index ] = FrameGraphNodeVisitStatus::Added;

                sorted_nodes.push( node_handle );

                stack.pop();

                continue;
            }

            node_status[ node_handle.index ] = FrameGraphNodeVisitStatus::Visited;

            FrameGraphNode* node = builder->access_node( node_handle );

            // Leaf node
            if ( node->edges.size == 0 ) {
                continue;
            }

            for ( u32 r = 0; r < node->edges.size; ++r ) {
                FrameGraphNodeHandle child_handle = node->edges[ r ];

                if ( node_status[ child_handle.index ] == FrameGraphNodeVisitStatus::New ) {
                    stack.push( child_handle );
                }
            }
        }
    }

    nodes.clear();
    persistent_update_nodes.clear();

    for ( i32 i = sorted_nodes.size - 1; i >= 0; --i ) {
#if FRAME_GRAPH_DEBUG
        FrameGraphNode* node = builder->access_node( sorted_nodes[ i ] );
        rprint( "Node %s is at position %d\n", node->name, nodes.size );
#endif

        nodes.push( sorted_nodes[ i ] );
    }

    node_status.shutdown();
    stack.shutdown();
    sorted_nodes.shutdown();

    // NOTE(marco): allocations and deallocations are used for verification purposes only
    u32 resource_count = builder->resource_cache.resources.used_indices;
    Array<FrameGraphNodeHandle> allocations;
    allocations.init( &local_allocator, resource_count, resource_count );
    for ( u32 i = 0; i < resource_count; ++i) {
        allocations[ i ].index = k_invalid_index;
    }

    Array<FrameGraphNodeHandle> deallocations;
    deallocations.init( &local_allocator, resource_count, resource_count );
    for ( u32 i = 0; i < resource_count; ++i) {
        deallocations[ i ].index = k_invalid_index;
    }

    Array<ImageHandle> free_list;
    free_list.init( &local_allocator, resource_count );

    size_t peak_memory = 0;
    size_t instant_memory = 0;

    for ( u32 i = 0; i < nodes.size; ++i ) {
        FrameGraphNode* node = builder->access_node( nodes[ i ] );
        if ( !node->enabled ) {
            continue;
        }

        for ( u32 j = 0; j < node->inputs.size; ++j ) {
            FrameGraphResource* input_resource = builder->access_resource( node->inputs[ j ] );
            FrameGraphResource* resource = builder->access_resource( input_resource->output_handle );

            if ( resource == nullptr ) {
                continue;
            }

            resource->ref_count++;
        }
    }

    // Create resources
    for ( u32 i = 0; i < nodes.size; ++i ) {
        FrameGraphNode* node = builder->access_node( nodes[ i ] );
        if ( !node->enabled ) {
            continue;
        }

        bool add_to_persistent_nodes = false;

        for ( u32 j = 0; j < node->outputs.size; ++j ) {
            FrameGraphResourceHandle output_handle = node->outputs[ j ];
            u32 resource_index = output_handle.index;
            FrameGraphResource* resource = builder->access_resource( output_handle );

            if ( !resource->resource_info.external && allocations[ resource_index ].index == k_invalid_index ) {
                RASSERT( deallocations[ resource_index ].index == k_invalid_index )
                allocations[ resource_index ] = nodes[ i ];

                if ( resource->type == FrameGraphResourceType_Attachment ) {
                    if ( resource->output_handle.index != output_handle.index ) {
                        // This resource has already been allocated by the first node that produced it
                        continue;
                    }

                    FrameGraphResourceInfo& info = resource->resource_info;

                    // Resolve texture size if needed
                    if ( info.texture.width == 0 || info.texture.height == 0 ) {

                        if ( builder->resolution_info ) {
                            info.texture.width = u32( builder->resolution_info->render_width * info.texture.scale_width );
                            info.texture.height = u32( builder->resolution_info->render_height * info.texture.scale_height );
                        }
                        else {
                            info.texture.width = u32( builder->device->swapchain_width * info.texture.scale_width );
                            info.texture.height = u32( builder->device->swapchain_height * info.texture.scale_height );
                        }
                    }

                    TextureFlags::Mask texture_creation_flags = info.texture.compute ? ( TextureFlags::Mask )(TextureFlags::RenderTarget_mask | TextureFlags::Compute_mask) : TextureFlags::RenderTarget_mask;

                    bool found_suitable_free_resource = false;
                    // Avoid memory aliasing for persistent resources
                    if ( free_list.size > 0 && !info.texture.persistent ) {
                        for ( u32 r = 0; r < free_list.size; ++r ) {
                            ImageHandle alias_image_handle = free_list[ r ];
                            Image* alias_image = builder->device->get_image( alias_image_handle );

                            if ( alias_image->width != info.texture.width ||
                                 alias_image->height != info.texture.height ||
                                 alias_image->vk_format != info.texture.format ) {
                                continue;
                            }

                            // NOTE(marco): this texture has already been aliased, get original image
                            if ( alias_image->alias_image.is_valid() ) {
                                alias_image_handle = alias_image->alias_image;
                                alias_image = builder->device->get_image( alias_image_handle );
                            }

                            ImageCreation texture_creation{ };
                            texture_creation.set_data( nullptr ).set_alias( alias_image_handle ).set_name( resource->name ).set_format_type( info.texture.format, TextureType::Enum::Texture2D ).set_size( info.texture.width, info.texture.height, info.texture.depth ).set_flags( texture_creation_flags );
                            ImageHandle handle = builder->device->create_image( texture_creation );

                            VkImageAspectFlags image_view_flag = TextureFormat::has_depth( texture_creation.format ) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;

                            ImageViewHandle view_handle = builder->device->create_image_view( {
                                .parent_image = handle,
                                .view_type = to_vk_image_view_type( texture_creation.type ),
                                .sub_resource = { image_view_flag, 0, 1, 0, 1 },
                                .name = resource->name } );

                            builder->device->add_image_view_to_bindless( view_handle );

                            info.texture.image = handle;
                            info.texture.image_view = view_handle;

                            free_list.delete_swap( r );
                            found_suitable_free_resource = true;
                            break;
                        }
                    }

                    if ( !found_suitable_free_resource ) {
                        ImageCreation image_creation{ };
                        image_creation.set_data( nullptr ).set_name( resource->name ).set_format_type( info.texture.format, TextureType::Enum::Texture2D ).set_size( info.texture.width, info.texture.height, info.texture.depth ).set_flags( texture_creation_flags );
                        ImageHandle handle = builder->device->create_image( image_creation );

                        VkImageAspectFlags image_view_flag = TextureFormat::has_depth( image_creation.format ) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;

                        ImageViewCreation image_view_creation = {
                            .parent_image = handle,
                            .view_type = to_vk_image_view_type( image_creation.type ),
                            .sub_resource = { image_view_flag, 0, 1, 0, 1 },
                            .name = resource->name };
                        ImageViewHandle view_handle = builder->device->create_image_view( image_view_creation );

                        builder->device->add_image_view_to_bindless( view_handle );

                        info.texture.image = handle;
                        info.texture.image_view = view_handle;

                        // Add previous image and image view
                        if ( info.texture.persistent ) {
                            image_creation.name = builder->rename_buffer.append_use_f( "%s_previous", resource->name );
                            info.texture.previous_image = builder->device->create_image( image_creation );

                            image_view_creation.parent_image = info.texture.previous_image;
                            image_view_creation.name = image_creation.name;
                            info.texture.previous_image_view = builder->device->create_image_view( image_view_creation );

                            builder->device->add_image_view_to_bindless( info.texture.previous_image_view );

                            add_to_persistent_nodes = true;
                        }
                    }
                }

#if FRAME_GRAPH_DEBUG
                rprint( "Output %s allocated on node %d\n", resource->name, nodes[ i ].index );
#endif
            }
        }

        for ( u32 j = 0; j < node->inputs.size; ++j ) {
            FrameGraphResource* input_resource = builder->access_resource( node->inputs[ j ] );

            u32 resource_index = input_resource->output_handle.index;
            FrameGraphResource* resource = builder->access_resource( input_resource->output_handle );

            if ( resource == nullptr ) {
                continue;
            }

            resource->ref_count--;

            if ( resource->resource_info.texture.persistent ) {
                add_to_persistent_nodes = true;
            }

            if ( !resource->resource_info.external && resource->ref_count == 0 ) {
                RASSERT( deallocations[ resource_index ].index == k_invalid_index );
                deallocations[ resource_index ] = nodes[ i ];

                if ( (resource->type == FrameGraphResourceType_Attachment || resource->type == FrameGraphResourceType_Texture) &&
                     !(resource->resource_info.texture.persistent || resource->resource_info.texture.disable_memory_aliasing ) ) {

                    ImageViewHandle image_view = resource->resource_info.texture.image_view;
                    ImageView* view = builder->device->get_image_view( image_view );
                    Image* image = builder->device->get_image( view->parent_image );

                    free_list.push( image->handle );
                }

#if FRAME_GRAPH_DEBUG
                rprint( "Output %s deallocated on node %d\n", resource->name, nodes[ i ].index );
#endif
            }
        }


        if ( add_to_persistent_nodes ) {
            persistent_update_nodes.push( nodes[ i ] );
        }
    }

    allocations.shutdown();
    deallocations.shutdown();
    free_list.shutdown();

    // Cache output and textures used to begin pass command
    for ( u32 i = 0; i < nodes.size; ++i ) {
        FrameGraphNode* node = builder->access_node( nodes[ i ] );
        RASSERT( node->enabled );

        if ( node->compute ) {
            continue;
        }

        if ( !node->render_pass_output_cached ) {
            cache_node_render_pass_output( this, node );
            node->render_pass_output_cached = true;
        }

        if ( !node->output_textures_cached ) {
            cache_node_output_textures( this, node );
            node->output_textures_cached = true;
        }
    }

    // This should not be needed here, but just in case: free memory
    for ( u32 i = 0; i < batches.size; i++ ) {
        FrameGraphExecutionBatch& batch = batches[ i ];
        batch.nodes.shutdown();
    }
    batches.clear();

    // Calculate batches
    for ( u32 i = 0; i < nodes.size; ++i ) {
        FrameGraphNode* node = builder->access_node( nodes[ i ] );
        RASSERT( node->enabled );

        // Search for corresponding batch
        FrameGraphExecutionBatch* batch = nullptr;
        for ( u32 b = 0; b < batches.size; ++b ) {
            FrameGraphExecutionBatch* current_batch = &batches[ b ];

            if ( current_batch->scheduling.queue_type == node->scheduling.queue_type &&
                 current_batch->scheduling.phase == node->scheduling.phase ) {
                batch = current_batch;
                break;
            }
        }

        // If none found, add a new one
        if ( batch == nullptr ) {
            batch = &batches.push_use();

            batch->scheduling = node->scheduling;
            batch->nodes.init( allocator, 8 );
            batch->cb = nullptr;
        }

        // Add node to the batch
        RASSERT( batch );

        batch->nodes.push( nodes[ i ] );
    }
}

void FrameGraph::add_ui() {
    for ( u32 n = 0; n < nodes.size; ++n ) {
        FrameGraphNode* node = builder->access_node( nodes[ n ] );
        RASSERT( node->enabled );

        node->graph_render_pass->add_ui();
    }
}

void FrameGraph::render( u32 frame_index, u32 thread_index,
                         Renderer* renderer, RenderView* render_view,
                         RenderBlackboard* render_blackboard,
                         RenderConfig* render_config ) {

    RASSERTM( per_frame_persistent_update_called == true, "Forgot to call update_persistent_resources_handles after this method in previous frame!" );
    RenderScene* render_scene = render_view->scene;
    GpuProfiler* gpu_profiler = renderer->gpu->gpu_profiler;

    // Allocate and begin command buffers
    for ( u32 b = 0; b < batches.size; ++b ) {

        FrameGraphExecutionBatch& batch = batches[ b ];
        batch.cb = renderer->gpu->allocate_command_buffer( thread_index, frame_index, batch.scheduling.queue_type );

        batch.cb->begin();
        gpu_profiler->begin_command_buffer( batch.cb );

        // Frame Graph Marker
        batch.cb->push_marker( "Frame Graph" );
    }

    for ( u32 b = 0; b < batches.size; ++b ) {

        FrameGraphExecutionBatch& batch = batches[ b ];

        for ( u32 n = 0; n < batch.nodes.size; ++n ) {
            ZoneScopedN( "RenderPass" );

            FrameGraphNode* node = builder->access_node( batch.nodes[ n ] );
            RASSERT( node->enabled );

            CommandBuffer* cb = batch.cb;
            FrameGraphRenderContext render_context{ renderer, cb, this, render_view, render_blackboard, render_config, frame_index };

            //rprint( "%s\n", node->name );
            cb->push_marker( node->name );

            // Compute Node
            if ( node->compute ) {

                for ( u32 i = 0; i < node->inputs.size; ++i ) {
                    FrameGraphResource* input_resource = builder->access_resource( node->inputs[ i ] );
                    FrameGraphResource* resource = builder->access_resource( input_resource->output_handle );

                    if ( resource == nullptr || resource->resource_info.external ) {
                        continue;
                    }

                    if ( input_resource->type == FrameGraphResourceType_Texture ) {
                        Image* texture = cb->gpu_device->get_image( resource->resource_info.texture.image );

                        const VkImageAspectFlags aspect = TextureFormat::has_depth( texture->vk_format ) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;

                        cb->add_image_barrier( texture->handle, range_aspect( aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS ),
                                               { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                                 VK_ACCESS_2_SHADER_READ_BIT,
                                               VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL } );
                        //util_add_image_barrier( cb->gpu_device, cb->vk_command_buffer, texture, RESOURCE_STATE_SHADER_RESOURCE, 0, 1, TextureFormat::has_depth(texture->vk_format) );
                    } else if ( input_resource->type == FrameGraphResourceType_Attachment ) {
                        //// TODO: what to do with attachments ?
                        //Texture* texture = cb->gpu_device->access_texture( resource->resource_info.texture.handle );
                        //texture = texture;
                    }
                }

                for ( u32 o = 0; o < node->outputs.size; ++o ) {
                    FrameGraphResource* resource = builder->access_resource( node->outputs[ o ] );

                    if ( resource->type == FrameGraphResourceType_Attachment ) {
                        Image* texture = cb->gpu_device->get_image( resource->resource_info.texture.image );

                        if ( TextureFormat::has_depth( texture->vk_format ) ) {
                            // Is this supported even ?
                            RASSERT( false );
                        } else {
                            cb->add_image_barrier( texture->handle, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS ),
                                                   { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                                     VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
                                                     VK_IMAGE_LAYOUT_GENERAL } );
                            //util_add_image_barrier( cb->gpu_device, cb->vk_command_buffer, texture, RESOURCE_STATE_UNORDERED_ACCESS, 0, 1, false );
                        }
                    }
                }

                cb->flush_barriers();

                node->graph_render_pass->pre_render( render_context );
                node->graph_render_pass->render( render_context );
                node->graph_render_pass->post_render( render_context );

            } else if ( node->ray_tracing ) {
                // Ray-Tracing Node
                node->graph_render_pass->pre_render( render_context );
                node->graph_render_pass->render( render_context );
                node->graph_render_pass->post_render( render_context );

            } else {
                // Graphics Node
                u32 width = 0;
                u32 height = 0;

                node->graph_render_pass->pre_render( render_context );

                // Add input barriers
                for ( u32 i = 0; i < node->inputs.size; ++i ) {
                    FrameGraphResource* input_resource = builder->access_resource( node->inputs[ i ] );
                    FrameGraphResource* resource = builder->access_resource( input_resource->output_handle );

                    if ( resource == nullptr || resource->resource_info.external ) {
                        continue;
                    }

                    FrameGraphResourceType input_type = node->version == 2 ? node->temp_inputs_type[ i ] : input_resource->type;

                    if ( input_type == FrameGraphResourceType_Texture ) {
                        ImageView* image_view = cb->gpu_device->get_image_view( resource->resource_info.texture.image_view );
                        Image* texture = cb->gpu_device->get_image( resource->resource_info.texture.image );

                        const VkImageAspectFlags aspect = TextureFormat::has_depth( texture->vk_format ) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
                        cb->add_image_barrier( texture->handle, range_aspect( aspect, 0, 1, 0, 1 ),
                                               { VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                                 VK_ACCESS_2_SHADER_READ_BIT,
                                                 VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL } );
                        //util_add_image_barrier( cb->gpu_device, cb->vk_command_buffer, texture, RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 0, 1, TextureFormat::has_depth( texture->vk_format ) );
                    } else if ( input_type == FrameGraphResourceType_Attachment ) {
                        ImageView* image_view = cb->gpu_device->get_image_view( resource->resource_info.texture.image_view );
                        Image* texture = cb->gpu_device->get_image( resource->resource_info.texture.image );

                        width = texture->width;
                        height = texture->height;

                        // For textures that are read-write check if a transition is needed.
                        if ( !TextureFormat::has_depth_or_stencil( texture->vk_format ) ) {
                            cb->add_image_barrier( texture->handle, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 ),
                                                   { VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
                                                     VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL } );
                            //util_add_image_barrier( cb->gpu_device, cb->vk_command_buffer, texture, RESOURCE_STATE_RENDER_TARGET, 0, 1, false );
                        } else {
                            cb->add_image_barrier( texture->handle, range_aspect( VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 ),
                                                   { VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                                                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                                                     VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL } );
                            //util_add_image_barrier( cb->gpu_device, cb->vk_command_buffer, texture, RESOURCE_STATE_DEPTH_WRITE, 0, 1, true );
                        }
                    }
                }

                cb->flush_barriers();

                // Add output barriers and collect pass data
                ArenaAllocator* temp_allocator = MemoryService::instance()->get_thread_allocator();
                u64 marker = temp_allocator->get_marker();

                for ( u32 o = 0; o < node->outputs.size; ++o ) {
                    FrameGraphResource* resource = builder->access_resource( node->outputs[ o ] );

                    if ( resource->type == FrameGraphResourceType_Attachment ) {
                        if ( node->version == 2 ) {
                            for ( u32 i = 0; i < node->inputs.size; ++i ) {
                                FrameGraphResource* input_resource = builder->access_resource( node->inputs[ i ] );
                                if ( input_resource->output_handle.index == resource->output_handle.index ) {
                                    // This resource is also an input, skip
                                    continue;
                                }
                            }
                        }

                        ImageView* image_view = cb->gpu_device->get_image_view( resource->resource_info.texture.image_view );
                        Image* texture = cb->gpu_device->get_image( resource->resource_info.texture.image );

                        width = texture->width;
                        height = texture->height;

                        // Depth texture
                        if ( TextureFormat::has_depth( texture->vk_format ) ) {
                            cb->add_image_barrier( texture->handle, range_aspect( VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 ),
                                                   { VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                                                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                                                     VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL } );
                            //util_add_image_barrier( cb->gpu_device, cb->vk_command_buffer, texture, RESOURCE_STATE_DEPTH_WRITE, 0, 1, true );
                            cb->flush_barriers();
                            f32* clear_color = resource->resource_info.texture.clear_values;
                            cb->clear_depth_stencil( clear_color[ 0 ], (u8)clear_color[ 1 ] );

                        } else {
                            // Color texture
                            cb->add_image_barrier( texture->handle, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 ),
                                                   { VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
                                                     VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL } );
                            //util_add_image_barrier( cb->gpu_device, cb->vk_command_buffer, texture, RESOURCE_STATE_RENDER_TARGET, 0, 1, false );
                            cb->flush_barriers();
                            f32* clear_color = resource->resource_info.texture.clear_values;
                            cb->clear( clear_color[ 0 ], clear_color[ 1 ], clear_color[ 2 ], clear_color[ 3 ], o );
                        }
                    }
                }

                /*Rect2DInt scissor{ 0, 0,( u16 )width, ( u16 )height };
                cb->set_scissor( &scissor );

                Viewport viewport{ };
                viewport.rect = { 0, 0, ( u16 )width, ( u16 )height };
                viewport.min_depth = 0.0f;
                viewport.max_depth = 1.0f;

                cb->set_viewport( &viewport );*/

                if ( cb->gpu_device->fragment_shading_rate_present ) {
                    cb->set_shading_rate( 1, 1 );
                }

                // Cache needed spans
                const u32 num_textures = node->output_image_views.size;
                Span<const ImageViewHandle> rts( node->output_image_views.data, num_textures );
                Span<const VkAttachmentLoadOp> loads( node->render_pass_output.color_operations, num_textures );
                Span<const VkClearValue> clear_colors( cb->clear_values, num_textures );

                cb->begin_render_pass( rts, loads, clear_colors,
                                       node->output_depth_image_view,
                                       node->render_pass_output.depth_operation,
                                       cb->clear_values[ CommandBuffer::k_depth_stencil_clear_index ],
                                       node->output_shading_rate,
                                       1, node->render_pass_output.multiview_mask );

                cb->set_fullscreen_scissor();
                cb->set_fullscreen_viewport();

                node->graph_render_pass->render( render_context );

                cb->end_render_pass();

                node->graph_render_pass->post_render( render_context );
            }

            cb->pop_marker();
            //gpu_profiler->pop_timestamp( cb );
        }
    }

    for ( u32 b = 0; b < batches.size; ++b ) {

        FrameGraphExecutionBatch& batch = batches[ b ];

        //gpu_profiler->pop_timestamp( batch.cb );
        batch.cb->pop_marker();

        gpu_profiler->end_command_buffer( batch.cb );
        batch.cb->end();
    }
}

void FrameGraph::on_resize( Renderer* renderer, RenderBlackboard* render_blackboard,
                            RenderConfig* render_config, u32 new_width, u32 new_height ) {

    GpuDevice& gpu = *renderer->gpu;

    for ( u32 n = 0; n < nodes.size; ++n ) {
        FrameGraphNode* node = builder->access_node( nodes[ n ] );
        RASSERT( node->enabled );

        for ( u32 r = 0; r < node->outputs.size; ++r ) {
            FrameGraphResource* resource = access_resource( node->outputs[ r ] );

            FrameGraphResourceInfo& info = resource->resource_info;

            // Avoid attachments or external resources
            if ( resource->type != FrameGraphResourceType_Attachment ||
                 resource->resource_info.external ) {
                continue;
            }

            if ( info.texture.scale_height == 0.0f || info.texture.scale_width == 0.0f ) {
                info.texture.width = new_width;
                info.texture.height = new_height;
            } else {
                info.texture.width = u32( new_width * info.texture.scale_width );
                info.texture.height = u32( new_height * info.texture.scale_height );
            }

            gpu.resize_image( info.texture.image, info.texture.width, info.texture.height );
            gpu.recreate_image_view( info.texture.image_view );
            gpu.add_image_view_to_bindless( info.texture.image_view );

            if ( info.texture.persistent ) {
                gpu.resize_image( info.texture.previous_image, info.texture.width, info.texture.height );
                gpu.recreate_image_view( info.texture.previous_image_view );
                gpu.add_image_view_to_bindless( info.texture.previous_image_view );
            }
        }

        FrameGraphResourceContext resource_context{ renderer, this, render_blackboard, render_config, nullptr };
        node->graph_render_pass->on_resize( resource_context, new_width, new_height );
    }
}

void FrameGraph::reload_shaders( RenderScene& scene, RenderBlackboard* render_blackboard, RenderConfig* render_config ) {

    FrameGraphResourceContext resource_context{ scene.renderer, this, render_blackboard, render_config, &scene };

    for ( u32 n = 0; n < nodes.size; ++n ) {
        FrameGraphNode* node = builder->access_node( nodes[ n ] );
        RASSERT( node->enabled );

        node->graph_render_pass->update_psos( resource_context, PipelineUpdatePhase::Reload );
    }
}

void FrameGraph::update_persistent_resources_handles() {

    RASSERT( per_frame_persistent_update_called );

    for ( u32 i = 0; i < persistent_update_nodes.size; i++ ) {
        FrameGraphNode* node = builder->access_node( persistent_update_nodes[ i ] );

        // Flip current/previous image and image view nodes
        for ( u32 j = 0; j < node->outputs.size; ++j ) {
            FrameGraphResourceHandle output_handle = node->outputs[ j ];
            FrameGraphResource* resource = builder->access_resource( output_handle );
            if ( !resource->resource_info.texture.persistent ) {
                continue;
            }

            FrameGraphResourceInfo& info = resource->resource_info;
            // Cache what is now "previous"
            ImageHandle previous_image = info.texture.image;
            ImageViewHandle previous_image_view = info.texture.image_view;
            // Flip current with previous
            info.texture.image = info.texture.previous_image;
            info.texture.image_view = info.texture.previous_image_view;

            info.texture.previous_image = previous_image;
            info.texture.previous_image_view = previous_image_view;
        }

        cache_node_output_textures( this, node );
    }

    per_frame_persistent_update_called = true;
}

CommandBuffer* FrameGraph::get_command_buffer_from_batch( CommandQueueType queue, u32 phase ) {

    for ( u32 b = 0; b < batches.size; ++b ) {
        FrameGraphExecutionBatch& batch = batches[ b ];

        if ( batch.scheduling.queue_type == queue &&
             batch.scheduling.phase == phase ) {
            return batch.cb;
        }
    }

    RASSERT( false );
    return nullptr;
}

void FrameGraph::debug_ui() {

    if ( ImGui::CollapsingHeader( "Nodes" ) ) {
        for ( u32 n = 0; n < nodes.size; ++n ) {
            FrameGraphNode* node = builder->access_node( nodes[ n ] );

            ImGui::Separator();
            ImGui::Text( "Pass: %s", node->name );
            static cstring node_types_names[] = { "Graphics", "Compute", "Ray Tracing" };
            static cstring node_queue_names[] = { "Graphics", "Compute" };
            ImGui::Text( "\tType: %s", node_types_names[ node->compute ? ( node->ray_tracing ? 2 : 1 ) : 0 ] );
            ImGui::Text( "\tQueue: %s", node_queue_names[ node->scheduling.queue_type == CommandQueueType::Compute ? 1 : 0 ] );

            ImGui::Text( "\tInputs" );
            for ( u32 i = 0; i < node->inputs.size; ++i ) {
                FrameGraphResource* resource = access_resource( node->inputs[ i ] );
                FrameGraphResource* output_resource = access_output_resource( node->inputs[ i ] );
                ImGui::Text( "\t\t%s %u %u", resource->name, output_resource->resource_info.texture.image_view.index(), output_resource->resource_info.buffer.handle.index() );
            }


            ImGui::Text( "\tOutputs" );
            for ( u32 o = 0; o < node->outputs.size; ++o ) {
                FrameGraphResource* resource = builder->access_resource( node->outputs[ o ] );
                FrameGraphResource* output_resource = access_output_resource( node->outputs[ o ] );
                ImGui::Text( "\t\t%s %u %u", resource->name, output_resource->resource_info.texture.image_view.index(), output_resource->resource_info.buffer.handle.index() );
            }
        }
    }
}

void FrameGraph::add_node( FrameGraphNodeCreation& creation ) {
    FrameGraphNodeHandle handle = builder->create_node( creation );
    all_nodes.push( handle );
}

void FrameGraph::add_node_v2( const FrameGraphNodeCreation_v2& creation ) {
    FrameGraphNodeHandle handle = builder->create_node_v2( creation );
    all_nodes.push( handle );
}

FrameGraphNode* FrameGraph::get_node( cstring name ) {
    return builder->get_node( name );
}

FrameGraphNode* FrameGraph::access_node( FrameGraphNodeHandle handle ) {
    return builder->access_node( handle );
}

void FrameGraph::cache_render_pass_output( cstring node_name, GpuDevice* gpu, RenderPassOutput& render_pass_output, bool compute_node ) {
    FrameGraphNode* node = node_name ? get_node( node_name ) : nullptr;
    if ( node ) {

        // TODO: handle better
        if ( strcmp( node_name, "swapchain" ) == 0 ) {
            render_pass_output = gpu->get_swapchain_output();
        } else if ( compute_node ) {
            render_pass_output = gpu->get_swapchain_output();
        } else {
            render_pass_output = node->render_pass_output;
        }
    } else {
        rprint( "Cannot find render pass %s. Defaulting to swapchain\n", node_name );
        render_pass_output = gpu->get_swapchain_output();
    }
}

void FrameGraph::add_resource( cstring name, FrameGraphResourceType type, FrameGraphResourceInfo resource_info ) {
    builder->add_resource( name, type, resource_info );
}

FrameGraphResource* FrameGraph::get_resource( cstring name ) {
    return builder->get_resource( name );
}

FrameGraphResource* FrameGraph::access_resource( FrameGraphResourceHandle handle ) {
    return builder->access_resource( handle );
}

FrameGraphResource* FrameGraph::access_output_resource( FrameGraphResourceHandle handle ) {
    FrameGraphResource* resource = builder->access_resource( handle );
    RASSERT( resource != nullptr );
    if ( resource->output_handle.index != k_invalid_index ) {
        resource = builder->access_resource( resource->output_handle );
    }

    return resource;
}

// FrameGraphRenderPassCache /////////////////////////////////////////////////////////////

void FrameGraphRenderPassCache::init( Allocator* allocator )
{
    render_pass_map.init( allocator, FrameGraphBuilder::k_max_render_pass_count );
}

void FrameGraphRenderPassCache::shutdown( )
{
    render_pass_map.shutdown( );
}

// FrameGraphResourceCache /////////////////////////////////////////////////////////////

void FrameGraphResourceCache::init( Allocator* allocator, GpuDevice* device_ )
{
    device = device_;

    resources.init( allocator, FrameGraphBuilder::k_max_resources_count );
    resource_map.init( allocator, FrameGraphBuilder::k_max_resources_count );
}

void FrameGraphResourceCache::shutdown( )
{
    FlatHashMapIterator it = resource_map.iterator_begin();
    while ( it.is_valid() ) {

        u32 resource_index = resource_map.get( it );
        FrameGraphResource* resource = resources.get( resource_index );

        if ( ( resource->type == FrameGraphResourceType_Texture || resource->type == FrameGraphResourceType_Attachment ) ) {

            // Avoid external resources - must be handled by their owner
            if ( resource->resource_info.external ) {
                resource_map.iterator_advance( it );
                continue;
            }

            if ( resource->resource_info.texture.image.is_valid() ) {
                Image* image = device->get_image( resource->resource_info.texture.image );
                device->destroy_image( image->handle );
            }

            if ( resource->resource_info.texture.image_view.is_valid() ) {
                ImageView* view = device->get_image_view( resource->resource_info.texture.image_view );
                device->destroy_image_view( view->handle );
            }


            if ( resource->resource_info.texture.previous_image.is_valid() ) {
                Image* image = device->get_image( resource->resource_info.texture.previous_image );
                device->destroy_image( image->handle );
            }

            if ( resource->resource_info.texture.previous_image_view.is_valid() ) {
                ImageView* view = device->get_image_view( resource->resource_info.texture.previous_image_view );
                device->destroy_image_view( view->handle );
            }

        } else if ( ( resource->type == FrameGraphResourceType_Buffer )
                    && ( resource->resource_info.buffer.handle.is_valid() ) ) {
            Buffer* buffer = device->get_buffer( resource->resource_info.buffer.handle );
            device->destroy_buffer( buffer->handle );
        }

        resource_map.iterator_advance( it );
    }

    resources.free_all_resources();
    resources.shutdown();
    resource_map.shutdown( );
}

// FrameGraphNodeCache /////////////////////////////////////////////////////////////

void FrameGraphNodeCache::init( Allocator* allocator, GpuDevice* device_ )
{
    device = device_;

    nodes.init( allocator, FrameGraphBuilder::k_max_nodes_count, sizeof( FrameGraphNode ) );
    node_map.init( allocator, FrameGraphBuilder::k_max_nodes_count );
}

void FrameGraphNodeCache::shutdown( )
{
    nodes.free_all_resources();
    nodes.shutdown( );
    node_map.shutdown();
}

// FrameGraphBuilder /////////////////////////////////////////////////////////////

void FrameGraphBuilder::init( GpuDevice* device_ ) {
    device = device_;

    allocator = device->allocator;

    resource_cache.init( allocator, device );
    node_cache.init( allocator, device );
    render_pass_cache.init( allocator );

    rename_buffer.init( rkilo( 16 ), allocator );
}

void FrameGraphBuilder::shutdown() {
    resource_cache.shutdown( );
    node_cache.shutdown( );
    render_pass_cache.shutdown( );

    rename_buffer.shutdown();
}

FrameGraphResourceHandle FrameGraphBuilder::create_node_output( const FrameGraphResourceCreation& creation, FrameGraphNodeHandle producer )
{
    FrameGraphResourceHandle resource_handle{ k_invalid_index };
    resource_handle.index = resource_cache.resources.obtain_resource();

    if ( resource_handle.index == k_invalid_index ) {
        rprint( "Unable to allocate more node output (cannot allocate %s)!\n", creation.name );
        return resource_handle;
    }

    FrameGraphResource* resource = resource_cache.resources.get( resource_handle.index );
    resource->name = creation.name;
    resource->type = creation.type;

    if ( creation.type != FrameGraphResourceType_Reference ) {
        resource->resource_info = creation.resource_info;
        resource->output_handle = resource_handle;
        resource->producer = producer;
        resource->ref_count = 0;

        if ( resource->type == FrameGraphResourceType_Buffer ) {
            resource->resource_info.buffer.handle = {};
        } else {
            resource->resource_info.texture.image = ImageHandle();
            resource->resource_info.texture.image_view = ImageViewHandle();
        }

        FrameGraphNode* producer_node = access_node( producer );
        RASSERT( producer_node != nullptr );

        if ( producer_node->enabled ) {
            // TODO(marco): eventually we want to allow enabling/disabling a node at runtime.
            // We will need to patch the producer when the graph changes
            resource_cache.resource_map.insert( hash_bytes( ( void* )resource->name, strlen( creation.name ) ), resource_handle.index );
        }
    }

    return resource_handle;
}

FrameGraphResourceHandle FrameGraphBuilder::create_output_handle( const FrameGraphResourceCreation& creation )
{
    FrameGraphResourceHandle resource_handle{ k_invalid_index };
    resource_handle.index = resource_cache.resources.obtain_resource();

    if ( resource_handle.index == k_invalid_index ) {
        rprint( "Unable to allocate more node output handle (cannot allocate %s)!\n", creation.name );
        return resource_handle;
    }

    FrameGraphResource* resource = resource_cache.resources.get( resource_handle.index );
    resource->name = creation.name;
    resource->type = creation.type;

    RASSERT( creation.type != FrameGraphResourceType_Reference );
    resource->resource_info = creation.resource_info;
    resource->output_handle = resource_handle;
    resource->ref_count = 0;
    resource->rename_count = 0;

    if ( resource->type == FrameGraphResourceType_Buffer ) {
        resource->resource_info.buffer.handle = {};
    } else {
        resource->resource_info.texture.image = ImageHandle();
        resource->resource_info.texture.image_view = ImageViewHandle();
    }

    resource_cache.resource_map.insert( hash_bytes( ( void* )resource->name, strlen( creation.name ) ), resource_handle.index );

    return resource_handle;
}

FrameGraphResourceHandle FrameGraphBuilder::create_output_reference( FrameGraphResourceHandle original_handle, FrameGraphResourceType type )
{
    FrameGraphResourceHandle resource_handle{ k_invalid_index };
    resource_handle.index = resource_cache.resources.obtain_resource();

    if ( resource_handle.index == k_invalid_index ) {
        rprint( "Unable to allocate more frame graph output references!\n" );
        return resource_handle;
    }

    FrameGraphResource* resource = resource_cache.resources.get( resource_handle.index );

    FrameGraphResource* original_resource = resource_cache.resources.get( original_handle.index );
    while ( original_resource->type == FrameGraphResourceType_Reference ) {
        original_handle = original_resource->output_handle;
        original_resource = resource_cache.resources.get( original_handle.index );
    }

    resource->name = rename_buffer.append_use_f( "%s(%u)", original_resource->name, ++original_resource->rename_count );
    resource->type = type;
    resource->output_handle = original_handle;

    return resource_handle;
}

FrameGraphResourceHandle FrameGraphBuilder::create_node_input( const FrameGraphResourceCreation& creation )
{
    FrameGraphResourceHandle resource_handle = { k_invalid_index };

    resource_handle.index = resource_cache.resources.obtain_resource();

    if ( resource_handle.index == k_invalid_index ) {
        rprint( "Unable to allocate more frame graph node input (cannot allocate %s)!\n", creation.name );
        return resource_handle;
    }

    FrameGraphResource* resource = resource_cache.resources.get( resource_handle.index );

    resource->resource_info = { };
    resource->producer.index = k_invalid_index;
    resource->output_handle.index = k_invalid_index;
    resource->type = creation.type;
    resource->name = creation.name;
    resource->ref_count = 0;

    return resource_handle;
}

FrameGraphNodeHandle FrameGraphBuilder::create_node( const FrameGraphNodeCreation& creation )
{
    FrameGraphNodeHandle node_handle{ k_invalid_index };
    node_handle.index = node_cache.nodes.obtain_resource();

    if ( node_handle.index == k_invalid_index ) {
        rprint( "Unable to allocate more frame graph nodes (cannot allocate %s)!\n", creation.name );
        return node_handle;
    }

    FrameGraphNode* node = ( FrameGraphNode* )node_cache.nodes.access_resource( node_handle.index );
    node->version = 1;
    node->name = creation.name;
    node->enabled = creation.enabled;
    node->compute = creation.compute;
    node->ray_tracing = creation.ray_tracing;
    node->inputs.init( allocator, ( u32 )creation.inputs.size );
    node->outputs.init( allocator, ( u32 )creation.outputs.size );
    node->edges.init( allocator, ( u32 )creation.outputs.size );
    node->scheduling = { CommandQueueType::Graphics, 0 };

    node_cache.node_map.insert( hash_bytes( ( void* )node->name, strlen( node->name ) ), node_handle.index );

    // NOTE(marco): first create the outputs, then we can patch the input resources
    // with the right handles
    for ( u32 i = 0; i < creation.outputs.size; ++i ) {
        const FrameGraphResourceCreation& output_creation = creation.outputs[ i ];

        FrameGraphResourceHandle output = create_node_output( output_creation, node_handle );

        node->outputs.push( output );
    }

    for ( u32 i = 0; i < creation.inputs.size; ++i ) {
        const FrameGraphResourceCreation& input_creation = creation.inputs[ i ];

        FrameGraphResourceHandle input_handle = create_node_input( input_creation );

        node->inputs.push( input_handle );
    }

    return node_handle;
}

FrameGraphNodeHandle FrameGraphBuilder::create_node_v2( const FrameGraphNodeCreation_v2& creation )
{
    FrameGraphNodeHandle node_handle{ k_invalid_index };
    node_handle.index = node_cache.nodes.obtain_resource();

    if ( node_handle.index == k_invalid_index ) {
        rprint( "Unable to allocate more frame graph nodes (cannot allocate %s)!\n", creation.name );
        return node_handle;
    }

    FrameGraphNode* node = ( FrameGraphNode* )node_cache.nodes.access_resource( node_handle.index );
    node->version = 2;
    node->name = creation.name;
    node->enabled = creation.enabled;
    node->compute = creation.compute;
    node->ray_tracing = creation.ray_tracing;
    node->inputs.init( allocator, ( u32 )creation.inputs.size );
    node->temp_inputs_type.init( allocator, ( u32 )creation.inputs.size );
    node->outputs.init( allocator, ( u32 )creation.outputs.size );
    node->edges.init( allocator, ( u32 )creation.outputs.size );
    node->scheduling = creation.scheduling;

    node_cache.node_map.insert( hash_bytes( ( void* )node->name, strlen( node->name ) ), node_handle.index );

    // NOTE(marco): first create the outputs, then we can patch the input resources
    // with the right handles
    for ( u32 i = 0; i < creation.outputs.size; ++i ) {
        FrameGraphResourceHandle output = creation.outputs[ i ];
        FrameGraphResource* resource = access_resource( output );
        RASSERT( resource != nullptr );

        resource->producer = node_handle;

        node->outputs.push( output );
    }

    for ( u32 i = 0; i < creation.inputs.size; ++i ) {
        node->inputs.push( creation.inputs[ i ].handle );
        node->temp_inputs_type.push( creation.inputs[ i ].type );

        // Safety check for input/outputs problems, normally due to Span problems.
        const FrameGraphResourceHandle input = creation.inputs[ i ].handle;
        for ( u32 o = 0; o < creation.outputs.size; ++o ) {
            RASSERTM( input.index != creation.outputs[ o ].index, "Node %s uses resource handle %u as both input and output", creation.name, input.index );
        }
    }

    return node_handle;
}

FrameGraphResourceHandle FrameGraphBuilder::get_output_handle( cstring node_name, cstring resource_name ) {
    FrameGraphNode* node = get_node( node_name );
    if ( node == nullptr ) {
        rprint( "Cannot find node %s in frame graph!\n", node_name );
        return { k_invalid_index };
    }

    for ( u32 i = 0; i < node->outputs.size; ++i ) {
        FrameGraphResource* resource = access_resource( node->outputs[ i ] );

        // NOTE(marco): we use strstr here to allow for renaming of resources
        if ( strstr( resource->name, resource_name ) != nullptr ) {
            return node->outputs[ i ];
        }
    }

    return { k_invalid_index };
}

FrameGraphNode* FrameGraphBuilder::get_node( cstring name ) {
    FlatHashMapIterator it = node_cache.node_map.find( hash_calculate( name ) );
    if ( it.is_invalid() ) {
        rprint( "Cannot find node %s in frame graph!\n", name );
        return nullptr;
    }

    FrameGraphNode* node = ( FrameGraphNode* )node_cache.nodes.access_resource( node_cache.node_map.get( it ) );

    return node;
}

FrameGraphNode* FrameGraphBuilder::access_node( FrameGraphNodeHandle handle ) {
    FrameGraphNode* node = ( FrameGraphNode* )node_cache.nodes.access_resource( handle.index );

    return node;
}

void FrameGraphBuilder::add_resource( cstring name, FrameGraphResourceType type, FrameGraphResourceInfo resource_info ) {
    FlatHashMapIterator it = resource_cache.resource_map.find( hash_calculate( name ) );
    assert( it.is_invalid() );

    FrameGraphResourceHandle resource_handle{ k_invalid_index };
    resource_handle.index = resource_cache.resources.obtain_resource();

    if ( resource_handle.index == k_invalid_index ) {
        rprint( "Unable to allocate more frame graph resources (cannot allocate %s)!\n", name );
        return;
    }

    FrameGraphResource* resource = resource_cache.resources.get( resource_handle.index );
    resource->name = name;
    resource->type = type;

    resource->resource_info = resource_info;
    resource->ref_count = 0;

    resource_cache.resource_map.insert( hash_bytes( ( void* )name, strlen( name ) ), resource_handle.index );
}

FrameGraphResource* FrameGraphBuilder::get_resource( cstring name ) {
    FlatHashMapIterator it = resource_cache.resource_map.find( hash_calculate( name ) );
    if ( it.is_invalid() ) {
        //rprint( "Cannot find resource %s in frame graph!\n", name );
        return nullptr;
    }

    FrameGraphResource* resource = resource_cache.resources.get( resource_cache.resource_map.get( it ) );

    return resource;
}

FrameGraphResource* FrameGraphBuilder::access_resource( FrameGraphResourceHandle handle ) {
    FrameGraphResource* resource = resource_cache.resources.get( handle.index );

    return resource;
}

void FrameGraphBuilder::register_render_pass( cstring name, FrameGraphRenderPass* render_pass )
{
    u64 key = hash_calculate( name );

    FlatHashMapIterator it = render_pass_cache.render_pass_map.find( key );
    if ( it.is_valid() ) {
        rprint( "Render pass %s already registered in frame graph!\n", name );
        return;
    }

    it = node_cache.node_map.find( key );
    if ( it.is_invalid() ) {
        rprint( "Cannot find node %s to register render pass!\n", name );
        return;
    }

    render_pass_cache.render_pass_map.insert( key, render_pass );

    FrameGraphNode* node = ( FrameGraphNode* )node_cache.nodes.access_resource( node_cache.node_map.get( it ) );
    node->graph_render_pass = render_pass;
}

FrameGraphResourceInfo& FrameGraphResourceInfo::set_external( bool value ) {
    external = value;
    return *this;
}

FrameGraphResourceInfo& FrameGraphResourceInfo::set_buffer( sizet size, VkBufferUsageFlags flags, BufferHandle handle ) {
    buffer.size = size;
    buffer.flags = flags;
    buffer.handle = handle;
    return *this;
}

FrameGraphResourceInfo& FrameGraphResourceInfo::set_external_texture_2d( u32 width, u32 height, VkFormat format, VkImageUsageFlags flags, ImageHandle image, ImageViewHandle image_view ) {

    texture.width = width;
    texture.height = height;
    texture.depth = 1;
    texture.format = format;
    texture.flags = flags;
    texture.image = image;
    texture.image_view = image_view;

    external = true;

    return *this;
}

FrameGraphResourceInfo& FrameGraphResourceInfo::set_external_texture_3d( u32 width, u32 height, u32 depth, VkFormat format, VkImageUsageFlags flags, ImageHandle image, ImageViewHandle image_view ) {

    texture.width = width;
    texture.height = height;
    texture.depth = depth;
    texture.format = format;
    texture.flags = flags;
    texture.image = image;
    texture.image_view = image_view;

    external = true;

    return *this;
}

} // namespace raptor
