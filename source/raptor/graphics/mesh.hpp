#pragma once

#include "foundation/platform.hpp"

namespace raptor {

    struct Mesh;

    //
    //
    struct MeshInstance {

        u32                 mesh_index              = u32_max;

        u32                 gpu_mesh_instance_index = u32_max;
        u32                 scene_graph_node_index  = u32_max;

    }; // struct MeshInstance

    //
    //
    struct MeshInstanceDraw {
        MeshInstance*       mesh_instance       = nullptr;
        u32                 material_pass_index = u32_max;
    }; // struct MeshInstanceDraw

} // namespace raptor
