#pragma once

#include "graphics/frame_graph.hpp"
#include "graphics/renderer.hpp"

namespace raptor {

    struct Renderer;

    static const u32            k_max_depth_pyramid_levels = 12;

    /// An enumeration of all the permutations that can be passed to the SPD algorithm.
    ///
    /// SPD features are organized through a set of pre-defined compile
    /// permutation options that need to be specified. Which shader blob
    /// is returned for pipeline creation will be determined by what combination
    /// of shader permutations are enabled.
    ///
    /// @ingroup SPD
    typedef enum SpdShaderPermutationOptions
    {
        SPD_SHADER_PERMUTATION_LINEAR_SAMPLE          = (1 << 0),  ///< Sampling will be done with a linear sampler vs. via load
        SPD_SHADER_PERMUTATION_WAVE_INTEROP_LDS       = (1 << 1),  ///< Wave ops will be done via LDS rather than wave ops
        SPD_SHADER_PERMUTATION_FORCE_WAVE64           = (1 << 2),  ///< doesn't map to a define, selects different table
        SPD_SHADER_PERMUTATION_ALLOW_FP16             = (1 << 3),  ///< Enables fast math computations where possible
        SPD_SHADER_PERMUTATION_DOWNSAMPLE_FILTER_MEAN = (1 << 4),  ///< Get average of input values in SpdReduce
        SPD_SHADER_PERMUTATION_DOWNSAMPLE_FILTER_MIN  = (1 << 5),  ///< Get minimum of input values in SpdReduce
        SPD_SHADER_PERMUTATION_DOWNSAMPLE_FILTER_MAX  = (1 << 6),  ///< Get maximum of input values in SpdReduce
    } SpdShaderPermutationOptions;

    // Constants for SPD dispatches. Must be kept in sync with cbSPD in ffx_spd_callbacks_hlsl.h
    typedef struct SpdConstants
    {
        uint32_t mips;
        uint32_t numWorkGroups;
        uint32_t workGroupOffset[2];
        float    invInputSize[2];       // Only used for linear sampling mode
        float    padding[2];

    } SpdConstants;

    //
    //
    struct DepthPyramidPass : public FrameGraphRenderPass {

        void                    declare_frame_graph_node( FrameGraphResourceContext& context ) override;

        void                    update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) override;

        void                    render( FrameGraphRenderContext& context ) override;
        void                    on_resize( FrameGraphResourceContext& context, u32 new_width, u32 new_height ) override;
        void                    post_render( FrameGraphRenderContext& context ) override;

        void                    create_gpu_resources( FrameGraphResourceContext& context ) override;
        void                    destroy_gpu_resources( FrameGraphResourceContext& context ) override;

        void                    create_depth_pyramid_resource( Renderer* renderer, ImageHandle depth_image, ImageViewHandle depth_image_view );
        void                    set_external_framegraph_resource( FrameGraph* frame_graph, GpuDevice& gpu );

        ComputePipelineState    pipeline;
        ComputePipelineState    spd_pipeline;

        ImageHandle             depth_pyramid_image;
        SamplerHandle           depth_pyramid_sampler;
        ImageViewHandle         depth_pyramid_image_views[ k_max_depth_pyramid_levels ];
        DescriptorSetHandle     depth_hierarchy_descriptor_set[ k_max_depth_pyramid_levels ];

        u32                     depth_pyramid_levels = 0;
        SpdConstants            spd_constants;
        BufferHandle            spd_atomic_buffer;
        DescriptorSetHandle     spd_descriptor_set;
        ImageHandle             spd_depth_image;
        ImageViewHandle         spd_depth_image_view_all_mips[ k_max_depth_pyramid_levels + 1 ];
        ImageViewHandle         spd_depth_image_view_mid_mip;

        bool                    update_depth_pyramid;
    }; // struct DepthPyramidPass

} // namespace raptor
