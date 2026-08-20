#pragma once

#include "graphics/frame_graph.hpp"
#include "graphics/renderer.hpp"

namespace raptor {

    struct Renderer;

    //
    //
    struct TemporalAntiAliasingPass : public FrameGraphRenderPass {

        void                    declare_frame_graph_node( FrameGraphResourceContext& context ) override;
        void                    update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) override;

        void                    pre_render( FrameGraphRenderContext& context ) override;
        void                    render( FrameGraphRenderContext& context ) override;

        void                    on_resize( FrameGraphResourceContext& context, u32 new_width, u32 new_height ) override;

        void                    create_gpu_resources( FrameGraphResourceContext& context ) override;
        void                    upload_gpu_data( FrameGraphResourceContext& context ) override;
        void                    destroy_gpu_resources( FrameGraphResourceContext& context ) override;

        void                    update_dependent_resources( FrameGraphResourceContext& context ) override;

        ComputePipelineState    taa_pipeline;

        ImageHandle             history_textures[ 2 ];
        ImageViewHandle         history_image_views[ 2 ];

        DescriptorSetHandle     taa_descriptor_set;
        u32                     taa_constants_offset;

        u32                     current_history_texture_index = 1;
        u32                     previous_history_texture_index = 0;

        Renderer*               renderer;

    }; // struct TemporalAntiAliasingPass

} // namespace raptor