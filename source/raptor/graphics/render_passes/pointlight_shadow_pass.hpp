#pragma once

#include "graphics/frame_graph.hpp"
#include "graphics/render_scene.hpp"
#include "graphics/renderer.hpp"

namespace raptor {

    struct PointlightShadowsRuntimeData;

    static const u32 k_shadow_map_resolution = 512;
    static const u32 k_shadow_mip_count = 3;
    static const u32 k_max_layers = 256 * 6;

    //
    //
    struct PointlightShadowPass2 : public FrameGraphRenderPass {

        void                    declare_frame_graph_node( FrameGraphResourceContext& context ) override;

        void                    update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) override;
        void                    upload_gpu_data( FrameGraphResourceContext& context ) override;

        void                    render( FrameGraphRenderContext& context ) override;

        void                    create_gpu_resources( FrameGraphResourceContext& context ) override;
        void                    destroy_gpu_resources( FrameGraphResourceContext& context ) override;

        bool                    update_sparse_binding( FrameGraphRenderContext& context ); // called during rendering

        void                    recreate_lightcount_dependent_resources( FrameGraphResourceContext& context );
        void                    add_descriptors( DescriptorSetBinder& descriptors, ShaderReflectionInfo* shader_reflection,
                                                 PointlightShadowsRuntimeData& shadows, u32 frame_index );

        BufferHandle            draw_keys_sb[ k_max_frames ];
        BufferHandle            draw_meta_sb[ k_max_frames ];
        BufferHandle            meshlet_instances_sb[ k_max_frames ];
        BufferHandle            task_indirect_cmds_sb[ k_max_frames ];
        BufferHandle            indirect_draw_count_sb[ k_max_frames ];
        BufferHandle            meshlet_instance_cursor_sb[ k_max_frames ];
        BufferHandle            draw_debug_sb[ k_max_frames ];
        BufferHandle            global_debug_info_sb[ k_max_frames ];
        BufferHandle            pointlight_view_projections_sb[ k_max_frames ];
        BufferHandle            pointlight_spheres_sb[ k_max_frames ];

        DescriptorSetHandle     clear_counters_ds[ k_max_frames ];
        DescriptorSetHandle     build_lists_ds[ k_max_frames ];
        DescriptorSetHandle     build_indirect_cmds_ds[ k_max_frames ];
        DescriptorSetHandle     meshlet_draw_ds[ k_max_frames ];
        DescriptorSetHandle     shadow_resolution_descriptor_set[ k_max_frames ];

        ImageHandle             cubemap_shadow_array_image;
        ImageViewHandle         cubemap_shadow_array_image_view;
        ImageViewHandle         cubemap_shadow_mip_views[ k_shadow_mip_count ];

        ComputePipelineState    clear_counters_pipeline;
        ComputePipelineState    build_lists_pipeline;
        ComputePipelineState    build_indirect_cmds_pipeline;
        ComputePipelineState    shadow_resolution_pipeline;

        GraphicsPipelineState   meshlet_draw_pipeline;

        PagePoolHandle          shadow_maps_pool;

        u32                     last_active_lights = 0;
        u32                     resident_mip_levels[ k_num_lights ];

    }; // struct PointlightShadowPass2

} // namespace raptor