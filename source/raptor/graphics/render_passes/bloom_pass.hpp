#pragma once

#include "graphics/frame_graph.hpp"
#include "graphics/renderer.hpp"

namespace raptor {

    //
    //
    struct BloomPass : public FrameGraphRenderPass {

        void declare_frame_graph_node( FrameGraphResourceContext& context ) override;

        void on_resize( FrameGraphResourceContext& context, u32 new_width, u32 new_height ) override;

        void create_gpu_resources( FrameGraphResourceContext& context ) override;
        void destroy_gpu_resources( FrameGraphResourceContext& context ) override;

        void add_ui();

        void update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) override;

        void post_render( FrameGraphRenderContext& context ) override;
        
        void create_image_views_for_mipmaps( GpuDevice& gpu, FrameGraph* frame_graph, u32 mip_levels );

        void set_external_framegraph_resource( FrameGraph* frame_graph, GpuDevice& gpu );


        ImageHandle                 bloom_image;
        StaticArray<ImageViewHandle, 16> bloom_image_views;

        ComputePipelineState        downsample_pipeline;
        ComputePipelineState        upsample_pipeline;

        DescriptorSetHandle         descriptor_set;

        static constexpr cstring    k_name = "bloom_pass";
    }; // struct BloomPass

} // namespace raptor