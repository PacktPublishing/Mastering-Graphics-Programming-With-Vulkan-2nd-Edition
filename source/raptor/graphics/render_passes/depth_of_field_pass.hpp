#pragma once

#include "graphics/frame_graph.hpp"

namespace raptor {

    struct Renderer;
    struct TextureResource;

    //
    //
    struct DoFPass : public FrameGraphRenderPass {

        struct DoFData {
            u32                 textures[ 4 ]; // diffuse, depth
            float               znear;
            float               zfar;
            float               focal_length;
            float               plane_in_focus;
            float               aperture;
        }; // struct DoFData

        void                    add_ui() override;
        void                    pre_render( FrameGraphRenderContext& context ) override;
        void                    render( FrameGraphRenderContext& context ) override;
        void                    on_resize( FrameGraphResourceContext& context, u32 new_width, u32 new_height ) override;

        void                    create_gpu_resources( FrameGraphResourceContext& context ) override;
        void                    upload_gpu_data( FrameGraphResourceContext& context ) override;
        void                    destroy_gpu_resources( FrameGraphResourceContext& context ) override;

        DescriptorSetHandle     descriptor_set;
        Renderer*               renderer;
        u32                     constants_offset;

        TextureResource*        scene_mips;
        FrameGraphResource*     depth_texture;

        float                   znear;
        float                   zfar;
        float                   focal_length;
        float                   plane_in_focus;
        float                   aperture;
    }; // struct DoFPass

} // namespace raptor