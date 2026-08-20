#pragma once

#include "foundation/array.hpp"

#include "graphics/gpu_resources.hpp"
#include "graphics/render_blackboard.hpp"
#include "graphics/render_scene.hpp"

#include "external/enkiTS/TaskScheduler.h"

#include "foundation/color.hpp"

#include "frame_graph.hpp"

namespace raptor {

struct CommandBuffer;
struct FrameGraph;
struct FrameGraphRenderPass;
struct GpuDevice;
struct GpuTechnique;
struct MeshInstance;
struct Renderer;
struct RenderScene;
struct SceneGraph;

//
//
struct UploadGpuDataContext {
    GameCamera&             game_camera;
    glm::vec2               last_clicked_position_left_button;

    ArenaAllocator*         scratch_allocator;
    Renderer*               renderer;
    RenderBlackboard&       render_blackboard;
    RenderConfig&           render_config;
    FrameGraph*             frame_graph;
    RenderScene*            scene;
    GpuFrameData&           scene_data;

}; // struct UploadGpuDataContext

//
//
struct UpscaleSettings {
    f32                     render_scale = 0.75f;
    bool                    enabled = true;
}; // struct UpscaleSettings

// Rendering Features ////////////////////////////////////////////////////

//
struct MeshesRenderingFeature {

    void        create_geometry_resources( Renderer* renderer, RenderBlackboard* render_blackboard, RenderScene* render_scene );
    void        create_gpu_resources( Renderer* renderer, RenderBlackboard* render_blackboard, RenderScene* render_scene );
    void        destroy_gpu_resources( Renderer* renderer, RenderBlackboard* render_blackboard );

    void        on_resize( Renderer* renderer, RenderBlackboard* render_blackboard, u32 new_width, u32 new_height );

    void        upload_gpu_data( UploadGpuDataContext& context );

}; // struct MeshesRenderingFeature


//
struct GpuCullingRenderingFeature {

    void        create_gpu_resources( Renderer* renderer, RenderBlackboard* render_blackboard, RenderScene* render_scene );
    void        destroy_gpu_resources( Renderer* renderer, RenderBlackboard* render_blackboard );

    void        on_resize( Renderer* renderer, RenderBlackboard* render_blackboard, u32 new_width, u32 new_height );

    void        upload_gpu_data( UploadGpuDataContext& context );

}; // struct GpuCullingRenderingFeature

//
struct MeshletsRenderingFeature {

    void        create_geometry_resources( Renderer* renderer, RenderBlackboard* render_blackboard, RenderScene* render_scene );
    void        create_gpu_resources( Renderer* renderer, RenderBlackboard* render_blackboard, RenderScene* render_scene );
    void        destroy_gpu_resources( Renderer* renderer, RenderBlackboard* render_blackboard );

    void        on_resize( Renderer* renderer, RenderBlackboard* render_blackboard, u32 new_width, u32 new_height );

    void        upload_gpu_data( UploadGpuDataContext& context );

}; // struct MeshletsRenderingFeature

//
struct LightingRenderingFeature {

    void        create_gpu_resources( Renderer* renderer, RenderBlackboard* render_blackboard );
    void        destroy_gpu_resources( Renderer* renderer, RenderBlackboard* render_blackboard );

    void        on_resize( Renderer* renderer, RenderBlackboard* render_blackboard, u32 new_width, u32 new_height );

    void        upload_gpu_data( UploadGpuDataContext& context );


    static const u32    k_light_z_bins = 16;
    static const u32    k_tile_size = 8;
    static const u32    k_light_mask_u32_count = ( k_num_lights + 31 ) / 32;


}; // struct LightingRenderingFeature


//
struct PointlightShadowsRenderingFeature {

    void        create_gpu_resources( Renderer* renderer, RenderBlackboard* render_blackboard, RenderScene* scene );
    void        destroy_gpu_resources( Renderer* renderer, RenderBlackboard* render_blackboard );

    void        on_resize( Renderer* renderer, RenderBlackboard* render_blackboard, u32 new_width, u32 new_height );

