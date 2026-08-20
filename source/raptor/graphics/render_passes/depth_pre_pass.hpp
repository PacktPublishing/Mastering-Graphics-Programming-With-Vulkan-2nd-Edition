#pragma once

#include "graphics/mesh.hpp"
#include "graphics/frame_graph.hpp"

namespace raptor {

    struct Renderer;

    //
    //
    struct DepthPrePass : public FrameGraphRenderPass {
        void                    render( FrameGraphRenderContext& context ) override;

        void                    create_gpu_resources( FrameGraphResourceContext& context ) override;
        void                    destroy_gpu_resources( FrameGraphResourceContext& context ) override;

        Array<MeshInstanceDraw> mesh_instance_draws;
        Renderer*               renderer;
        u32                     meshlet_technique_index;
    }; // struct DepthPrePass

} // namespace raptor
