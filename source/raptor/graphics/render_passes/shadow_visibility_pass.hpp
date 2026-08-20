#pragma once

#include "graphics/frame_graph.hpp"
#include "graphics/renderer.hpp"

namespace raptor {

    //
    //
    struct ShadowVisibilityPass : public FrameGraphRenderPass {
        
        void                    declare_frame_graph_node( FrameGraphResourceContext& context ) override;

        void                    update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) override;


        void                    render( FrameGraphRenderContext& context ) override;
        void                    on_resize( FrameGraphResourceContext& context, u32 new_width, u32 new_height ) override;

        void                    create_gpu_resources( FrameGraphResourceContext& context ) override;
        void                    upload_gpu_data( FrameGraphResourceContext& context ) override;
        void                    destroy_gpu_resources( FrameGraphResourceContext& context ) override;
        void                    create_descriptors( Renderer* renderer, RenderBlackboard* render_blackboard );

        void                    recreate_textures( GpuDevice& gpu, u32 lights_count, u32 width, u32 height );

        ComputePipelineState    variance_pipeline;
        ComputePipelineState    visibility_pipeline;
        ComputePipelineState    visibility_filtering_pipeline;
        ComputePipelineState    cache_reprojection_pipeline;
        ComputePipelineState    upscaling_pipeline;

        DescriptorSetHandle     descriptor_set[ k_max_frames ];

        ImageHandle             variation_image;
        ImageHandle             filtered_visibility_image;
        ImageHandle             filtered_variation_image;
        ImageHandle             variation_cache_image[ 2 ];
        ImageHandle             visibility_cache_image[ 2 ];
        ImageHandle             samples_count_cache_image[ 2 ];

        ImageViewHandle         variation_image_view;
        ImageViewHandle         filtered_visibility_image_view;
        ImageViewHandle         filtered_variation_image_view;

        // Previous/current image views
        ImageViewHandle         variation_cache_image_view[ 2 ];
        ImageViewHandle         visibility_cache_image_view[ 2 ];
        ImageViewHandle         samples_count_cache_image_view[ 2 ];

        ImageViewHandle         cached_normals_image_view;

        u32                     constants_offset;

        FrameGraphResource*     shadow_visibility_resource;

        bool                    clear_resources;
        bool                    descriptors_created = false;
        u32                     last_active_lights_count = 0;

        f32                     texture_scale;

    }; // struct ShadowVisibilityPass

} // namespace raptor