    void        upload_gpu_data( UploadGpuDataContext& context );
}; // struct PointlightShadowsRenderingFeature

//
struct DebugDrawRenderingFeature {

    //
    struct LineVertex {

        glm::vec3 position;
        Color   color;

        void    set( glm::vec3 position_, Color color_ ) { position = position_; color = color_; }
        void    set( glm::vec2 position_, Color color_ ) { position = { position_.x, position_.y, 0 }; color = color_; }
    }; // struct LineVertex


    void        create_gpu_resources( Renderer* renderer, RenderBlackboard* render_blackboard, FrameGraph* frame_graph );
    void        destroy_gpu_resources( Renderer* renderer, RenderBlackboard* render_blackboard );

    void        on_resize( Renderer* renderer, RenderBlackboard* render_blackboard, u32 new_width, u32 new_height );

    void        upload_gpu_data( UploadGpuDataContext& context );

    void        line( const glm::vec3& from, const glm::vec3& to, Color color );
    void        line_2d( const glm::vec2& from, const glm::vec2& to, Color color );
    void        line( const glm::vec3& from, const glm::vec3& to, Color color0, Color color1 );

    void        aabb( const glm::vec3& min, const glm::vec3 max, Color color );

    void        circle_wire( const glm::vec3& center, const glm::vec3& normal,
                             float radius, Color color, int segments = 32 );

    void        arc_wire( const glm::vec3& center, const glm::vec3& normal, float radius,
                          float start_angle_rad, float end_angle_rad, Color color,
                          int segments = 16 );

    void        sphere_wire( const glm::vec3& center, float radius, Color color,
                             int segments = 24, int latitude_rings = 0 );

    void        cone_wire( const glm::vec3& apex, const glm::vec3& axis,
                           float base_radius, Color color,
                           int segments = 24, int side_spokes = 8 );

    void        cylinder_wire( const glm::vec3& center0, const glm::vec3& center1,
                               float radius, Color color,
                               int segments = 24, int vertical_lines = 8 );

    void        plane_grid_wire( const glm::vec3& plane_point,
                                 const glm::vec3& plane_normal,
                                 float half_extent, float cell_size,
                                 Color color, int bold_every = 5,
                                 Color bold_color = Color::white() );

    void        plane_grid_wire( const glm::vec3& plane_point,
                                 const glm::vec3& plane_normal,
                                 float half_extent, float cell_size,
                                 Color color, int bold_every );

    void        frustum_wire( const glm::vec3 corners[ 8 ], Color color );

    void        frustum_wire_from_inv_viewproj( const glm::mat4& inv_view_proj, Color color );

    void        obb_wire( const glm::vec3& center, const glm::vec3& axis_x,
                          const glm::vec3& axis_y, const glm::vec3& axis_z,
                          const glm::vec3& half_extents, Color color );


    void        ray_wire( const glm::vec3& origin, const glm::vec3& dir,
                          float length, Color color );

    void        arrow_wire( const glm::vec3& origin, const glm::vec3& dir,
                            float length, float head_length,
                            float head_radius, Color color,
                            int head_segments = 12 );

    void        position_cross_wire( const glm::vec3& position,
                                     float size, Color color );

    void        axis_gizmo_wire( const glm::vec3& position, const glm::vec3& axis_x,
                                 const glm::vec3& axis_y, const glm::vec3& axis_z,
                                 float size );

    void        point_light_wire( const glm::vec3& position,
                                  float radius, Color color,
                                  float cross_size = 0.1f );

    Array<LineVertex> lines;
    Array<LineVertex> lines_2d;

}; // struct DebugDrawRenderingFeature

//
struct PostProcessRenderingFeature {

    void        update_psos( Renderer* renderer, FrameGraph* frame_graph, PipelineUpdatePhase phase );

    void        create_gpu_resources( Renderer* renderer, RenderBlackboard* render_blackboard, FrameGraph* frame_graph );
    void        destroy_gpu_resources( Renderer* renderer, RenderBlackboard* render_blackboard );

