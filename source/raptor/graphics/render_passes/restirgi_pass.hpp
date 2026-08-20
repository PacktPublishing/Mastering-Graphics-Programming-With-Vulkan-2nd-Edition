#pragma once

#include "graphics/frame_graph.hpp"
#include "graphics/renderer.hpp"
#include "external/glm/vec3.hpp"
#include "external/glm/vec4.hpp"

namespace raptor {

    struct Renderer;

    struct ReSTIRGICommonResources {
        ImageHandle             normals_texture;
        ImageViewHandle         normals_image_view;

        ImageHandle             depth_texture;
        ImageViewHandle         depth_image_view;

        ImageHandle             linear_depth_texture;
        ImageViewHandle         linear_depth_image_view;

        ImageHandle             albedo_texture;
        ImageViewHandle         albedo_image_view;

        ImageHandle             orm_texture;
        ImageViewHandle         orm_image_view;

        ImageHandle             mesh_id_texture;
        ImageViewHandle         mesh_id_image_view;

        ImageHandle             motion_vectors_texture;
        ImageViewHandle         motion_vectors_image_view;

        ImageHandle             linear_depth_history_texture;
        ImageViewHandle         linear_depth_history_image_view;

        ImageHandle             normal_history_texture;
        ImageViewHandle         normal_history_image_view;

        ImageHandle             mesh_id_history_texture;
        ImageViewHandle         mesh_id_history_image_view;
    }; // struct ReSTIRGICommonResources

    struct alignas(16) ReservoirSample {
        // NOTE(marco): we use vec4 to ensure proper alignment of the struct in the buffer
        glm::vec4 xv;
        glm::vec4 nv; // visible point and surface normal
        glm::vec3 xs;
        f32       s_roughness;
        glm::vec3 ns; // sample point and surface normal
        f32       s_metalness;
        glm::vec4 s_albedo;
        glm::vec4 lo; // outgoing radiance at sample point

        glm::vec4 wi; // sampled direction

        glm::vec3 Fs; // BRDF at xv
        float p_wi; // pdf of sampled direction

        float p_hat; // target function
        float cos_theta;  // dot(nv, wi)
        u32   pad[2];
    };

    struct alignas(16) Reservoir {
        ReservoirSample z;

        // From Algorithm 1, Weighted Reservoir Sampling
        float w_sum;   // line 3
        float W;       // line 5
        u32  M;        // line 4
        u32  pad;
    };

    //
    //
    struct ReSTIRGIPass : public FrameGraphRenderPass {

        void                    declare_frame_graph_node( FrameGraphResourceContext& context ) override;

        void                    update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) override;

        void                    render( FrameGraphRenderContext& context ) override;

        void                    on_resize( FrameGraphResourceContext& context, u32 new_width, u32 new_height ) override;
        void                    create_gpu_resources( FrameGraphResourceContext& context ) override;
        void                    upload_gpu_data( FrameGraphResourceContext& context ) override;
        void                    destroy_gpu_resources( FrameGraphResourceContext& context ) override;

        void                    update_dependent_resources( FrameGraphResourceContext& context ) override;
        void                    create_descriptors( FrameGraphRenderContext& context );
        void                    create_descriptors( FrameGraphResourceContext& context );

        Renderer*               renderer;
        bool                    descriptors_created = false;

        u32                     constants_offset;
        u32                     reservoir_buffer_size;

        BufferHandle            temporal_reservoir_buffer[2];
        BufferHandle            spatial_reservoir_buffer;
        ImageHandle             output_texture;
        ImageViewHandle         output_image_view;
        ImageHandle             output_indirect_texture;
        ImageViewHandle         output_indirect_image_view;
        ImageHandle             output_history_texture;
        ImageViewHandle         output_history_image_view;

        RayTracingPipelineState sample_generation_pipeline;
        ComputePipelineState    spatial_sampling_pipeline;
        ComputePipelineState    temporal_accumulation_pipeline;
        DescriptorSetHandle     descriptor_set_ping;
        DescriptorSetHandle     descriptor_set_pong;

        ReSTIRGICommonResources resources;

        f32                     texture_scale = 1.0f;
        bool                    reset_history = true;

    }; // ReSTIRGIPass

} // namespace raptor
