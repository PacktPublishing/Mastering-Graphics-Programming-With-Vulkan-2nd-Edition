#include "graphics/scene_graph.hpp"
#include "graphics/render_scene.hpp"

#include "foundation/numerics.hpp"
#include "foundation/time.hpp"

#include "external/glm/mat4x4.hpp"
#include "external/imgui/imgui.h"

#include <string.h>

namespace raptor {

void SceneGraph::init( Allocator* resident_allocator, u32 num_nodes ) {
    nodes_hierarchy.init( resident_allocator, num_nodes );
    local_matrices.init( resident_allocator, num_nodes );
    world_matrices.init( resident_allocator, num_nodes );
    nodes_debug_data.init( resident_allocator, num_nodes );
    node_name_to_index.init( resident_allocator, num_nodes );

    updated_nodes.init( resident_allocator, num_nodes );

    names_buffer.init( rkilo( 64 ), resident_allocator );
}

void SceneGraph::shutdown() {
    nodes_debug_data.shutdown();
    for ( u32 i = 0; i < nodes_hierarchy.size; ++i ) {
        nodes_hierarchy[ i ].children.shutdown();
    }
    nodes_hierarchy.shutdown();
    updated_nodes.shutdown();
    local_matrices.shutdown();
    world_matrices.shutdown();
    node_name_to_index.shutdown();

    names_buffer.shutdown();
}

void SceneGraph::resize( u32 num_nodes ) {
    nodes_hierarchy.set_size( num_nodes );
    local_matrices.set_size( num_nodes );
    world_matrices.set_size( num_nodes );
    nodes_debug_data.set_size( num_nodes );

    updated_nodes.resize( num_nodes );
}

void SceneGraph::init_new_nodes( u32 offset, u32 num_nodes ) {
    memset( nodes_hierarchy.data + offset, 0, num_nodes * sizeof( Hierarchy ) );

    for ( u32 i = offset; i < offset + num_nodes; ++i ) {
        nodes_hierarchy[ i ].parent = -1;
        nodes_debug_data[ i ].name = nullptr;
        nodes_debug_data[ i ].is_bone = false;
    }
}

void SceneGraph::update_matrices() {

    // TODO: per level update
    u32 max_level = 0;
    for ( u32 i = 0; i < nodes_hierarchy.size; ++i ) {
        max_level = raptor::max(max_level, (u32)nodes_hierarchy[ i ].level );
    }
    u32 current_level = 0;
    u32 nodes_visited = 0;

    //i64 time = time_now();
    while ( current_level <= max_level ) {

        for ( u32 i = 0; i < nodes_hierarchy.size; ++i ) {

            if ( nodes_hierarchy[ i ].level != current_level ) {
                continue;
            }

            if ( updated_nodes.get_bit( i ) == 0 ) {
                continue;
            }

            updated_nodes.clear_bit( i );

            if ( nodes_hierarchy[ i ].parent == -1 ) {
                world_matrices[ i ] = local_matrices[ i ];
            } else {
                const glm::mat4& parent_matrix = world_matrices[ nodes_hierarchy[ i ].parent ];
                world_matrices[ i ] = parent_matrix * local_matrices[ i ];
            }

            ++nodes_visited;
        }

        ++current_level;
    }

    //rprint( "Updated scene graph in %fms\n", time_from_milliseconds( time ) );

    /*for ( u32 i = 0; i < nodes_hierarchy.size; ++i ) {
        if ( updated_nodes.get_bit( i ) == 0 ) {
            continue;
        }

        updated_nodes.clear_bit( i );

        if ( nodes_hierarchy[ i ].parent == -1 ) {
            world_matrices[ i ] = local_matrices[ i ];
        }
        else {
            const mat4s& parent_matrix = world_matrices[ nodes_hierarchy[ i ].parent ];
            world_matrices[ i ] = glms_mat4_mul( parent_matrix, local_matrices[ i ] );
        }
    }*/
}

void SceneGraph::set_hierarchy( u32 node_index, u32 parent_index, u32 level ) {
    // Mark node as updated
    updated_nodes.set_bit( node_index );
    nodes_hierarchy[ node_index ].parent = parent_index;
    nodes_hierarchy[ node_index ].level = level;

    nodes_hierarchy[ parent_index ].children.push( node_index );

    sort_update_order = true;
}

void SceneGraph::debug_ui_node( u32 node_index ) {
    const Hierarchy& hierarchy = nodes_hierarchy[ node_index ];

    cstring node_name = nodes_debug_data[ node_index ].name;
    cstring name = names_buffer.append_use_f( "%s (%u)", node_name ? node_name : "<no name>", node_index );

    // TODO(marco): node selection between leaf and tree nodes is still not precise, but good enough for now
    if ( hierarchy.children.size == 0 ) {
        if ( ImGui::Selectable( name ) ) {
            selected_node = node_index;
        }
        return;
    }

    if ( ImGui::TreeNode( name ) ) {
        for ( u32 c = 0; c < hierarchy.children.size; ++c ) {
            debug_ui_node( hierarchy.children[ c ] );
        }
        ImGui::TreePop();
    }
    if ( ImGui::IsItemClicked() ) {
        selected_node = node_index;
    }
}

void SceneGraph::debug_ui() {
    if ( selected_node != (u32)-1 ) {
        const Hierarchy& hierarchy = nodes_hierarchy[ selected_node ];

        const glm::mat4& local = local_matrices[selected_node];
        ImGui::Text("%.2f %.2f %.2f %.2f", local[0][0], local[1][0], local[2][0], local[3][0]);
        ImGui::Text("%.2f %.2f %.2f %.2f", local[0][1], local[1][1], local[2][1], local[3][1]);
        ImGui::Text("%.2f %.2f %.2f %.2f", local[0][2], local[1][2], local[2][2], local[3][2]);
        ImGui::Text("%.2f %.2f %.2f %.2f", local[0][3], local[1][3], local[2][3], local[3][3]);
        ImGui::Separator();
    }

    if ( ImGui::CollapsingHeader( "Scene Graph" ) ) {
        names_buffer.clear();
        for ( u32 i = 0; i < nodes_hierarchy.size; ++i ) {
            const Hierarchy& hierarchy = nodes_hierarchy[ i ];
            if ( hierarchy.parent != -1 ) {
                continue;
            }

            debug_ui_node( i );
        }
    }
}

void SceneGraph::set_local_matrix( u32 node_index, const glm::mat4& local_matrix ) {
    // Mark node as updated
    updated_nodes.set_bit( node_index );
    local_matrices[ node_index ] = local_matrix;
}

void SceneGraph::set_debug_data( u32 node_index, cstring name ) {
    nodes_debug_data[ node_index ].name = name;

    // If the node doesn't have a name, it likely cannot be used to match animations in a separate file and we can ignore it
    if ( name == nullptr ) {
        return;
    }

    u64 name_hash = hash_calculate( name );
    node_name_to_index.insert( name_hash, node_index );
}

void SceneGraph::set_node_as_bone( u32 node_index ) {
    nodes_debug_data[ node_index ].is_bone = true;
}

u32 SceneGraph::get_node_index( cstring name ) {
    RASSERT( name );
    u64 name_hash = hash_calculate( name );
    auto map_it = node_name_to_index.find( name_hash );
    if ( map_it.is_valid() ) {
        return node_name_to_index.get( map_it );
    }

    return (u32)-1;
}

u32 SceneGraph::node_count() {
    return nodes_hierarchy.size;
}

} // namespace raptor