    void        on_resize( Renderer* renderer, RenderBlackboard* render_blackboard, u32 new_width, u32 new_height );

    void        upload_gpu_data( UploadGpuDataContext& context );

    DescriptorSetHandle     fullscreen_ds;

    GraphicsPipelineState   passthrough_pipeline;
    GraphicsPipelineState   main_post_pipeline;
    GraphicsPipelineState   main_post_pipeline_slang;

    u32                     post_cb_offset;

}; // struct PostProcessRenderingFeature

//
struct RayTracingScene {
    Array<BLASHandle>       blases;
    TLASHandle              tlas;

    BufferHandle            geometry_transform_buffer;
}; // struct RayTracingScene

//
struct RayTracingRenderFeature {

    void                    init( Allocator* allocator, Renderer* renderer );
    void                    shutdown( Renderer* renderer );

    void                    add_meshes_to_build_from_scene( RenderScene* scene );

    void                    build_acceleration_structures_from_scene( Renderer* renderer, RenderScene* scene, ArenaAllocator* scratch, f32 global_scale );
    void                    build_acceleration_structures_from_static_scene( Renderer* renderer, RenderScene* scene, ArenaAllocator* scratch, f32 global_scale );
    void                    build_or_update_tlas( Renderer* renderer, RenderScene* scene, ArenaAllocator* scratch, CommandBuffer* cb );

    void                    update_psos( Renderer* renderer, FrameGraph* frame_graph, PipelineUpdatePhase phase );

    void                    create_gpu_resources( Renderer* renderer, RenderBlackboard* render_blackboard, FrameGraph* frame_graph );
    void                    destroy_gpu_resources( Renderer* renderer, RenderBlackboard* render_blackboard );

    void                    on_resize( Renderer* renderer, RenderBlackboard* render_blackboard, u32 new_width, u32 new_height );

    void                    upload_gpu_data( UploadGpuDataContext& context );

    Array<u32>              blas_meshes_to_build;
    RayTracingScene         ray_tracing_scene;

    bool                    tlas_needs_rebuild = true;
}; // struct RayTracingRenderFeature

//
// FrameRenderer /////////////////////////////////////////////////////////
struct FrameRenderer {

    void                    init( Allocator* resident_allocator, Renderer* renderer,
                                  FrameGraph* frame_graph, SceneGraph* scene_graph,
                                  RenderScene* scene );
    void                    shutdown();

    void                    add_render_pass( cstring name, FrameGraphRenderPass* render_pass );
    void                    declare_frame_graph_structure( FrameGraph& frame_graph );
    void                    compile_passes_psos();

    void                    set_upscale_settings( const UpscaleSettings& settings ) { upscale_settings = settings; }
    void                    calculate_resolution_info( u32 swapchain_width, u32 swapchain_height );
    void                    on_resize( GpuDevice& gpu, u32 new_width, u32 new_height );

    void                    upload_gpu_data( GameCamera& game_camera, glm::vec2 last_clicked_position_left_button, ArenaAllocator* frame_scratch );
    void                    render( CommandBuffer* gpu_commands, RenderScene* render_scene );

    void                    create_resources( ArenaAllocator* scratch_allocator );
    void                    update_dependent_resources();

    void                    reload_psos();

    Allocator*              resident_allocator;
    SceneGraph*             scene_graph;

    Renderer*               renderer;
    FrameGraph*             frame_graph;

    RenderScene*            scene;
    RenderView              main_view;
    UpscaleSettings         upscale_settings;
    ResolutionInfo          resolution_info;

    MeshesRenderingFeature*         meshes      = nullptr;
    GpuCullingRenderingFeature*     gpu_culling = nullptr;
    LightingRenderingFeature*       lighting    = nullptr;
    MeshletsRenderingFeature*       meshlets    = nullptr;
    PostProcessRenderingFeature*    post        = nullptr;
    DebugDrawRenderingFeature*      debug_draw  = nullptr;
    PointlightShadowsRenderingFeature* point_shadows = nullptr;
    RayTracingRenderFeature*        ray_tracing = nullptr;

