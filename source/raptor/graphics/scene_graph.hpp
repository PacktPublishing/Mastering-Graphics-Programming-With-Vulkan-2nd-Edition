#pragma once

#include "foundation/array.hpp"
#include "foundation/hash_map.hpp"
#include "foundation/bit.hpp"
#include "foundation/string.hpp"

#include "external/glm/mat4x4.hpp"

namespace raptor {

//
struct Hierarchy {
    i32                 parent : 24;
    i32                 level : 8;
    Array<u32>          children;
}; // struct Hierarchy

//
struct SceneGraphNodeDebugData {
    cstring             name;
    bool                is_bone;
}; // struct SceneGraphNodeDebugData


//
//
struct SceneGraph {

    void                init( Allocator* resident_allocator, u32 num_nodes );
    void                shutdown();

    void                init_new_nodes( u32 offset, u32 num_nodes );
    void                resize( u32 num_nodes );
    void                update_matrices();

    void                debug_ui();
    void                debug_ui_node( u32 node_index );

    void                set_hierarchy( u32 node_index, u32 parent_index, u32 level );
    void                set_local_matrix( u32 node_index, const glm::mat4& local_matrix );
    void                set_debug_data( u32 node_index, cstring name );
    void                set_node_as_bone( u32 node_index );
    u32                 get_node_index( cstring name );

    u32                 node_count();

    Array<glm::mat4>    local_matrices;
    Array<glm::mat4>    world_matrices;
    Array<Hierarchy>    nodes_hierarchy;
    Array<SceneGraphNodeDebugData> nodes_debug_data;
    FlatHashMap<u64, u32> node_name_to_index;

    StringBuffer        names_buffer;

    BitSet              updated_nodes;

    bool                sort_update_order = true;

    u32                 selected_node = -1;

}; // struct SceneGraph

} // namespace raptor
