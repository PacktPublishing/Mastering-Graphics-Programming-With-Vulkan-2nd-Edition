#pragma once

#include "graphics/frame_graph.hpp"
#include "graphics/renderer.hpp"

namespace raptor {

    struct Renderer;

    //
    struct SVGFCommonResources {
        ImageHandle             reflections_texture;
        ImageViewHandle         reflections_image_view;

        ImageHandle             restirgi_output_texture;
        ImageViewHandle         restirgi_output_image_view;

        ImageHandle             motion_vectors_texture;
        ImageViewHandle         motion_vectors_image_view;

        ImageHandle             depth_texture;
        ImageViewHandle         depth_image_view;

        ImageHandle             normals_texture;
        ImageViewHandle         normals_image_view;

        ImageHandle             mesh_id_texture;
        ImageViewHandle         mesh_id_image_view;

        ImageHandle             depth_normal_fwidth_texture;
        ImageViewHandle         depth_normal_fwidth_image_view;

        ImageHandle             linear_z_dd_texture;
        ImageViewHandle         linear_z_dd_image_view;

        ImageHandle             integrated_reflection_color_texture;
        ImageViewHandle         integrated_reflection_color_image_view;

        ImageHandle             integrated_reflection_moments_texture;
        ImageViewHandle         integrated_reflection_moments_image_view;

        ImageHandle             integrated_restirgi_color_texture;
        ImageViewHandle         integrated_restirgi_color_image_view;

        ImageHandle             integrated_restirgi_moments_texture;
        ImageViewHandle         integrated_restirgi_moments_image_view;
    }; // struct SVGFCommonResources

    //
    struct SVGFAccumulationOutput {
        ImageHandle             last_frame_normals_texture;
        ImageViewHandle         last_frame_normals_image_view;
        ImageHandle             last_frame_mesh_id_texture;
        ImageViewHandle         last_frame_mesh_id_image_view;
        ImageHandle             last_frame_linear_depth_texture;
        ImageViewHandle         last_frame_linear_depth_image_view;
        ImageHandle             reflections_history_texture;
        ImageViewHandle         reflections_history_image_view;
        ImageHandle             reflections_moments_history_texture;
        ImageViewHandle         reflections_moments_history_image_view;
        ImageHandle             restirgi_history_texture;
        ImageViewHandle         restirgi_history_image_view;
        ImageHandle             restirgi_moments_history_texture;
        ImageViewHandle         restirgi_moments_history_image_view;
    }; // struct SVGFAccumulationOutput

    //
    struct SVGFGuideResources {
        ImageHandle             normals_texture;
        ImageViewHandle         normals_image_view;

        ImageHandle             mesh_id_texture;
        ImageViewHandle         mesh_id_image_view;

        ImageHandle             linear_depth_texture;
        ImageViewHandle         linear_depth_image_view;

        ImageHandle             depth_normal_fwidth_texture;
        ImageViewHandle         depth_normal_fwidth_image_view;

        ImageHandle             motion_vectors_texture;
        ImageViewHandle         motion_vectors_image_view;
    }; // struct SVGFGuideResources

    //
    //
    struct SVGFGuideDownsamplePass : public FrameGraphRenderPass {

        void                    declare_frame_graph_node( FrameGraphResourceContext& context ) override;

        void                    update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) override;

        void                    render( FrameGraphRenderContext& context ) override;

        void                    on_resize( FrameGraphResourceContext& context, u32 new_width, u32 new_height ) override;
        void                    create_gpu_resources( FrameGraphResourceContext& context ) override;
        void                    upload_gpu_data( FrameGraphResourceContext& context ) override;
        void                    destroy_gpu_resources( FrameGraphResourceContext& context ) override;

        void                    create_descriptors( FrameGraphResourceContext& context );

        Renderer*               renderer = nullptr;

        u32                     constants_offset = 0;

        SVGFCommonResources     resources;
        SVGFGuideResources      output;

        DescriptorSetHandle     descriptor_set;
        ComputePipelineState    pipeline;

