#pragma once

#include "graphics/gpu_resources.hpp"
#include "graphics/render_scene.hpp"

struct cgltf_data;
struct cgltf_accessor;
struct cgltf_texture_view;
struct cgltf_material;

namespace raptor {
    //
    //
    struct glTFModel : public RenderModel {

        virtual                 ~glTFModel() override { };

        void                    init( RenderScene* render_scene, SceneGraph* scene_graph, Allocator* resident_allocator, Renderer* renderer ) override;
        void                    load_model( cstring filename, cstring path, ArenaAllocator* temp_allocator ) override;
        void                    shutdown( Renderer* renderer ) override;

        u16                     get_material_texture( GpuDevice& gpu, cgltf_data& gltf_scene, u32 image_offset, u32 sampler_offset, cgltf_texture_view& texture_info );

        void                    fill_pbr_material( cgltf_data& gltf_scene, Renderer& renderer, u32 image_offset, u32 sampler_offset, cgltf_material& material, PBRMaterial& pbr_material );

        cgltf_data*             gltf_data; // Source gltf scene

    }; // struct GltfScene

} // namespace raptor
