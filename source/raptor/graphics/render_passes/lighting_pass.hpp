#pragma once

#include "graphics/frame_graph.hpp"
#include "graphics/renderer.hpp"

namespace raptor {

    struct Renderer;

    //
    //
    struct LightingPass : public FrameGraphRenderPass {

        void                    declare_frame_graph_node( FrameGraphResourceContext& context ) override;

        void                    update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) override;

        void                    render( FrameGraphRenderContext& context ) override;
        void                    post_render( FrameGraphRenderContext& context ) override;

        void                    on_resize( FrameGraphResourceContext& context, u32 new_width, u32 new_height ) override;

        void                    create_gpu_resources( FrameGraphResourceContext& context ) override;
        void                    destroy_gpu_resources( FrameGraphResourceContext& context ) override;
        void                    update_dependent_resources( FrameGraphResourceContext& context ) override;

        void                    upload_gpu_data( FrameGraphResourceContext& context ) override;
        void                    create_descriptors( FrameGraphResourceContext& context );

        u32                     constants_offset;

        bool                    use_compute;

        ComputePipelineState    compute_pipeline;

        DescriptorSetHandle     lighting_descriptor_set[ k_max_frames ];

        ImageHandle             lighting_debug_texture;
        ImageViewHandle         lighting_debug_image_view;

        DescriptorSetHandle     fragment_rate_descriptor_set[ k_max_frames ];
        BufferHandle            fragment_rate_texture_index[ k_max_frames ];

        FrameGraphResource*     color_texture;
        FrameGraphResource*     normal_texture;
        FrameGraphResource*     roughness_texture;
        FrameGraphResource*     depth_texture;
        FrameGraphResource*     emissive_texture;
        FrameGraphResource*     gi_texture;
        FrameGraphResource*     reflections_texture;

        FrameGraphResource*     output_texture;
    }; // struct LightPass


} // namespace raptor
