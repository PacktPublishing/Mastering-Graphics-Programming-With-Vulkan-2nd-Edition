#pragma once

#include "graphics/gpu_resources.hpp"
#include "graphics/render_scene.hpp"
#include "graphics/renderer.hpp"

#include "foundation/hash_map.hpp"

struct ufbx_scene;
struct ufbx_anim;

namespace raptor
{
    //
    //
    struct FbxModel :  public RenderModel {

        virtual                                 ~FbxModel() override { };

        void                                    init( RenderScene* render_scene, SceneGraph* scene_graph, Allocator* resident_allocator, Renderer* renderer_ ) override;
        void                                    load_model( cstring filename, cstring path, ArenaAllocator* temp_allocator ) override;
        void                                    add_animation( cstring filename, cstring path, ArenaAllocator* temp_allocator ) override;
        void                                    shutdown( Renderer* renderer ) override;

        void                                    load_animation( ufbx_scene* fbx_scene, ufbx_anim* fbx_animation );

        ufbx_scene*                             fbx_scene;
    };

}