    TextureResource*        dither_4x4_texture  = nullptr;
    TextureResource*        dither_8x8_texture  = nullptr;
    TextureResource*        blue_noise_texture  = nullptr;

    RenderBlackboard        render_blackboard;
    RenderConfig            render_config;

    GpuFrameData            frame_data;

    Array<FrameGraphRenderPass*> render_passes;

    // TODO:
    Array<GpuMaterialData>  gpu_materials;
    Array<glm::vec4>        gpu_mesh_bounds;
    Array<glm::vec4>        gpu_mesh_aabbs;
    Array<GpuMeshInstanceData> gpu_mesh_instances;

}; // struct FrameRenderer

// DrawTask ///////////////////////////////////////////////////////////

//
//
struct DrawTask : public enki::ITaskSet {

    GpuDevice*              gpu         = nullptr;
    FrameGraph*             frame_graph = nullptr;
    Renderer*               renderer    = nullptr;
    ImGuiService*           imgui       = nullptr;
    GpuVisualProfiler*      gpu_visual_profiler = nullptr;
    RenderScene*            scene       = nullptr;
    FrameRenderer*          frame_renderer = nullptr;
    u32                     thread_id   = 0;
    // NOTE(marco): gpu state might change between init and execute!
    u32                     current_frame_index = 0;
    CommandBuffer*          gfx_cb      = nullptr;

    void init( GpuDevice* gpu_, FrameGraph* frame_graph_, Renderer* renderer_,
                ImGuiService* imgui_, GpuVisualProfiler* gpu_visual_profiler_, RenderScene* scene_,
                FrameRenderer* frame_renderer );

    void ExecuteRange( enki::TaskSetPartition range_, uint32_t threadnum_ ) override;

}; // struct DrawTask


// Math utils /////////////////////////////////////////////////////////
glm::vec3                   extract_scale( const glm::mat4& m );

void                        get_bounds_for_axis( const glm::vec3& a, const glm::vec3& C, float r, float nearZ, glm::vec3& L, glm::vec3& U );
void                        get_bounds_for_axis_rh( const glm::vec3& a_vs, const glm::vec3& C_vs, float r, float near_plane, glm::vec3& L_vs, glm::vec3& U_vs );

glm::vec3                   project( const glm::mat4& P, const glm::vec3& Q );

void                        project_aabb_cubemap_positive_x( const glm::vec3 aabb[ 2 ], f32& s_min, f32& s_max, f32& t_min, f32& t_max );
void                        project_aabb_cubemap_negative_x( const glm::vec3 aabb[ 2 ], f32& s_min, f32& s_max, f32& t_min, f32& t_max );
void                        project_aabb_cubemap_positive_y( const glm::vec3 aabb[ 2 ], f32& s_min, f32& s_max, f32& t_min, f32& t_max );
void                        project_aabb_cubemap_negative_y( const glm::vec3 aabb[ 2 ], f32& s_min, f32& s_max, f32& t_min, f32& t_max );
void                        project_aabb_cubemap_positive_z( const glm::vec3 aabb[ 2 ], f32& s_min, f32& s_max, f32& t_min, f32& t_max );
void                        project_aabb_cubemap_negative_z( const glm::vec3 aabb[ 2 ], f32& s_min, f32& s_max, f32& t_min, f32& t_max );

// Numerical sequences, used to calculate jittering values.
f32                         halton( i32 i, i32 b );
f32                         interleaved_gradient_noise( glm::vec2 pixel, i32 index );

glm::vec2                   halton23_sequence( i32 index );
glm::vec2                   m_robert_r2_sequence( i32 index );
glm::vec2                   interleaved_gradient_sequence( i32 index );
glm::vec2                   hammersley_sequence( i32 index, i32 num_samples );

VkTransformMatrixKHR        to_vk_transform_matrix( const glm::mat4& m );

} // namespace raptor