        f32                     texture_scale = 1.0f;

    }; // SVGFGuideDownsamplePass

    //
    //
    struct SVGFAccumulationPass : public FrameGraphRenderPass {

        void                    declare_frame_graph_node( FrameGraphResourceContext& context ) override;

        void                    update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) override;

        void                    pre_render( FrameGraphRenderContext& context ) override;
        void                    render( FrameGraphRenderContext& context ) override;

        void                    on_resize( FrameGraphResourceContext& context, u32 new_width, u32 new_height ) override;
        void                    create_gpu_resources( FrameGraphResourceContext& context ) override;
        void                    upload_gpu_data( FrameGraphResourceContext& context ) override;
        void                    destroy_gpu_resources( FrameGraphResourceContext& context ) override;

        void                    update_dependent_resources( FrameGraphResourceContext& context ) override;
        void                    create_descriptors( FrameGraphResourceContext& context );

        Renderer*               renderer;

        u32                     reflections_constants_offset;
        u32                     restirgi_constants_offset;

        SVGFCommonResources     resources;
        SVGFAccumulationOutput  output;
        SVGFGuideResources      guide;

        DescriptorSetHandle     descriptor_set;
        ComputePipelineState    pipeline;

        f32                     texture_scale = 1.0f;
        bool                    reset_history = false;

    }; // SVGFAccumulationPass

    //
    //
    struct SVGFVariancePass : public FrameGraphRenderPass {

        void                    declare_frame_graph_node( FrameGraphResourceContext& context ) override;

        void                    update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) override;

        void                    pre_render( FrameGraphRenderContext& context ) override;
        void                    render( FrameGraphRenderContext& context ) override;

        void                    on_resize( FrameGraphResourceContext& context, u32 new_width, u32 new_height ) override;

        void                    create_gpu_resources( FrameGraphResourceContext& context ) override;
        void                    upload_gpu_data( FrameGraphResourceContext& context ) override;
        void                    destroy_gpu_resources( FrameGraphResourceContext& context ) override;

        void                    update_dependent_resources( FrameGraphResourceContext& context ) override;
        void                    create_descriptors( FrameGraphResourceContext& context );

        Renderer*               renderer;

        u32                     reflections_constants_offset;
        u32                     restirgi_constants_offset;

        SVGFCommonResources     resources;
        SVGFAccumulationOutput  accumulation_input;
        SVGFGuideResources      guide;

        ImageHandle             reflections_variance_texture;
        ImageViewHandle         reflections_variance_image_view;

        ImageHandle             restirgi_variance_texture;
        ImageViewHandle         restirgi_variance_image_view;

        DescriptorSetHandle     descriptor_set;
        ComputePipelineState    pipeline;

        f32                     texture_scale = 1.0f;

    }; // SVGFVariancePass

    //
    //
    struct SVGFWaveletPass : public FrameGraphRenderPass {
        static const u32 k_num_passes = 5;

        void                    declare_frame_graph_node( FrameGraphResourceContext& context ) override;

        void                    update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) override;

        void                    pre_render( FrameGraphRenderContext& context ) override;
        void                    render( FrameGraphRenderContext& context ) override;

        void                    on_resize( FrameGraphResourceContext& context, u32 new_width, u32 new_height ) override;

        void                    create_gpu_resources( FrameGraphResourceContext& context ) override;
        void                    upload_gpu_data( FrameGraphResourceContext& context ) override;
        void                    destroy_gpu_resources( FrameGraphResourceContext& context ) override;

        void                    create_descriptors( FrameGraphResourceContext& context );

        Renderer*               renderer;

        SVGFCommonResources     resources;
        SVGFAccumulationOutput  accumulation_input;
        SVGFGuideResources      guide;

        ImageHandle             reflections_variance_texture;
        ImageViewHandle         reflections_variance_image_view;
        ImageHandle             restirgi_variance_texture;
        ImageViewHandle         restirgi_variance_image_view;

        ImageHandle             reflections_ping_pong_color_image;
        ImageViewHandle         reflections_ping_pong_color_image_view;
        ImageHandle             restirgi_ping_pong_color_image;
        ImageViewHandle         restirgi_ping_pong_color_image_view;
        ImageHandle             ping_pong_variance_image;
        ImageViewHandle         ping_pong_variance_image_view;

        ImageHandle             svgf_reflections_output_texture;
        ImageHandle             svgf_restirgi_output_texture;

        u32                     reflections_constant_offsets[ k_num_passes ];
        u32                     restirgi_constant_offsets[ k_num_passes ];

        DescriptorSetHandle     descriptor_set[ k_num_passes ];
        ComputePipelineState    pipeline;

        f32                     texture_scale = 1.0f;

    }; // SVGFWaveletPass

} // namespace raptor
