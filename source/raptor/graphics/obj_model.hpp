#pragma once

#include "graphics/gpu_resources.hpp"
#include "graphics/render_scene.hpp"
#include "graphics/renderer.hpp"

#include "foundation/hash_map.hpp"

struct aiScene;

namespace raptor
{
    //
    //
    struct ObjModel :  public RenderModel {

        virtual                                 ~ObjModel() override { };

        void                                    init( RenderScene* render_scene, SceneGraph* scene_graph, Allocator* resident_allocator, Renderer* renderer_ ) override;
        void                                    load_model( cstring filename, cstring path, ArenaAllocator* temp_allocator ) override;
        void                                    shutdown( Renderer* renderer ) override;

        u32                                     load_texture( cstring texture_path, cstring path, ArenaAllocator* temp_allocator );

        // All graphics resources used by the scene
        SamplerResource*                        sampler;

        const aiScene*                          assimp_scene;
        FlatHashMap<u64, u32>                   texture_map;

    }; // struct ObjScene

} // namespace raptor
