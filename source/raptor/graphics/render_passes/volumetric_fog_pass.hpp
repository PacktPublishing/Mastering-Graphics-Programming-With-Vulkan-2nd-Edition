#pragma once

#include "graphics/frame_graph.hpp"
#include "graphics/renderer.hpp"

namespace raptor {

    //
    //
    struct VolumetricFogPass : public FrameGraphRenderPass {

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

        // Inject Data
        ComputePipelineState    inject_data_pipeline;
        ComputePipelineState    inject_data_pipeline_slang;
        ImageHandle             froxel_data_image_0;
        ImageViewHandle         froxel_data_image_view_0;

        // Light Scattering
        ComputePipelineState    light_scattering_pipeline;
        ComputePipelineState    light_scattering_pipeline_slang;
        ImageHandle             temporal_history_image[ 2 ]{};
        ImageViewHandle         temporal_history_image_view[ 2 ]{};
        u32                     temporal_current_index = 0;
        u32                     temporal_previous_index = 1;

        DescriptorSetHandle     light_scattering_descriptor_set[ k_max_frames ];
        DescriptorSetHandle     light_scattering_descriptor_set_slang[ k_max_frames ];

        // Light Integration
        ComputePipelineState    light_integration_pipeline;
        ComputePipelineState    light_integration_pipeline_slang;
        ImageHandle             raw_light_scattering_image{};
        ImageViewHandle         raw_light_scattering_image_view{};

        // Spatial Filtering
        ComputePipelineState    spatial_filtering_pipeline;
        ComputePipelineState    spatial_filtering_pipeline_slang;
        // Temporal Filtering
        ComputePipelineState    temporal_filtering_pipeline;
        ComputePipelineState    temporal_filtering_pipeline_slang;
        // Volumetric Noise baking
        ComputePipelineState    volumetric_noise_baking_pipeline;
        ComputePipelineState    volumetric_noise_baking_pipeline_slang;
        ImageHandle             volumetric_noise_image;
        ImageViewHandle         volumetric_noise_image_view;
        SamplerHandle           volumetric_tiling_sampler;
        bool                    has_baked_noise = false;

        SamplerHandle           volumetric_fog_sampler;

        DescriptorSetHandle     fog_descriptor_set;
        u32                     fog_constants_offset;

    }; // struct VolumetricFogPass

} // namespace raptor