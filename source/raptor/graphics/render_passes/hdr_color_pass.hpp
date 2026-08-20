#pragma once

#include "graphics/frame_graph.hpp"
#include "graphics/renderer.hpp"

namespace raptor {

    //
    //
    struct HDRColorCopyPass : public FrameGraphRenderPass {

        void declare_frame_graph_node( FrameGraphResourceContext& context ) override;

        void post_render( FrameGraphRenderContext& context ) override;

        static constexpr cstring    k_name = "hdr_color_copy_pass";
    }; // struct HDRColorCopyPass;

} // namespace raptor