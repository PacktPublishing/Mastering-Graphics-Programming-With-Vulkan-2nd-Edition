
#include "application/window.hpp"
#include "application/input.hpp"
#include "application/game_camera.hpp"

#include "graphics/gpu_device.hpp"
#include "graphics/command_buffer.hpp"
#include "graphics/gpu_profiler.hpp"
#include "graphics/raptor_imgui.hpp"
#include "graphics/renderer.hpp"
#include "graphics/render_scene.hpp"
#include "graphics/frame_graph.hpp"
#include "graphics/asynchronous_loader.hpp"
#include "graphics/scene_graph.hpp"
#include "graphics/render_resources_loader.hpp"
#include "graphics/frame_renderer.hpp"

#include "external/glm/vec2.hpp"
#include "external/glm/mat4x4.hpp"
#include "external/glm/matrix.hpp"
#include "external/enkiTS/TaskScheduler.h"
#include "external/json.hpp"

#include "foundation/file.hpp"
#include "foundation/numerics.hpp"
#include "foundation/time.hpp"
#include "foundation/resource_manager.hpp"

#include "external/imgui/imgui.h"
#include "external/stb_image.h"
#include "external/tracy/tracy/Tracy.hpp"

#include "graphics/render_passes/culling_pass.hpp"
#include "graphics/render_passes/ddgi_pass.hpp"
#include "graphics/render_passes/debug_pass.hpp"
#include "graphics/render_passes/depth_of_field_pass.hpp"
#include "graphics/render_passes/depth_pre_pass.hpp"
#include "graphics/render_passes/depth_pyramid_pass.hpp"
#include "graphics/render_passes/gbuffer_pass.hpp"
#include "graphics/render_passes/lighting_pass.hpp"
#include "graphics/render_passes/motion_vector_pass.hpp"
#include "graphics/render_passes/pointlight_shadow_pass.hpp"
#include "graphics/render_passes/raytraced_reflections_pass.hpp"
#include "graphics/render_passes/raytracing_test_pass.hpp"
#include "graphics/render_passes/shadow_visibility_pass.hpp"
#include "graphics/render_passes/svgf_pass.hpp"
#include "graphics/render_passes/temporal_anti_aliasing_pass.hpp"
#include "graphics/render_passes/transparent_pass.hpp"
#include "graphics/render_passes/volumetric_fog_pass.hpp"

#include <stdio.h>
#include <stdlib.h>

///////////////////////////////////////

// Input callback
static void input_os_messages_callback( void* os_event, void* user_data ) {
    raptor::InputService* input = ( raptor::InputService* )user_data;
    input->on_event( os_event );
}

// IOTasks ////////////////////////////////////////////////////////////////
//
//
struct RunPinnedTaskLoopTask : enki::IPinnedTask {

    void Execute() override {
        while ( !task_scheduler->GetIsShutdownRequested() && execute ) {
            task_scheduler->WaitForNewPinnedTasks(); // this thread will 'sleep' until there are new pinned tasks
            task_scheduler->RunPinnedTasks();
        }
    }

    enki::TaskScheduler*    task_scheduler;
    bool                    execute         = true;
}; // struct RunPinnedTaskLoopTask

//
//
struct AsynchronousLoadTask : enki::IPinnedTask {

    void Execute() override {
        // Do file IO
        while ( execute ) {
            async_loader->update();
        }
    }

    raptor::AsynchronousLoader* async_loader;
    enki::TaskScheduler*        task_scheduler;
    bool                        execute         = true;
}; // struct AsynchronousLoadTask

//
//
glm::vec4 normalize_plane( glm::vec4 plane ) {
    f32 len = glm::length( glm::vec3{ plane.x, plane.y, plane.z } );
    return plane / len;
}

f32 linearize_depth( f32 depth, f32 z_far, f32 z_near ) {
    return z_near * z_far / ( z_far + depth * ( z_near - z_far ) );
}

static void test_sphere_aabb( raptor::GameCamera& game_camera ) {
    glm::vec4 pos{ -14.5f, 1.28f, 0.f, 1.f };
    f32 radius = 0.5f;
    glm::vec4 view_space_pos = game_camera.camera.view * pos;
    bool camera_visible = view_space_pos.z < radius + game_camera.camera.near_plane;

    // X is positive, then it returns the same values as the longer method.
    glm::vec2 cx{ view_space_pos.x, -view_space_pos.z };
    glm::vec2 vx{ sqrtf( glm::dot( cx, cx ) - ( radius * radius ) ), radius };
    glm::mat2 xtransf_min{ vx.x, vx.y, -vx.y, vx.x };
    glm::vec2 minx = xtransf_min * cx;
    glm::mat2 xtransf_max{ vx.x, -vx.y, vx.y, vx.x };
    glm::vec2 maxx = xtransf_max * cx;

    glm::vec2 cy{ -view_space_pos.y, -view_space_pos.z };
    glm::vec2 vy{ sqrtf( glm::dot( cy, cy ) - ( radius * radius ) ), radius };
    glm::mat2 ytransf_min{ vy.x, vy.y, -vy.y, vy.x };
    glm::vec2 miny = ytransf_min * cy;
    glm::mat2 ytransf_max{ vy.x, -vy.y, vy.y, vy.x };
    glm::vec2 maxy = ytransf_max * cy;

    glm::vec4 aabb{ minx.x / minx.y * game_camera.camera.projection[0][0], miny.x / miny.y * game_camera.camera.projection[1][1],
               maxx.x / maxx.y * game_camera.camera.projection[0][0], maxy.x / maxy.y * game_camera.camera.projection[1][1] };
    glm::vec4 aabb2{ aabb.x * 0.5f + 0.5f, aabb.w * -0.5f + 0.5f, aabb.z * 0.5f + 0.5f, aabb.y * -0.5f + 0.5f };

    glm::vec3 left, right, top, bottom;
    raptor::get_bounds_for_axis( glm::vec3{ 1,0,0 }, { view_space_pos.x, view_space_pos.y, view_space_pos.z }, radius, game_camera.camera.near_plane, left, right );
    raptor::get_bounds_for_axis( glm::vec3{ 0,1,0 }, { view_space_pos.x, view_space_pos.y, view_space_pos.z }, radius, game_camera.camera.near_plane, top, bottom );

    left = raptor::project( game_camera.camera.projection, left );
    right = raptor::project( game_camera.camera.projection, right );
    top = raptor::project( game_camera.camera.projection, top );
    bottom = raptor::project( game_camera.camera.projection, bottom );

    glm::vec4 clip_space_pos = game_camera.camera.projection * view_space_pos;

    // left,right,bottom and top are in clip space (-1,1). Convert to 0..1 for UV, as used from the optimized version to read the depth pyramid.
    rprint( "Camera visible %u, x %f, %f, widh %f --- %f,%f width %f\n", camera_visible ? 1 : 0, aabb2.x, aabb2.z, aabb2.z - aabb2.x, left.x * 0.5 + 0.5, right.x * 0.5 + 0.5, ( left.x - right.x ) * 0.5 );
    rprint( "y %f, %f, height %f --- %f,%f height %f\n", aabb2.y, aabb2.w, aabb2.w - aabb2.y, top.y * 0.5 + 0.5, bottom.y * 0.5 + 0.5, ( top.y - bottom.y ) * 0.5 );
}

// Light placement function ///////////////////////////////////////////////
void place_lights( raptor::Array<raptor::Light>& lights, u32 active_lights, bool grid ) {

    using namespace raptor;

    if ( grid ) {
        const u32 lights_per_side = raptor::ceilu32( sqrtf( active_lights * 1.f ) );
        for ( u32 i = 0; i < active_lights; ++i ) {
            Light& light = lights[ i ];

            const f32 x = ( i % lights_per_side ) - lights_per_side * .5f;
            const f32 y = 0.05f;
            const f32 z = ( i / lights_per_side ) - lights_per_side * .5f;

            light.world_position = { x, y, z };
            light.intensity = 10.f;
            light.radius = 0.25f;
            light.color = { 1, 1, 1 };
        }
    }

    //// TODO(marco): we should take this into account when generating the lights positions
    //const float scale = 0.008f;

    //for ( u32 i = 0; i < k_num_lights; ++i ) {
    //    float x = get_random_value( mesh_aabb[ 0 ].x * scale, mesh_aabb[ 1 ].x * scale );
    //    float y = get_random_value( mesh_aabb[ 0 ].y * scale, mesh_aabb[ 1 ].y * scale );
    //    float z = get_random_value( mesh_aabb[ 0 ].z * scale, mesh_aabb[ 1 ].z * scale );

    //    float r = get_random_value( 0.0f, 1.0f );
    //    float g = get_random_value( 0.0f, 1.0f );
    //    float b = get_random_value( 0.0f, 1.0f );

    //    Light new_light{ };
    //    new_light.world_position = glm::vec3{ x, y, z };
    //    new_light.radius = 1.2f; // TODO(marco): random as well?

    //    new_light.color = glm::vec3{ r, g, b };
    //    new_light.intensity = 30.0f;

    //    lights.push( new_light );
    //}
}

//
u32 get_cube_face_mask( glm::vec3 cube_map_pos, glm::vec3 aabb[2] ) {

    glm::vec3 plane_normals[] = { {-1, 1, 0}, {1, 1, 0}, {1, 0, 1}, {1, 0, -1}, {0, 1, 1}, {0, -1, 1} };
    glm::vec3 abs_plane_normals[] = { {1, 1, 0}, {1, 1, 0}, {1, 0, 1}, {1, 0, 1}, {0, 1, 1}, {0, 1, 1} };

    glm::vec3 aabb_center = ( aabb[ 0 ] + aabb[ 1 ] ) * 0.5f;
    glm::vec3 center = aabb_center - cube_map_pos;
    glm::vec3 extents = ( aabb[ 1 ] - aabb[ 0 ] ) * 0.5f;

    bool rp[ 6 ];
    bool rn[ 6 ];

    for ( u32 i = 0; i < 6; ++i ) {
        f32 dist = glm::dot( center, plane_normals[ i ] );
        f32 radius = glm::dot( extents, abs_plane_normals[ i ] );

        rp[ i ] = dist > -radius;
        rn[ i ] = dist < radius;
    }

    u32 fpx = rn[ 0 ] && rp[ 1 ] && rp[ 2 ] && rp[ 3 ] && aabb[ 1 ].x > cube_map_pos.x;
    u32 fnx = rp[ 0 ] && rn[ 1 ] && rn[ 2 ] && rn[ 3 ] && aabb[ 0 ].x < cube_map_pos.x;
    u32 fpy = rp[ 0 ] && rp[ 1 ] && rp[ 4 ] && rn[ 5 ] && aabb[ 1 ].y > cube_map_pos.y;
    u32 fny = rn[ 0 ] && rn[ 1 ] && rn[ 4 ] && rp[ 5 ] && aabb[ 0 ].y < cube_map_pos.y;
    u32 fpz = rp[ 2 ] && rn[ 3 ] && rp[ 4 ] && rp[ 5 ] && aabb[ 1 ].z > cube_map_pos.z;
    u32 fnz = rn[ 2 ] && rp[ 3 ] && rn[ 4 ] && rn[ 5 ] && aabb[ 0 ].z < cube_map_pos.z;

    return fpx | ( fnx << 1 ) | ( fpy << 2 ) | ( fny << 3 ) | ( fpz << 4 ) | ( fnz << 5 );
}

static void perform_geometric_tests( bool enable_aabb_cubemap_test, raptor::RenderScene* scene, const glm::vec3& aabb_test_position,
                                     raptor::GpuFrameData& scene_data, bool freeze_occlusion_camera, raptor::GameCamera& game_camera,
                                     bool enable_light_tile_debug, raptor::Allocator* allocator, bool enable_light_cluster_debug,
                                     raptor::DebugDrawRenderingFeature& debug_draw ) {

    using namespace raptor;

    //f32 distance = glms_vec3_distance( { 0,0,0 }, light.world_position );
    //f32 distance_normalized = distance / (half_radius * 2.f);
    //f32 f = half_radius * 2;
    //f32 n = 0.01f;
    //float NormZComp = ( f + n ) / ( f - n ) - ( 2 * f * n ) / ( f - n ) / distance;
    //float NormZComp2 = ( f ) / ( n - f ) - ( f * n ) / ( n - f ) / distance;

    //// return z_near * z_far / (z_far + depth * (z_near - z_far));
    //f32 linear_d = n * f / ( f + 0.983 * ( n - f ) );
    //f32 linear_d2 = n * f / ( f + 1 * ( n - f ) );
    //f32 linear_d3 = n * f / ( f + 0.01 * ( n - f ) );

    //// ( f + z * ( n - f ) ) * lin_z = n * f;
    //// f * lin_z + (z * lin_z * (n - f ) = n * f
    //// ((n * f) - f * lin_z ) / (n - f) = z * lin_z

    //NormZComp = ( f + n ) / ( f - n ) - ( 2 * f * n ) / ( f - n ) / n;
    //NormZComp = ( f + n ) / ( f - n ) - ( 2 * f * n ) / ( f - n ) / f;
    //NormZComp2 = -( f ) / ( n - f ) - ( f * n ) / ( n - f ) / n;
    //NormZComp2 = -( f ) / ( n - f ) - ( f * n ) / ( n - f ) / f;

    //glm::mat4 view = glms_look( light.world_position, { 0,0,-1 }, { 0,-1,0 } );
    //// TODO: this should be radius of the light.
    //glm::mat4 projection = glms_perspective( glm_rad( 90.f ), 1.f, 0.01f, light.radius );
    //glm::mat4 view_projection = glms_mat4_mul( projection, view );

    //glm::vec3 pos_cs = project( view_projection, { 0,0,0 } );

    //rprint( "DDDD %f %f %f %f\n", NormZComp, -NormZComp2, linear_d, pos_cs.z );
    //{
    //    float fn = 1.0f / ( 0.01f - light.radius );
    //    float a = ( 0.01f + light.radius ) * fn;
    //    float b = 2.0f * 0.01f * light.radius * fn;
    //    float projectedDistance = light.world_position.z;
    //    float z = projectedDistance * a + b;
    //    float dbDistance = z / projectedDistance;

    //    float bc = dbDistance - NormZComp;
    //    float bd = dbDistance - NormZComp2;
    //}


    // Test AABB cubemap intersection method
    if ( enable_aabb_cubemap_test ) {
        // Draw enclosing cubemap aabb
        glm::vec3 cubemap_position = { 0.f, 0.f, 0.f };
        glm::vec3 cubemap_half_size = { 1, 1, 1 };
        debug_draw.aabb( cubemap_position - cubemap_half_size, cubemap_position + cubemap_half_size, Color::blue() );

        glm::vec3 aabb[] = { aabb_test_position - glm::vec3{ 0.2f }, aabb_test_position + glm::vec3{ 0.2f } };
        u32 res = get_cube_face_mask( cubemap_position, aabb );
        // Positive X
        if ( ( res & 1 ) ) {
            debug_draw.aabb( cubemap_position + glm::vec3{ 1,0,0 }, cubemap_position + glm::vec3{ 1.2f, .2f, .2f }, { Color::get_distinct_color( 0 ) } );
        }
        // Negative X
        if ( ( res & 2 ) ) {
            debug_draw.aabb( cubemap_position + glm::vec3{ -1,0,0 }, cubemap_position + glm::vec3{ -1.2f, -.2f, -.2f }, { Color::get_distinct_color( 1 ) } );
        }
        // Positive Y
        if ( ( res & 4 ) ) {
            debug_draw.aabb( cubemap_position + glm::vec3{ 0,1,0 }, cubemap_position + glm::vec3{ .2f, 1.2f, .2f }, { Color::get_distinct_color( 2 ) } );
        }
        // Negative Y
        if ( ( res & 8 ) ) {
            debug_draw.aabb( cubemap_position + glm::vec3{ 0,-1,0 }, cubemap_position + glm::vec3{ .2f, -1.2f, .2f }, { Color::get_distinct_color( 3 ) } );
        }
        // Positive Z
        if ( ( res & 16 ) ) {
            debug_draw.aabb( cubemap_position + glm::vec3{ 0,0,1 }, cubemap_position + glm::vec3{ .2f, .2f, 1.2f }, { Color::get_distinct_color( 4 ) } );
        }
        // Negative Z
        if ( ( res & 32 ) ) {
            debug_draw.aabb( cubemap_position + glm::vec3{ 0,0,-1 }, cubemap_position + glm::vec3{ .2f, .2f, -1.2f }, { Color::get_distinct_color( 5 ) } );
        }
        // Draw aabb to test inside cubemap
        debug_draw.aabb( aabb[ 0 ], aabb[ 1 ], Color::white() );
        //debug_draw.line( { -1,-1,-1 }, { 1,1,1 }, { Color::white } );
        //debug_draw.line( { -1,-1,1 }, { 1,1,-1 }, { Color::white } );

        /*debug_draw.line({0.5,0,-0.5}, {-1 + .5,1,0 - .5}, {Color::blue});
        debug_draw.line( { -0.5,0,-0.5 }, { 1 - .5,1,0 - .5 }, { Color::green } );
        debug_draw.line( { 0,0,0 }, { 1,0,1 }, { Color::red } );
        debug_draw.line( { 0,0,0 }, { 1,0,-1 }, { Color::yellow } );
        debug_draw.line( { 0,0,0 }, { 0,1,1 }, { Color::white } );
        debug_draw.line( { 0,0,0 }, { 0,-1,1 }, { 0xffffff00 } ); */

        // AABB -> cubemap face rectangle test
        f32 s_min, s_max, t_min, t_max;
        project_aabb_cubemap_positive_x( aabb, s_min, s_max, t_min, t_max );
        //rprint( "POS X s %f,%f | t %f,%f\n", s_min, s_max, t_min, t_max );
        project_aabb_cubemap_negative_x( aabb, s_min, s_max, t_min, t_max );
        //rprint( "NEG X s %f,%f | t %f,%f\n", s_min, s_max, t_min, t_max );
        project_aabb_cubemap_positive_y( aabb, s_min, s_max, t_min, t_max );
        //rprint( "POS Y s %f,%f | t %f,%f\n", s_min, s_max, t_min, t_max );
        project_aabb_cubemap_negative_y( aabb, s_min, s_max, t_min, t_max );
        //rprint( "NEG Y s %f,%f | t %f,%f\n", s_min, s_max, t_min, t_max );
        project_aabb_cubemap_positive_z( aabb, s_min, s_max, t_min, t_max );
        //rprint( "POS Z s %f,%f | t %f,%f\n", s_min, s_max, t_min, t_max );
        project_aabb_cubemap_negative_z( aabb, s_min, s_max, t_min, t_max );
        //rprint( "NEG Z s %f,%f | t %f,%f\n", s_min, s_max, t_min, t_max );
    }

    if ( false ) {
        // NOTE(marco): adpated from http://www.aortiz.me/2018/12/21/CG.html#clustered-shading
        const u32 z_count = 32;
        const f32 tile_size = 64.0f;
        const f32 tile_pixels = tile_size * tile_size;
        const u32 tile_x_count = u32( scene_data.resolution_x / f32( tile_size ) );
        const u32 tile_y_count = u32( scene_data.resolution_y / f32( tile_size ) );

        const f32 tile_radius_sq = ( ( tile_size * 0.5f ) * ( tile_size * 0.5f ) ) * 2;

        const glm::vec3 eye_pos = glm::vec3{ 0, 0, 0 };

        static Camera last_camera{ };

        if ( !freeze_occlusion_camera ) {
            last_camera = game_camera.camera;
        }

        glm::mat4 inverse_projection = glm::inverse( last_camera.projection );
        glm::mat4 inverse_view = glm::inverse( last_camera.view );

        auto screen_to_view = [&]( const glm::vec4& screen_pos ) -> glm::vec3 {
            //Convert to NDC
            glm::vec2 text_coord{ screen_pos.x / scene_data.resolution_x, screen_pos.y / scene_data.resolution_y };

            //Convert to clipSpace
            glm::vec4 clip = glm::vec4{ text_coord.x * 2.0f - 1.0f,
                                ( 1.0f - text_coord.y ) * 2.0f - 1.0f,
                                screen_pos.z,
                                screen_pos.w };

            //View space transform
            glm::vec4 view = inverse_projection * clip;

            //Perspective projection
            // view = glms_vec4_scale( view, 1.0f / view.w );

            return glm::vec3{ view.x, view.y, view.z };
        };

        auto line_intersection_to_z_plane = [&]( const glm::vec3& a, const glm::vec3& b, f32 z ) -> glm::vec3 {
            //all clusters planes are aligned in the same z direction
            glm::vec3 normal = glm::vec3{ 0.0, 0.0, 1.0 };

            //getting the line from the eye to the tile
            glm::vec3 ab = b - a;

            //Computing the intersection length for the line and the plane
            f32 t = ( z - glm::dot( normal, a ) ) / glm::dot( normal, ab );

            //Computing the actual xyz position of the point along the line
            glm::vec3 result = a + ab * t;

            return result;
        };

        const f32 z_near = scene_data.z_near;
        const f32 z_far = scene_data.z_far;
        const f32 z_ratio = z_far / z_near;
        const f32 z_bin_range = 1.0f / f32( z_count );

        u32 light_count = scene->active_lights;

        Array<glm::vec3> lights_aabb_view;
        lights_aabb_view.init( allocator, light_count * 2, light_count * 2 );

        for ( u32 l = 0; l < light_count; ++l ) {
            Light& light = scene->lights[ l ];
            light.shadow_map_resolution = 0.0f;
            light.tile_x = 0;
            light.tile_y = 0;
            light.solid_angle = 0.0f;

            glm::vec4 aabb_min_view = last_camera.view * light.aabb_min;
            glm::vec4 aabb_max_view = last_camera.view * light.aabb_max;

            lights_aabb_view[ l * 2 ] = glm::vec3{ aabb_min_view.x, aabb_min_view.y, aabb_min_view.z };
            lights_aabb_view[ l * 2 + 1 ] = glm::vec3{ aabb_max_view.x, aabb_max_view.y, aabb_max_view.z };
        }

        for ( u32 z = 0; z < z_count; ++z ) {
            for ( u32 y = 0; y < tile_y_count; ++y ) {
                for ( u32 x = 0; x < tile_x_count; ++x ) {
                    // Calculating the min and max point in screen space
                    glm::vec4 max_point_screen = glm::vec4{ f32( ( x + 1 ) * tile_size ),
                                                    f32( ( y + 1 ) * tile_size ),
                                                    0.0f, 1.0f }; // Top Right

                    glm::vec4 min_point_screen = glm::vec4{ f32( x * tile_size ),
                                                    f32( y * tile_size ),
                                                    0.0f, 1.0f }; // Top Right

                    glm::vec4 tile_center_screen = ( min_point_screen + max_point_screen ) * 0.5f;
                    glm::vec2 tile_center{ tile_center_screen.x, tile_center_screen.y };

                    // Pass min and max to view space
                    glm::vec3 max_point_view = screen_to_view( max_point_screen );
                    glm::vec3 min_point_view = screen_to_view( min_point_screen );

                    // Near and far values of the cluster in view space
                    // We use equation (2) directly to obtain the tile values
                    f32 tile_near = z_near * pow( z_ratio, f32( z ) * z_bin_range );
                    f32 tile_far = z_near * pow( z_ratio, f32( z + 1 ) * z_bin_range );

                    // Finding the 4 intersection points made from each point to the cluster near/far plane
                    glm::vec3 min_point_near = line_intersection_to_z_plane( eye_pos, min_point_view, tile_near );
                    glm::vec3 min_point_far = line_intersection_to_z_plane( eye_pos, min_point_view, tile_far );
                    glm::vec3 max_point_near = line_intersection_to_z_plane( eye_pos, max_point_view, tile_near );
                    glm::vec3 max_point_far = line_intersection_to_z_plane( eye_pos, max_point_view, tile_far );

                    glm::vec3 min_point_aabb_view = glm::min( glm::min( min_point_near, min_point_far ), glm::min( max_point_near, max_point_far ) );
                    glm::vec3 max_point_aabb_view = glm::max( glm::max( min_point_near, min_point_far ), glm::max( max_point_near, max_point_far ) );

                    glm::vec4 min_point_aabb_world{ min_point_aabb_view.x, min_point_aabb_view.y, min_point_aabb_view.z, 1.0f };
                    glm::vec4 max_point_aabb_world{ max_point_aabb_view.x, max_point_aabb_view.y, max_point_aabb_view.z, 1.0f };

                    min_point_aabb_world = inverse_view * min_point_aabb_world;
                    max_point_aabb_world = inverse_view * max_point_aabb_world;

                    bool intersects_light = false;
                    for ( u32 l = 0; l < scene->active_lights; ++l ) {
                        Light& light = scene->lights[ l ];

                        glm::vec3& light_aabb_min = lights_aabb_view[ l * 2 ];
                        glm::vec3& light_aabb_max = lights_aabb_view[ l * 2 + 1 ];

                        f32 minx = min( min( light_aabb_min.x, light_aabb_max.x ), min( min_point_aabb_view.x, max_point_aabb_view.x ) );
                        f32 miny = min( min( light_aabb_min.y, light_aabb_max.y ), min( min_point_aabb_view.y, max_point_aabb_view.y ) );
                        f32 minz = min( min( light_aabb_min.z, light_aabb_max.z ), min( min_point_aabb_view.z, max_point_aabb_view.z ) );

                        f32 maxx = max( max( light_aabb_min.x, light_aabb_max.x ), max( min_point_aabb_view.x, max_point_aabb_view.x ) );
                        f32 maxy = max( max( light_aabb_min.y, light_aabb_max.y ), max( min_point_aabb_view.y, max_point_aabb_view.y ) );
                        f32 maxz = max( max( light_aabb_min.z, light_aabb_max.z ), max( min_point_aabb_view.z, max_point_aabb_view.z ) );

                        f32 dx = abs( maxx - minx );
                        f32 dy = abs( maxy - miny );
                        f32 dz = abs( maxz - minz );

                        f32 allx = abs( light_aabb_max.x - light_aabb_min.x ) + abs( max_point_aabb_view.x - min_point_aabb_view.x );
                        f32 ally = abs( light_aabb_max.y - light_aabb_min.y ) + abs( max_point_aabb_view.y - min_point_aabb_view.y );
                        f32 allz = abs( light_aabb_max.z - light_aabb_min.z ) + abs( max_point_aabb_view.z - min_point_aabb_view.z );

                        bool intersects = ( dx <= allx ) && ( dy < ally ) && ( dz <= allz );

                        if ( intersects ) {
                            intersects_light = true;

                            glm::vec4 sphere_world{ light.world_position.x, light.world_position.y, light.world_position.z, 1.0f };
                            glm::vec4 sphere_ndc = last_camera.view_projection * sphere_world;

                            sphere_ndc.x /= sphere_ndc.w;
                            sphere_ndc.y /= sphere_ndc.w;

                            glm::vec2 sphere_screen{ ( ( sphere_ndc.x + 1.0f ) * 0.5f ) * scene_data.resolution_x, ( ( sphere_ndc.y + 1.0f ) * 0.5f ) * scene_data.resolution_y, };

                            f32 d = glm::distance( sphere_screen, tile_center );

                            f32 diff = d * d - tile_radius_sq;

                            if ( diff < 1.0e-4 ) {
                                continue;
                            }

                            // NOTE(marco): as defined in https://math.stackexchange.com/questions/73238/calculating-solid-angle-for-a-sphere-in-space
                            f32 solid_angle = ( 2.0f * rpi ) * ( 1.0f - ( sqrtf( diff ) / d ) );

                            // NOTE(marco): following https://efficientshading.com/wp-content/uploads/s2015_shadows.pdf
                            f32 resolution = sqrtf( ( 4.0f * rpi * tile_pixels ) / ( 6 * solid_angle ) );

                            if ( resolution > light.shadow_map_resolution ) {
                                light.shadow_map_resolution = resolution;
                                light.tile_x = x;
                                light.tile_y = y;
                                light.solid_angle = solid_angle;
                            }
                        }
                    }

                    if ( enable_light_cluster_debug && intersects_light ) {
                        debug_draw.aabb( glm::vec3{ min_point_aabb_world.x, min_point_aabb_world.y, min_point_aabb_world.z },
                                                    glm::vec3{ max_point_aabb_world.x, max_point_aabb_world.y, max_point_aabb_world.z },
                                                    { Color::get_distinct_color( z ) } );
                    }
                }
            }
        }

        lights_aabb_view.shutdown();

        if ( enable_light_tile_debug ) {
            f32 light_pos_len = 0.01f;
            for ( u32 l = 0; l < light_count; ++l ) {
                Light& light = scene->lights[ l ];

                //rprint( "Light resolution %f\n", light.shadow_map_resolution );

                if ( light.shadow_map_resolution != 0.0f ) {
                    {
                        glm::vec4 sphere_world{ light.world_position.x, light.world_position.y, light.world_position.z, 1.0f };
                        glm::vec4 sphere_ndc = last_camera.view_projection * sphere_world;

                        sphere_ndc.x /= sphere_ndc.w;
                        sphere_ndc.y /= sphere_ndc.w;

                        glm::vec2 top_left{ sphere_ndc.x - light_pos_len, sphere_ndc.y - light_pos_len };
                        glm::vec2 bottom_right{ sphere_ndc.x + light_pos_len, sphere_ndc.y + light_pos_len };
                        glm::vec2 top_right{ sphere_ndc.x + light_pos_len, sphere_ndc.y - light_pos_len };
                        glm::vec2 bottom_left{ sphere_ndc.x - light_pos_len, sphere_ndc.y + light_pos_len };

                        debug_draw.line_2d( top_left, bottom_right, { Color::get_distinct_color( l + 1 ) } );
                        debug_draw.line_2d( top_right, bottom_left, { Color::get_distinct_color( l + 1 ) } );
                    }

                    {
                        glm::vec2 screen_scale{ 1.0f / f32( scene_data.resolution_x ), 1.0f / ( scene_data.resolution_y ) };

                        glm::vec2 bottom_right{ f32( ( light.tile_x + 1 ) * tile_size ), f32( scene_data.resolution_y - ( light.tile_y + 1 ) * tile_size ) };
                        bottom_right = ( ( bottom_right * screen_scale ) * 2.0f ) - 1.0f;

                        glm::vec2 top_left{ f32( ( light.tile_x ) * tile_size ), f32( scene_data.resolution_y - ( light.tile_y ) * tile_size ) };
                        top_left = ( ( top_left * screen_scale ) * 2.0f ) - 1.0f;

                        glm::vec2 top_right{ bottom_right.x, top_left.y };
                        glm::vec2 bottom_left{ top_left.x, bottom_right.y };

                        debug_draw.line_2d( top_left, top_right, { Color::get_distinct_color( l + 1 ) } );
                        debug_draw.line_2d( top_right, bottom_right, { Color::get_distinct_color( l + 1 ) } );
                        debug_draw.line_2d( bottom_left, bottom_right, { Color::get_distinct_color( l + 1 ) } );
                        debug_draw.line_2d( bottom_left, top_left, { Color::get_distinct_color( l + 1 ) } );
                    }
                }
            }
        }
    }
}

// Enums
namespace JitterType {
    enum Enum {
        Halton = 0,
        R2,
        Hammersley,
        InterleavedGradients
    };

    cstring names[] = { "Halton", "Martin Robert R2", "Hammersley", "Interleaved Gradients"};
} // namespace JitterType

//
//
int main( int argc, char** argv ) {

    if ( argc < 2 ) {
        printf( "Usage: chapter15 [path to glTF model]\n");
        InjectDefault3DModel();
    }

    using namespace raptor;

    time_service_init();

    // Init services
    MemoryServiceConfiguration memory_configuration;
    memory_configuration.maximum_dynamic_size = rgiga( 2ull );

    MemoryService::instance()->init( &memory_configuration );
    Allocator* allocator = &MemoryService::instance()->system_allocator;

    ArenaAllocator scratch_allocator;
    scratch_allocator.init( rmega( 8 ) );

    enki::TaskSchedulerConfig config;
    // In this example we create more threads than the hardware can run,
    // because the IO thread will spend most of it's time idle or blocked
    // and therefore not scheduled for CPU time by the OS
    config.numTaskThreadsToCreate += 1;
    enki::TaskScheduler task_scheduler;

    task_scheduler.Initialize( config );

    // window
    WindowConfiguration wconf{ 1280, 800, "Raptor Chapter 15: RT Reflections", &MemoryService::instance()->system_allocator};
    raptor::Window window;
    window.init( &wconf );

    InputService input;
    input.init( allocator );

    // Callback register: input needs to react to OS messages.
    window.register_os_messages_callback( input_os_messages_callback, &input );

    // graphics
    GpuDeviceCreation dc;
    dc.set_window( window.width, window.height, window.platform_handle ).set_allocator( &MemoryService::instance()->system_allocator )
      .set_num_threads( task_scheduler.GetNumTaskThreads() );
    dc.enable_bindless = true;
    dc.enable_ray_tracing = true;
    dc.enable_vrs = false;

    // Allocate specific resource pool sizes
    dc.resource_pool_creation.buffers = 4096;
    dc.resource_pool_creation.descriptor_set_layouts = 256;
    dc.resource_pool_creation.descriptor_sets = 1024;
    dc.resource_pool_creation.pipelines = 256;
    dc.resource_pool_creation.shaders = 256;
    dc.resource_pool_creation.samplers = 128;
    dc.resource_pool_creation.images = 1024;
    dc.descriptor_pool_creation.combined_image_samplers = 1024;
    dc.descriptor_pool_creation.storage_texel_buffers = 1;
    dc.descriptor_pool_creation.uniform_texel_buffers = 1;

    GpuDevice gpu;
    gpu.init( dc );

    ResourceManager rm;
    rm.init( allocator, nullptr );

    GpuVisualProfiler gpu_profiler;
    gpu_profiler.init( allocator, gpu.gpu_timestamp_frequency, 100, dc.gpu_time_queries_per_frame );

    RendererResourcePoolCreation rrpc{ };
    rrpc.textures = dc.resource_pool_creation.images;
    rrpc.buffers = dc.resource_pool_creation.buffers;
    rrpc.samplers = dc.resource_pool_creation.samplers;

    Renderer renderer;
    renderer.init( { &gpu, allocator, rrpc } );
    renderer.set_loaders( &rm );

    {
        ArenaScope scoped_allocator( &scratch_allocator );
        StringBuffer temporary_name_buffer;
        temporary_name_buffer.init( 1024, scoped_allocator.allocator );

        // Create binaries folders
        cstring shader_binaries_folder = temporary_name_buffer.append_use_f( "%s/shaders/", RAPTOR_DATA_FOLDER );
        if ( !directory_exists( shader_binaries_folder ) ) {
            if ( directory_create( shader_binaries_folder ) ) {
                rprint( "Created folder %s\n", shader_binaries_folder );
            } else {
                rprint( "Cannot create folder %s\n" );
            }
        }
        strcpy( renderer.resource_cache.binary_data_folder, shader_binaries_folder );
    }

    ImGuiService* imgui = ImGuiService::instance();
    ImGuiServiceConfiguration imgui_config{ &gpu, &renderer, window.platform_handle };
    imgui->init( &imgui_config );

    GameCamera game_camera;
    game_camera.camera.init_perpective( 0.1f, 100.f, 60.f, wconf.width * 1.f / wconf.height );
    game_camera.init( true, 20.f, 6.f, 0.1f );

    //RenderResourcesLoader render_resources_loader;

    sizet scratch_marker = scratch_allocator.get_marker();

    StringBuffer temporary_name_buffer;
    temporary_name_buffer.init( 1024, &scratch_allocator );

    // Create binaries folders
    cstring shader_binaries_folder = temporary_name_buffer.append_use_f( "%s/shaders/", RAPTOR_DATA_FOLDER );
    if ( !directory_exists(shader_binaries_folder) ) {
        if ( directory_create( shader_binaries_folder ) ) {
            rprint( "Created folder %s\n", shader_binaries_folder );
        }
        else {
            rprint( "Cannot create folder %s\n" );
        }
    }
    strcpy( renderer.resource_cache.binary_data_folder, shader_binaries_folder );
    temporary_name_buffer.clear();

    SceneGraph scene_graph;
    scene_graph.init( allocator, 4 );

    // [TAG: Multithreading]
    AsynchronousLoader async_loader;
    async_loader.init( &renderer, allocator );

    Directory cwd{ };
    directory_current(&cwd);

    RenderScene render_scene{ };
    render_scene.init( &scene_graph, allocator, &renderer );

    for ( i32 arg_i = 1; arg_i < argc; ++arg_i ) {
        cstring scene_path = argv[ arg_i ];
        sizet scene_path_len = strlen( argv[ arg_i ] );

        char file_base_path[ 512 ]{ };
        memcpy( file_base_path, scene_path, scene_path_len );
        file_directory_from_path( file_base_path );

        directory_change( file_base_path );

        char file_name[ 512 ]{ };
        memcpy( file_name, scene_path, scene_path_len );
        file_name_from_path( file_name );

        render_scene.add_model( file_name, file_base_path, &scratch_allocator );
    }

    // NOTE(marco): restore working directory
    directory_change( cwd.path );

    FrameGraphBuilder frame_graph_builder;
    frame_graph_builder.init( &gpu );

    FrameGraph frame_graph;
    frame_graph.init( &frame_graph_builder );

    FrameRenderer frame_renderer;
    frame_renderer.init( allocator, &renderer, &frame_graph, &scene_graph, &render_scene );

    if ( gpu.fragment_shading_rate_present )
    {
        ImageCreation texture_creation{ };
        u32 adjusted_width = ( window.width + gpu.min_fragment_shading_rate_texel_size.width - 1 ) / gpu.min_fragment_shading_rate_texel_size.width;
        u32 adjusted_height = ( window.height + gpu.min_fragment_shading_rate_texel_size.height - 1 ) / gpu.min_fragment_shading_rate_texel_size.height;
        texture_creation.set_size( adjusted_width, adjusted_height, 1 ).set_format_type( VK_FORMAT_R8_UINT, TextureType::Texture2D ).set_mips( 1 ).set_layers( 1 ).set_flags( TextureFlags::Compute_mask | TextureFlags::ShadingRate_mask ).set_name( "fragment_shading_rate" );

        frame_renderer.render_blackboard.fragment_shading_rate_image = gpu.create_image( texture_creation );
        frame_renderer.render_blackboard.fragment_shading_rate_image_view = gpu.create_image_view( {
            .parent_image = frame_renderer.render_blackboard.fragment_shading_rate_image, .view_type = VK_IMAGE_VIEW_TYPE_2D,
            .sub_resource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }, .name = texture_creation.name } );

        gpu.add_image_view_to_bindless( frame_renderer.render_blackboard.fragment_shading_rate_image_view );

        FrameGraphResourceInfo resource_info{ };
        resource_info.set_external_texture_2d( adjusted_width, adjusted_height, VK_FORMAT_R8_UINT, 0, frame_renderer.render_blackboard.fragment_shading_rate_image, frame_renderer.render_blackboard.fragment_shading_rate_image_view );
        frame_graph.add_resource( "shading_rate_image", FrameGraphResourceType_ShadingRate, resource_info );
    }

    static bool use_shader_cache = true;
    static cstring techniques[] = { "reflections.json", "ray_tracing.json",
                                    "fullscreen.json", "main.json",
                                    "pbr_lighting.json", "dof.json", "cloth.json", "debug.json",
                                    "culling.json" };

    static bool changed_techniques[ ArraySize( techniques ) ];
    // Single Gpu Technique parsing.
    /*auto load_technique = [ & ]( cstring technique_name, bool& shader_changed ) {
        temporary_name_buffer.clear();
        cstring path = temporary_name_buffer.append_use_f( "%s/%s", RAPTOR_SHADER_FOLDER, technique_name );
        render_resources_loader.load_gpu_technique( path, use_shader_cache, shader_changed, allocator );
    };

    auto reload_technique = [ & ]( cstring technique_name, bool& shader_changed ) {
        temporary_name_buffer.clear();
        cstring path = temporary_name_buffer.append_use_f( "%s/%s", RAPTOR_SHADER_FOLDER, technique_name );
        render_resources_loader.reload_gpu_technique( path, use_shader_cache, shader_changed, allocator );
    };*/

    // Gpu Technique collection parsing
    //auto load_all_techniques = [ & ]() {
    //    const sizet num_techniques = ArraySize( techniques );
    //    for ( sizet t = 0; t < num_techniques; ++t ) {
    //        load_technique( techniques[ t ], changed_techniques[ t ] );
    //    }

    //    RASSERT( false );
    //    //frame_graph_builder.get_render_pass<FrameGraphRenderPass>( "gbuffer_pass_early" )->compile_psos( &renderer );
    //    //frame_graph_builder.get_render_pass<FrameGraphRenderPass>( "volumetric_fog_pass" )->compile_psos( &renderer );
    //    //frame_graph_builder.get_render_pass<FrameGraphRenderPass>( "indirect_lighting_pass" )->compile_psos( &renderer );
    //};

    //auto reload_all_techniques = [ & ]() {
    //    const sizet num_techniques = ArraySize( techniques );
    //    for ( sizet t = 0; t < num_techniques; ++t ) {
    //        reload_technique( techniques[ t ], changed_techniques[ t ] );
    //    }
    //};

    TextureResource* dither_texture = nullptr;
    TextureResource* blue_noise_128_rg_texture = nullptr;
    SamplerHandle repeat_sampler, repeat_nearest_sampler;
    // Load frame graph and parse gpu techniques
    {
        cstring frame_graph_path = temporary_name_buffer.append_use_f( "%s/%s", RAPTOR_WORKING_FOLDER, "graph_ray_tracing.json" );

        frame_graph.parse( frame_graph_path, &scratch_allocator );
        frame_graph.compile();

        FrameGraphNode* point_shadows_pass_node = frame_graph.get_node( "point_shadows_pass" );
        if ( point_shadows_pass_node ) {
            point_shadows_pass_node->render_pass_output.reset().depth( VK_FORMAT_D16_UNORM, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL );
        }
        // Cache frame graph resources in scene
        FrameGraphResource* resource = frame_graph.get_resource( "motion_vectors" );
        if ( resource ) {
            frame_renderer.render_blackboard.motion_vector_image_view = resource->resource_info.texture.image_view;
        }

        resource = frame_graph.get_resource( "visibility_motion_vectors" );
        if ( resource ) {
            frame_renderer.render_blackboard.visibility_motion_vector_image_view = resource->resource_info.texture.image_view;
        }

       // render_resources_loader.init( &renderer, &scratch_allocator, &frame_graph );

        SamplerCreation sampler_creation;
        sampler_creation.set_address_mode_uv( VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT )
            .set_min_mag_mip( VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR ).set_name( "repeat_sampler" );
        repeat_sampler = gpu.create_sampler( sampler_creation );

        sampler_creation.set_min_mag_mip( VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST ).set_name( "repeat_nearest_sampler" );
        repeat_nearest_sampler = gpu.create_sampler( sampler_creation );

        // TODO: add this to render graph itself.
        // Add utility textures (dithering, blue noise ...)
        temporary_name_buffer.clear();
        cstring dither_texture_path = temporary_name_buffer.append_use_f( "%s/BayerDither4x4.png", RAPTOR_DATA_FOLDER );
      //  dither_texture = render_resources_loader.load_texture( dither_texture_path, false );

        gpu.link_image_sampler( dither_texture->image, repeat_nearest_sampler );

        temporary_name_buffer.clear();
        cstring blue_noise_texture_path = temporary_name_buffer.append_use_f( "%s/LDR_RG01_0.png", RAPTOR_DATA_FOLDER );
      //  blue_noise_128_rg_texture = render_resources_loader.load_texture( blue_noise_texture_path, false );

        gpu.link_image_sampler( blue_noise_128_rg_texture->image, repeat_sampler );

        frame_renderer.render_blackboard.blue_noise_128_rg_image_view_index = blue_noise_128_rg_texture->image_view.index();

        frame_renderer.add_render_pass( "depth_pre_pass", new ( allocator->allocate( sizeof( DepthPrePass ), 64 ) )DepthPrePass );
        frame_renderer.add_render_pass( "gbuffer_pass_early", new ( allocator->allocate( sizeof( GBufferPass ), 64 ) )GBufferPass );
        frame_renderer.add_render_pass( "gbuffer_pass_late", new ( allocator->allocate( sizeof( LateGBufferPass ), 64 ) )LateGBufferPass );
        frame_renderer.add_render_pass( "lighting_pass", new ( allocator->allocate( sizeof( LightingPass ), 64 ) )LightingPass );
        frame_renderer.add_render_pass( "transparent_pass", new ( allocator->allocate( sizeof( TransparentPass ), 64 ) )TransparentPass );
        frame_renderer.add_render_pass( "depth_of_field_pass", new ( allocator->allocate( sizeof( DoFPass ), 64 ) )DoFPass );
        frame_renderer.add_render_pass( "debug_pass", new ( allocator->allocate( sizeof( DebugPass ), 64 ) )DebugPass );
        frame_renderer.add_render_pass( "mesh_occlusion_early_pass", new ( allocator->allocate( sizeof( CullingEarlyPass ), 64 ) )CullingEarlyPass );
        frame_renderer.add_render_pass( "mesh_occlusion_late_pass", new ( allocator->allocate( sizeof( CullingLatePass ), 64 ) )CullingLatePass );
        frame_renderer.add_render_pass( "depth_pyramid_pass", new ( allocator->allocate( sizeof( DepthPyramidPass ), 64 ) )DepthPyramidPass );
        //frame_renderer.add_render_pass( "point_shadows_pass", new ( allocator->allocate( sizeof( PointlightShadowPass ), 64 ) )PointlightShadowPass );
        frame_renderer.add_render_pass( "volumetric_fog_pass", new ( allocator->allocate( sizeof( VolumetricFogPass ), 64 ) )VolumetricFogPass );
        frame_renderer.add_render_pass( "temporal_anti_aliasing_pass", new ( allocator->allocate( sizeof( TemporalAntiAliasingPass ), 64 ) )TemporalAntiAliasingPass );
        frame_renderer.add_render_pass( "motion_vector_pass", new ( allocator->allocate( sizeof( MotionVectorPass ), 64 ) )MotionVectorPass );
        frame_renderer.add_render_pass( "ray_tracing_test", new ( allocator->allocate( sizeof( RayTracingTestPass ), 64 ) )RayTracingTestPass );
        frame_renderer.add_render_pass( "shadow_visibility_pass", new ( allocator->allocate( sizeof( ShadowVisibilityPass ), 64 ) )ShadowVisibilityPass );
       // frame_renderer.add_render_pass( "indirect_lighting_pass", new ( allocator->allocate( sizeof( DDGIPass ), 64 ) )DDGIPass );
        frame_renderer.add_render_pass( "reflections_pass", new ( allocator->allocate( sizeof( RaytracedReflectionsPass ), 64 ) )RaytracedReflectionsPass );
        frame_renderer.add_render_pass( "svgf_accumulation_pass", new ( allocator->allocate( sizeof( SVGFAccumulationPass ), 64 ) )SVGFAccumulationPass );
        frame_renderer.add_render_pass( "svgf_variance_pass", new ( allocator->allocate( sizeof( SVGFVariancePass ), 64 ) )SVGFVariancePass );
        frame_renderer.add_render_pass( "svgf_wavelet_pass", new ( allocator->allocate( sizeof( SVGFWaveletPass ), 64 ) )SVGFWaveletPass );

        // Finally parse all techniques
       // load_all_techniques();
    }

    // NOTE(marco): build AS before preparing draws
    //{
    //    CommandBuffer* gfx_cb = gpu.allocate_command_buffer( 0, 0, CommandQueueType::Graphics );
    //    gfx_cb->begin();

    //    // NOTE(marco): build BLAS
    //    VkAccelerationStructureBuildGeometryInfoKHR as_info{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    //    as_info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    //    as_info.flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
    //    as_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    //    as_info.geometryCount = frame_renderer.render_blackboard.geometries.size;
    //    as_info.pGeometries = frame_renderer.render_blackboard.geometries.data;

    //    Array<u32> max_primitives_count;
    //    max_primitives_count.init( gpu.allocator, frame_renderer.render_blackboard.geometries.size, frame_renderer.render_blackboard.geometries.size );

    //    for ( u32 range_index = 0; range_index < frame_renderer.render_blackboard.geometries.size; range_index++ ) {
    //        max_primitives_count[ range_index ] = frame_renderer.render_blackboard.build_range_infos[ range_index ].primitiveCount;
    //    }

    //    VkAccelerationStructureBuildSizesInfoKHR as_size_info{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    //    vkGetAccelerationStructureBuildSizesKHR( gpu.vulkan_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &as_info, max_primitives_count.data, &as_size_info );

    //    //as_buffer_creation.set( VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR, ResourceUsageType::Immutable, as_size_info.accelerationStructureSize ).set_device_only( true ).set_name( "blas_buffer" );
    //    frame_renderer.render_blackboard.blas_buffer = gpu.create_buffer({
    //        .type_flags = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
    //        .usage = ResourceUsageType::Immutable, .size = (u32)as_size_info.accelerationStructureSize,
    //        .device_only = 1, .name = "blas_buffer" });

    //    Buffer* blas_buffer = gpu.get_buffer( frame_renderer.render_blackboard.blas_buffer );

    //    //as_buffer_creation.reset().set( VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR, ResourceUsageType::Immutable, as_size_info.buildScratchSize ).set_device_only( true ).set_name( "blas_scratch_buffer" );
    //    BufferHandle blas_scratch_buffer_handle = gpu.create_buffer( {
    //        .type_flags = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
    //        .usage = ResourceUsageType::Immutable, .size = ( u32 )as_size_info.buildScratchSize,
    //        .device_only = 1, .name = "blas_scratch_buffer" });

    //    VkAccelerationStructureCreateInfoKHR as_create_info{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
    //    as_create_info.buffer = blas_buffer->vk_buffer;
    //    as_create_info.offset = 0;
    //    as_create_info.size = as_size_info.accelerationStructureSize;
    //    as_create_info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;

    //    vkCreateAccelerationStructureKHR( gpu.vulkan_device, &as_create_info, gpu.vulkan_allocation_callbacks, &frame_renderer.render_blackboard.blas );

    //    as_info.dstAccelerationStructure = frame_renderer.render_blackboard.blas;

    //    as_info.scratchData.deviceAddress = gpu.get_buffer_device_address( blas_scratch_buffer_handle );

    //    VkAccelerationStructureBuildRangeInfoKHR* blas_ranges[] = {
    //        frame_renderer.render_blackboard.build_range_infos.data
    //    };

    //    vkCmdBuildAccelerationStructuresKHR( gfx_cb->vk_command_buffer, 1, &as_info, blas_ranges );
    //    gfx_cb->end();
    //    gpu.submit_immediate( gfx_cb );

    //    // NOTE(marco): build TLAS
    //    VkAccelerationStructureDeviceAddressInfoKHR blas_address_info{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
    //    blas_address_info.accelerationStructure = frame_renderer.render_blackboard.blas;

    //    VkDeviceAddress blas_address = vkGetAccelerationStructureDeviceAddressKHR( gpu.vulkan_device, &blas_address_info );

    //    VkAccelerationStructureInstanceKHR tlas_structure{ };
    //    // NOTE(marco): identity matrix
    //    tlas_structure.transform.matrix[ 0 ][ 0 ] =  1.0f;
    //    tlas_structure.transform.matrix[ 1 ][ 1 ] =  1.0f;
    //    tlas_structure.transform.matrix[ 2 ][ 2 ] = -1.0f;
    //    tlas_structure.mask = 0xff;
    //    tlas_structure.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    //    tlas_structure.accelerationStructureReference = blas_address;

    //    //as_buffer_creation.reset().set( VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, ResourceUsageType::Immutable, sizeof( VkAccelerationStructureInstanceKHR ) ).set_data( &tlas_structure ).set_name( "tlas_instance_buffer" );
    //    BufferHandle tlas_instance_buffer_handle = gpu.create_buffer( {
    //        .type_flags = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    //        .usage = ResourceUsageType::Immutable, .size = ( u32 )sizeof( VkAccelerationStructureInstanceKHR ),
    //        .initial_data = &tlas_structure, .name = "tlas_instance_buffer" });

    //    VkAccelerationStructureGeometryKHR tlas_geometry{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    //    tlas_geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    //    tlas_geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    //    tlas_geometry.geometry.instances.arrayOfPointers = false;
    //    tlas_geometry.geometry.instances.data.deviceAddress = gpu.get_buffer_device_address( tlas_instance_buffer_handle );

    //    as_info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    //    as_info.geometryCount = 1;
    //    as_info.pGeometries = &tlas_geometry;

    //    u32 max_instance_count = 1;

    //    vkGetAccelerationStructureBuildSizesKHR( gpu.vulkan_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &as_info, &max_instance_count, &as_size_info );

    //    //as_buffer_creation.reset().set( VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, ResourceUsageType::Immutable, as_size_info.accelerationStructureSize ).set_device_only( true ).set_name( "tlas_buffer" );
    //    frame_renderer.render_blackboard.tlas_buffer = gpu.create_buffer( {
    //        .type_flags = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, .usage = ResourceUsageType::Immutable,
    //        .size = ( u32 )as_size_info.accelerationStructureSize, .device_only = 1,
    //        .name = "tlas_buffer" });

    //    Buffer* tlas_buffer = gpu.get_buffer( frame_renderer.render_blackboard.tlas_buffer );

    //    //as_buffer_creation.reset().set( VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR, ResourceUsageType::Immutable, as_size_info.buildScratchSize ).set_device_only( true ).set_name( "tlas_scratch_buffer" );
    //    BufferHandle tlas_scratch_buffer_handle = gpu.create_buffer( {
    //        .type_flags = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
    //        .usage = ResourceUsageType::Immutable, .size = ( u32 )as_size_info.buildScratchSize,
    //        .device_only = 1, .name = "tlas_scratch_buffer" });

    //    as_create_info.buffer = tlas_buffer->vk_buffer;
    //    as_create_info.offset = 0;
    //    as_create_info.size = as_size_info.accelerationStructureSize;
    //    as_create_info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;

    //    vkCreateAccelerationStructureKHR( gpu.vulkan_device, &as_create_info, gpu.vulkan_allocation_callbacks, &frame_renderer.render_blackboard.tlas );

    //    as_info.dstAccelerationStructure = frame_renderer.render_blackboard.tlas;

    //    as_info.scratchData.deviceAddress = gpu.get_buffer_device_address( tlas_scratch_buffer_handle );

    //    VkAccelerationStructureBuildRangeInfoKHR tlas_range_info{ };
    //    tlas_range_info.primitiveCount = 1;

    //    VkAccelerationStructureBuildRangeInfoKHR* tlas_ranges[] = {
    //        &tlas_range_info
    //    };

    //    gfx_cb->reset();
    //    gfx_cb->begin();

    //    // TODO(marco): we shouldn't be doing this manually
    //    RASSERTM( false, "Update code with new gpu profiler" );
    //    //GpuThreadFramePools* thread_pools = gfx_cb->thread_frame_pool;
    //    //thread_pools->time_queries->reset();
    //    //vkCmdResetQueryPool( gfx_cb->vk_command_buffer, thread_pools->vulkan_timestamp_query_pool, 0, thread_pools->time_queries->time_queries.size );

    //    //vkCmdResetQueryPool( gfx_cb->vk_command_buffer, thread_pools->vulkan_pipeline_stats_query_pool, 0, GpuPipelineStatistics::Count );

    //    //vkCmdBeginQuery( gfx_cb->vk_command_buffer, thread_pools->vulkan_pipeline_stats_query_pool, 0, 0 );

    //    vkCmdBuildAccelerationStructuresKHR( gfx_cb->vk_command_buffer, 1, &as_info, tlas_ranges );

    //    gpu.submit_immediate( gfx_cb );

    //    frame_renderer.render_blackboard.geometries.shutdown();
    //    frame_renderer.render_blackboard.build_range_infos.shutdown();

    //    gpu.destroy_buffer( blas_scratch_buffer_handle );
    //    gpu.destroy_buffer( tlas_scratch_buffer_handle );
    //    gpu.destroy_buffer( tlas_instance_buffer_handle );

    //    max_primitives_count.shutdown();
    //}

    frame_renderer.prepare_draws( &scratch_allocator );

    // Start multithreading IO
    // Create IO threads at the end
    RunPinnedTaskLoopTask run_pinned_task;
    run_pinned_task.threadNum = task_scheduler.GetNumTaskThreads() - 1;
    run_pinned_task.task_scheduler = &task_scheduler;
    task_scheduler.AddPinnedTask( &run_pinned_task );

    // Send async load task to external thread FILE_IO
    AsynchronousLoadTask async_load_task;
    async_load_task.threadNum = run_pinned_task.threadNum;
    async_load_task.task_scheduler = &task_scheduler;
    async_load_task.async_loader = &async_loader;
    task_scheduler.AddPinnedTask( &async_load_task );

    i64 begin_frame_tick = time_now();
    i64 absolute_begin_frame_tick = begin_frame_tick;

    f32 spring_stiffness = 10000.0f;
    f32 spring_damping = 5000.0f;
    f32 air_density = 2.0f;
    bool reset_simulation = false;
    glm::vec3 wind_direction{ -2.0f, 0.0f, 0.0f };

    bool light_placement = true;

    for ( u32 i = 0; i < 6; ++i ) {
        frame_renderer.render_config.cubeface_flip[ i ] = false;
    }

    u32 image_view_to_debug = 127;
    Array<u32> image_view_indices;
    image_view_indices.init( allocator, gpu.images.size, gpu.images.size );

    Array<cstring> image_names;
    image_names.init( allocator, gpu.images.size, gpu.images.size );

    StringBuffer texture_names_pool;
    texture_names_pool.init( rkilo( 32 ), allocator );

    while ( !window.requested_exit ) {
        ZoneScopedN("RenderLoop");

        // New frame
        if ( !window.minimized ) {
            gpu.wait_for_previous_frame();
            VkResult result = gpu.acquire_next_swapchain_image();
            if ( result == VK_ERROR_OUT_OF_DATE_KHR ) {
                gpu.resize_swapchain();
            }
            gpu.update_descriptors();
            gpu.reset_pools();

            static bool one_time_check = true;
            if ( async_loader.file_load_requests.size == 0 && one_time_check ) {
                one_time_check = false;
                rprint( "Finished uploading textures in %f seconds\n", time_from_seconds( absolute_begin_frame_tick ) );
            }
        }

        window.handle_os_messages();
        input.new_frame();

        if ( window.resized ) {
            gpu.resize( window.width, window.height );
            window.resized = false;
            frame_graph.on_resize( &renderer, &frame_renderer.render_blackboard,
                                   &frame_renderer.render_config, window.width, window.height );
            render_scene.on_resize( gpu, &frame_graph, window.width, window.height );
            frame_renderer.update_dependent_resources();

            game_camera.camera.set_aspect_ratio( window.width * 1.f / window.height );
        }
        // This MUST be AFTER os messages!
        imgui->new_frame();

        const i64 current_tick = time_now();
        f32 delta_time = ( f32 )time_delta_seconds( begin_frame_tick, current_tick );
        begin_frame_tick = current_tick;

        input.update( delta_time );
        game_camera.update( &input, window.width, window.height, delta_time );
        window.center_mouse( game_camera.mouse_dragging );

        static f32 animation_speed_multiplier = 0.05f;
        static bool enable_frustum_cull_meshes = true;
        static bool enable_frustum_cull_meshlets = true;
        static bool enable_occlusion_cull_meshes = true;
        static bool enable_occlusion_cull_meshlets = true;
        static bool freeze_occlusion_camera = false;
        static bool enable_camera_inside = true;
        static bool use_mcguire_method = false;
        static bool skip_invisible_lights = true;
        static bool force_fullscreen_light_aabb = false;
        static glm::mat4 projection_transpose{ };
        static glm::vec3 aabb_test_position{ 0,0,0 };
        static bool enable_aabb_cubemap_test = false;
        static bool enable_light_cluster_debug = false;
        static bool enable_light_tile_debug = false;
        static bool debug_show_light_tiles = false;
        static bool debug_show_tiles = false;
        static bool debug_show_bins = false;
        static bool disable_shadows = false;
        static bool shadow_meshlets_cone_cull = true;
        static bool shadow_meshlets_sphere_cull = true;
        static bool shadow_meshes_sphere_cull = true;
        static bool shadow_meshlets_cubemap_face_cull = true;
        static u32 lighting_debug_modes = 0;
        static u32 light_to_debug = 0;
        static glm::vec2 last_clicked_position = glm::vec2{ 1280 / 2.0f, 800 / 2.0f };
        static glm::vec3 raytraced_shadow_light_direction = glm::vec3{ 0, 1, -0.2f };
        static glm::vec3 raytraced_shadow_light_position = glm::vec3{ 0, 1, 0 };
        static float raytraced_shadow_light_intensity = 5.0f;
        static i32 raytraced_shadow_light_type = 0;
        static f32 raytraced_shadow_light_radius = 10.f;
        static glm::vec3 raytraced_shadow_light_color = glm::vec3{ 1, 1, 1 };

        // Jittering update
        static u32 jitter_index = 0;
        //static JitterType::Enum jitter_type = JitterType::Halton;
        static u32 jitter_period = 2;
        glm::vec2 jitter_values = glm::vec2{ 0.0f, 0.0f };

        //switch ( jitter_type ) {
        //    case JitterType::Halton:
        //        jitter_values = halton23_sequence( jitter_index );
        //        break;

        //    case JitterType::R2:
        //        jitter_values = m_robert_r2_sequence( jitter_index );
        //        break;

        //    case JitterType::InterleavedGradients:
        //        jitter_values = interleaved_gradient_sequence( jitter_index );
        //        break;

        //    case JitterType::Hammersley:
        //        jitter_values = hammersley_sequence( jitter_index, jitter_period );
        //        break;
        //}
        jitter_index = ( jitter_index + 1 ) % jitter_period;

        glm::vec2 jitter_offsets = glm::vec2{ jitter_values.x * 2 - 1.0f, jitter_values.y * 2 - 1.0f };
        static f32 jitter_scale = 1.f;

        jitter_offsets.x *= jitter_scale;
        jitter_offsets.y *= jitter_scale;

        // Update also projection matrix of the camera.
        /*if ( frame_renderer.render_config.taa_enabled && frame_renderer.render_config.taa_jittering_enabled ) {
            game_camera.apply_jittering( jitter_offsets.x / gpu.swapchain_width, jitter_offsets.y / gpu.swapchain_height );
        }
        else*/ 
        {
            game_camera.camera.set_zoom( 1.0f );
            game_camera.camera.update();
        }

        {
            ZoneScopedN( "ImGui Recording" );

            if ( ImGui::Begin( "Raptor ImGui" ) ) {
                ImGui::Checkbox( "Use Slang Shaders", &frame_renderer.render_config.use_slang_shaders );
                ImGui::InputFloat( "Scene global scale", &frame_renderer.render_config.global_scale, 0.001f );
                ImGui::InputFloat3( "Camera position", &game_camera.camera.position[0] );
                ImGui::InputFloat3( "Camera target movement", &game_camera.target_movement[0] );
                ImGui::Separator();
                if ( ImGui::Button( "Reload Shaders" ) ) {
                   // reload_all_techniques();

                    frame_graph.reload_shaders( render_scene, &frame_renderer.render_blackboard, &frame_renderer.render_config );
                }
                ImGui::SliderFloat( "Force Roughness", &frame_renderer.render_config.forced_roughness, -1, 1 );
                ImGui::SliderFloat( "Force Metalness", &frame_renderer.render_config.forced_metalness, -1, 1 );
                if ( ImGui::CollapsingHeader( "Physics" ) ) {
                    ImGui::InputFloat3( "Wind direction", &wind_direction[0] );
                    ImGui::InputFloat( "Air density", &air_density );
                    ImGui::InputFloat( "Spring stiffness", &spring_stiffness );
                    ImGui::InputFloat( "Spring damping", &spring_damping );
                    ImGui::Checkbox( "Reset simulation", &reset_simulation );
                }

                if ( ImGui::CollapsingHeader( "Math tests" ) ) {
                    ImGui::Checkbox( "Enable AABB cubemap test", &enable_aabb_cubemap_test );
                    ImGui::Checkbox( "Enable light cluster debug", &enable_light_cluster_debug );
                    ImGui::Checkbox( "Enable light tile debug", &enable_light_tile_debug );
                    ImGui::SliderFloat3( "AABB test position", &aabb_test_position[0], -1.5f, 1.5f, "%1.2f" );
                }

                // Light editing
                if ( ImGui::CollapsingHeader( "Lights" ) ) {
                    ImGui::SliderUint( "Active Lights", &render_scene.active_lights, 1, k_num_lights - 1 );
                    ImGui::SliderUint( "Light Index", &light_to_debug, 0, render_scene.active_lights - 1 );

                    Light& selected_light = render_scene.lights[ light_to_debug ];
                    ImGui::SliderFloat3( "Light position", &selected_light.world_position[0], -10.f, 10.f, "%2.3f" );
                    ImGui::SliderFloat( "Light radius", &selected_light.radius, 0.01f, 10.f, "%2.3f" );
                    ImGui::SliderFloat( "Light intensity", &selected_light.intensity, 0.01f, 10.f, "%2.3f" );

                    f32 light_color[ 3 ] = { selected_light.color.x, selected_light.color.y, selected_light.color.z };
                    ImGui::ColorEdit3( "Light color", light_color );
                    selected_light.color = { light_color[ 0 ], light_color[ 1 ], light_color[ 2 ] };

                    ImGui::Checkbox( "Light Edit Debug Draws", &frame_renderer.render_config.show_light_edit_debug_draws );
                }

                if ( ImGui::CollapsingHeader( "Meshlets" ) ) {
                    static bool enable_meshlets = false;
                    enable_meshlets = frame_renderer.render_config.meshlets.use_meshlets && gpu.mesh_shaders_extension_present;
                    ImGui::Checkbox( "Use meshlets", &enable_meshlets );
                    frame_renderer.render_config.meshlets.use_meshlets = enable_meshlets;
                    ImGui::Checkbox( "Use meshlets emulation", &frame_renderer.render_config.meshlets.use_meshlets_emulation );
                    ImGui::Checkbox( "Use frustum cull for meshes", &enable_frustum_cull_meshes );
                    ImGui::Checkbox( "Use frustum cull for meshlets", &enable_frustum_cull_meshlets );
                    ImGui::Checkbox( "Use occlusion cull for meshes", &enable_occlusion_cull_meshes );
                    ImGui::Checkbox( "Use occlusion cull for meshlets", &enable_occlusion_cull_meshlets );
                    ImGui::Checkbox( "Use meshes sphere cull for shadows", &shadow_meshes_sphere_cull );
                    ImGui::Checkbox( "Use meshlets cone cull for shadows", &shadow_meshlets_cone_cull );
                    ImGui::Checkbox( "Use meshlets sphere cull for shadows", &shadow_meshlets_sphere_cull );
                    ImGui::Checkbox( "Use meshlets cubemap face cull for shadows", &shadow_meshlets_cubemap_face_cull );
                    ImGui::Checkbox( "Freeze occlusion camera", &freeze_occlusion_camera );
                }
                if ( ImGui::CollapsingHeader( "Clustered Lighting" ) ) {

                    ImGui::Checkbox( "Enable Camera Inside approximation", &enable_camera_inside );
                    ImGui::Checkbox( "Use McGuire method for AABB sphere", &use_mcguire_method );
                    ImGui::Checkbox( "Skip invisible lights", &skip_invisible_lights );
                    ImGui::Checkbox( "force fullscreen light aabb", &force_fullscreen_light_aabb );
                    ImGui::Checkbox( "debug show light tiles", &debug_show_light_tiles );
                    ImGui::Checkbox( "debug show tiles", &debug_show_tiles );
                    ImGui::Checkbox( "debug show bins", &debug_show_bins );
                    ImGui::SliderUint( "Lighting debug modes", &lighting_debug_modes, 0, 10 );
                }
                if ( ImGui::CollapsingHeader( "PointLight Shadows" ) ) {
                    ImGui::Checkbox( "Pointlight rendering", &frame_renderer.render_config.pointlight_rendering );
                    ImGui::Checkbox( "Pointlight rendering use meshlets", &frame_renderer.render_config.pointlight_use_meshlets );
                    ImGui::Checkbox( "Disable shadows", &disable_shadows );
                    ImGui::Checkbox( "Use tetrahedron shadows", &frame_renderer.render_config.use_tetrahedron_shadows );
                    ImGui::Checkbox( "Cubeface switch Pos X", &frame_renderer.render_config.cubeface_flip[ 0 ] );
                    ImGui::Checkbox( "Cubeface switch Neg X", &frame_renderer.render_config.cubeface_flip[ 1 ] );
                    ImGui::Checkbox( "Cubeface switch Pos Y", &frame_renderer.render_config.cubeface_flip[ 2 ] );
                    ImGui::Checkbox( "Cubeface switch Neg Y", &frame_renderer.render_config.cubeface_flip[ 3 ] );
                    ImGui::Checkbox( "Cubeface switch Pos Z", &frame_renderer.render_config.cubeface_flip[ 4 ] );
                    ImGui::Checkbox( "Cubeface switch Neg Z", &frame_renderer.render_config.cubeface_flip[ 5 ] );
                }
                
                frame_renderer.render_config.post.draw_imgui();

                if ( ImGui::CollapsingHeader( "Raytraced Shadows" ) ) {
                    static cstring light_type[] = { "Point", "Directional" };
                    ImGui::Combo( "RT Light Type", &raytraced_shadow_light_type, light_type, ArraySize( light_type ) );

                    ImGui::SliderFloat( "RT Light intensity", &raytraced_shadow_light_intensity, 0.01f, 10.f, "%2.2f" );
                    ImGui::ColorEdit3( "RT Light Color", &raytraced_shadow_light_color[0] );

                    // If directional light, disable light position and light radius controls
                    if ( raytraced_shadow_light_type == 1 ) {
                        ImGui::BeginDisabled();
                    }
                    ImGui::SliderFloat( "RT Light Radius", &raytraced_shadow_light_radius, 0.01f, 10.f );
                    ImGui::SliderFloat3( "RT Light Position", &raytraced_shadow_light_position[0], -10.f, 10.f, "%2.2f" );
                    if ( raytraced_shadow_light_type == 1 ) {
                        ImGui::EndDisabled();
                    }

                    // If type is a pointlight, disable the light direction
                    if ( raytraced_shadow_light_type == 0 ) {
                        ImGui::BeginDisabled();
                    }
                    ImGui::SliderFloat3( "RT Directional Direction", &raytraced_shadow_light_direction[0], -1.f, 1.f, "%2.2f" );
                    if ( raytraced_shadow_light_type == 0 ) {
                        ImGui::EndDisabled();
                    }
                }
                if ( ImGui::CollapsingHeader( "Global Illumination" ) ) {

                    //DDGIPass* ddgi_pass = frame_graph_builder.get_render_pass<DDGIPass>( "indirect_lighting_pass" );

                    //ImGui::Text( "Total Rays: %u, Rays per probe %u, Total Probes %u", ddgi_pass->get_total_rays(), ddgi_pass->probe_rays, ddgi_pass->get_total_probes() );
                    //ImGui::SliderInt( "Per frame probe updates", &frame_renderer.render_config.gi_per_frame_probes_update, 0, ddgi_pass->get_total_probes() );
                    //// Check if probe offsets needs to be recalculated.
                    //frame_renderer.render_config.gi_recalculate_offsets = false;

                    //ImGui::SliderFloat( "Indirect Intensity", &frame_renderer.render_config.gi_intensity, 0.0f, 1.0f );
                    //if ( ImGui::SliderFloat3( "Probe Grid Position", &frame_renderer.render_config.gi_probe_grid_position[0], -5.f, 5.f, "%2.3f" ) ) {
                    //    frame_renderer.render_config.gi_recalculate_offsets = true;
                    //}

                    //ImGui::Checkbox( "Use Infinite Bounces", &frame_renderer.render_config.gi_use_infinite_bounces );
                    //ImGui::SliderFloat( "Infinite bounces multiplier", &frame_renderer.render_config.gi_infinite_bounces_multiplier, 0.0f, 1.0f );

                    //if ( ImGui::SliderFloat3( "Probe Spacing", &frame_renderer.render_config.gi_probe_spacing[0], -2.f, 2.f, "%2.3f" ) ) {
                    //    frame_renderer.render_config.gi_recalculate_offsets = true;
                    //}

                    //ImGui::SliderFloat( "Hysteresis", &frame_renderer.render_config.gi_hysteresis, 0.0f, 1.0f );
                    //ImGui::SliderFloat( "Max Probe Offset", &frame_renderer.render_config.gi_max_probe_offset, 0.0f, 0.5f );
                    //ImGui::SliderFloat( "Sampling self shadow bias", &frame_renderer.render_config.gi_self_shadow_bias, 0.0f, 1.0f );
                    //ImGui::SliderFloat( "Probe Sphere Scale", &frame_renderer.render_config.gi_probe_sphere_scale, 0.0f, 1.0f );
                    //ImGui::Checkbox( "Show debug probes", &frame_renderer.render_config.gi_show_probes );
                    //ImGui::Checkbox( "Use Visibility", &frame_renderer.render_config.gi_use_visibility );
                    //ImGui::Checkbox( "Use Smooth Backface", &frame_renderer.render_config.gi_use_backface_smoothing );
                    //ImGui::Checkbox( "Use Perceptual Encoding", &frame_renderer.render_config.gi_use_perceptual_encoding );
                    //ImGui::Checkbox( "Use Backface Blending", &frame_renderer.render_config.gi_use_backface_blending );
                    //ImGui::Checkbox( "Use Probe Offsetting", &frame_renderer.render_config.gi_use_probe_offsetting );
                    //ImGui::Checkbox( "Use Probe Status", &frame_renderer.render_config.gi_use_probe_status );
                    //if ( ImGui::Checkbox( "Use Half Resolution Output", &frame_renderer.render_config.gi_use_half_resolution ) ) {
                    //    ddgi_pass->half_resolution_output = frame_renderer.render_config.gi_use_half_resolution;

                    //    FrameGraphResourceContext context{ &renderer, &frame_graph, &frame_renderer.render_blackboard, &frame_renderer.render_config, &render_scene };
                    //    ddgi_pass->on_resize( context, gpu.swapchain_width, gpu.swapchain_height );
                    //}
                    //ImGui::Checkbox( "Debug border vs inside", &frame_renderer.render_config.gi_debug_border );
                    //ImGui::Checkbox( "Debug border type (corner, row, column)", &frame_renderer.render_config.gi_debug_border_type );
                    //ImGui::Checkbox( "Debug border source pixels", &frame_renderer.render_config.gi_debug_border_source );
                }
                /*if ( ImGui::CollapsingHeader( "Raytraced Reflections" ) ) {
                    ImGui::SliderFloat( "Temporal Depth Difference", &frame_renderer.render_config.rt_temporal_depth_difference, 0.0f, 20.0f );
                    ImGui::SliderFloat( "Temporal Normal Difference", &frame_renderer.render_config.rt_temporal_normal_difference, 0.0f, 20.0f );
                    ImGui::SliderFloat( "Wavelet Sigma L", &frame_renderer.render_config.rt_wavelet_sigma_l, 0.0f, 20.0f );
                    ImGui::SliderFloat( "Wavelet Sigma N", &frame_renderer.render_config.rt_wavelet_sigma_n, 0.001f, 200.0f );
                    ImGui::SliderFloat( "Wavelet Sigma Z", &frame_renderer.render_config.rt_wavelet_sigma_z, 0.0f, 1.0f );
                }*/
                ImGui::Separator();

                //ImGui::Checkbox( "Show Debug GPU Draws", &frame_renderer.render_config.show_debug_gpu_draws );
                ImGui::Checkbox( "Dynamically recreate descriptor sets", &recreate_per_thread_descriptors );
                ImGui::Checkbox( "Use secondary command buffers", &use_secondary_command_buffers );
                ImGui::Separator();
                ImGui::SliderFloat( "Animation Speed Multiplier", &animation_speed_multiplier, 0.0f, 10.0f );
                ImGui::Separator();

                static bool fullscreen = false;
                if ( ImGui::Checkbox( "Fullscreen", &fullscreen ) ) {
                    window.set_fullscreen( fullscreen );
                }

                static i32 present_mode = renderer.gpu->present_mode;
                if ( ImGui::Combo( "Present Mode", &present_mode, raptor::PresentMode::s_value_names, raptor::PresentMode::Count ) ) {
                    renderer.set_presentation_mode( ( raptor::PresentMode::Enum )present_mode );
                }

                frame_graph.add_ui();
            }
            ImGui::End();

            if ( ImGui::Begin( "Scene" ) ) {

                static u32 selected_node = u32_max;

                ImGui::Text( "Selected node %u", selected_node );
                if ( selected_node < scene_graph.nodes_hierarchy.size ) {

                    glm::mat4& local_transform = scene_graph.local_matrices[ selected_node ];
                    f32 position[ 3 ]{ local_transform[3][0], local_transform[3][1], local_transform[3][2] };

                    if ( ImGui::SliderFloat3( "Node Position", position, -100.0f, 100.0f ) ) {
                        local_transform[3][0] = position[ 0 ];
                        local_transform[3][1] = position[ 1 ];
                        local_transform[3][2] = position[ 2 ];

                        scene_graph.set_local_matrix( selected_node, local_transform );
                    }
                    ImGui::Separator();
                }

                for ( u32 n = 0; n < scene_graph.nodes_hierarchy.size; ++n ) {
                    const SceneGraphNodeDebugData& node_debug_data = scene_graph.nodes_debug_data[ n ];
                    if ( ImGui::Selectable( node_debug_data.name ? node_debug_data.name : "-", n == selected_node) ) {
                        selected_node = n;
                    }
                }
            }
            ImGui::End();

            if ( ImGui::Begin( "GPU" ) ) {
                renderer.imgui_draw();
            }
            ImGui::End();

            if ( ImGui::Begin( "GPU Profiler" ) ) {
                ImGui::Text( "Cpu Time %0.2f ms", delta_time * 1000.f );
                gpu_profiler.imgui_draw();

            }
            ImGui::End();

            if ( ImGui::Begin( "Frame Graph Debug" ) ) {

                frame_graph.debug_ui();

                u32 max_image_views = gpu.image_views.size;
                u32 active_texture_count = 0;
                u32 active_texture_index = 0;
                texture_names_pool.clear();
                for ( u32 t = 0; t < max_image_views; ++t ) {

                    if ( gpu.image_views.active_elements.get_bit( t ) == 0 ) {
                        continue;
                    }

                    ImageView* image_view = &gpu.image_views.elements[ t ];

                    Image* image = gpu.get_image( image_view->parent_image );
                    if ( image != nullptr && image->name != nullptr ) {
                        image_names[ active_texture_count ] = texture_names_pool.append_use_f( "%s (%d)", image->name, t );
                        image_view_indices[ active_texture_count ] = t;

                        if ( t == image_view_to_debug ) {
                            active_texture_index = active_texture_count;
                        }

                        ++active_texture_count;
                    }
                }

                ImVec2 window_size = ImGui::GetWindowSize();
                window_size.y += 50;

                cstring combo_preview_value = image_names[ active_texture_index ];
                if (ImGui::BeginCombo("Image ID", combo_preview_value))
                {
                    for ( u32 t = 0; t < active_texture_count; ++t ) {
                        cstring image_name = image_names[ t ];
                        if ( strlen( image_name ) == 0 ) {
                            continue;
                        }

                        // TODO(gabriel): fix image view
                        const bool is_selected = ( image_view_to_debug == image_view_indices[ t ] );
                        if ( ImGui::Selectable( image_name, is_selected ) ) {
                            image_view_to_debug = image_view_indices[ t ];
                        }

                        if ( is_selected )
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }

                static i32 face_to = 0;
                ImGui::SliderInt( "Face", &face_to, 0, 5 );
                frame_renderer.render_config.cubemap_debug_face_index = ( u32 )face_to;
                ImGui::Checkbox( "Cubemap face enabled", &frame_renderer.render_config.cubemap_face_debug_enabled );

                // TODO(gabriel): fix image view
                ImGui::Image( ( ImTextureID )&image_view_to_debug, window_size );
            }
            ImGui::End();

            if ( ImGui::Begin( "Lights Debug" ) ) {
                const u32 lights_count = render_scene.lights.size;

                for ( u32 l = 0; l < lights_count; ++l ) {
                    Light& light = render_scene.lights[ l ];

                    ImGui::Text( "%d: %d, %d R: %0.2f a: %0.6f", l, light.tile_x, light.tile_y, light.shadow_map_resolution, light.solid_angle );
                }
            }
            ImGui::End();
        }
        {
            ZoneScopedN( "AnimationsUpdate" );
            render_scene.update_animations( delta_time * animation_speed_multiplier );
        }
        {
            ZoneScopedN( "SceneGraphUpdate" );
            scene_graph.update_matrices();
        }
        {
            ZoneScopedN( "JointsUpdate" );
            render_scene.update_joints( &frame_renderer );
        }

        {
            ZoneScopedN( "Gpu Buffers Update" );

            // TODO: move light placement here.
            if ( light_placement ) {
                light_placement = false;

                //place_lights( render_scene.lights, true );
            }

            // Update mouse clicked position
            if ( ( input.is_mouse_clicked( MOUSE_BUTTONS_LEFT ) || input.is_mouse_dragging( MOUSE_BUTTONS_LEFT ) ) && !ImGui::IsAnyItemHovered() ) {
                last_clicked_position = glm::vec2{ input.mouse_position.x, input.mouse_position.y };
            }

            frame_renderer.upload_gpu_data( game_camera, last_clicked_position, MemoryService::instance()->get_thread_allocator() );

            // Place light AABB with a smaller aabb to indicate the center.
            if ( frame_renderer.render_config.show_light_edit_debug_draws ) {
                const Light& light = render_scene.lights[ light_to_debug ];
                f32 half_radius = light.radius;
                //render_scene.debug_renderer.aabb( light.world_position - glm::vec3{ half_radius, half_radius ,half_radius }, light.world_position + glm::vec3{ half_radius, half_radius , half_radius }, { Color::white } );
                //render_scene.debug_renderer.aabb( light.world_position - glm::vec3{ .1f, .1f, .1f }, light.world_position + glm::vec3{ .1f, .1f, .1f }, { Color::green } );
            }
        }

        if ( !window.minimized ) {
            DrawTask draw_task;
            draw_task.init( renderer.gpu, &frame_graph, &renderer, imgui, &gpu_profiler, &render_scene, &frame_renderer );
            task_scheduler.AddTaskSetToPipe( &draw_task );

            CommandBuffer* async_compute_command_buffer = nullptr;
            {
                ZoneScopedN( "PhysicsUpdate" );
                async_compute_command_buffer = render_scene.update_physics( delta_time, air_density, spring_stiffness, spring_damping, wind_direction, reset_simulation );
                reset_simulation = false;
            }

            task_scheduler.WaitforTask( &draw_task );

            // Avoid using the same command buffer
            renderer.add_image_finalize_commands( ( draw_task.thread_id + 1 ) % task_scheduler.GetNumTaskThreads() );

            gpu.update_bindless_resources();
            gpu.update_sparse_resources();
            gpu.submit_command_buffers( async_compute_command_buffer );
            gpu.present();
            gpu.resolve_timestamps();
            gpu.process_pending_resource_deletion();
        } else {
            ImGui::Render();
        }

        FrameMark;
    }

    image_view_indices.shutdown();
    image_names.shutdown();
    texture_names_pool.shutdown();

    run_pinned_task.execute = false;
    async_load_task.execute = false;

    task_scheduler.WaitforAllAndShutdown();

    vkDeviceWaitIdle( gpu.vulkan_device );

    async_loader.shutdown();

    // Destroy resources built here.
    /*gpu.destroy_buffer( frame_renderer.render_blackboard.blas_buffer );
    vkDestroyAccelerationStructureKHR( gpu.vulkan_device, frame_renderer.render_blackboard.blas, gpu.vulkan_allocation_callbacks );
    gpu.destroy_buffer( frame_renderer.render_blackboard.tlas_buffer );
    vkDestroyAccelerationStructureKHR( gpu.vulkan_device, frame_renderer.render_blackboard.tlas, gpu.vulkan_allocation_callbacks );
    gpu.destroy_sampler( repeat_nearest_sampler );
    gpu.destroy_sampler( repeat_sampler );*/

    imgui->shutdown();

    gpu_profiler.shutdown();

    scene_graph.shutdown();

    frame_graph.shutdown();
    frame_graph_builder.shutdown();

    render_scene.shutdown( &renderer );
    frame_renderer.shutdown();

    rm.shutdown();
    renderer.shutdown();

    input.shutdown();
    window.unregister_os_messages_callback( input_os_messages_callback );
    window.shutdown();

    scratch_allocator.shutdown();
    MemoryService::instance()->shutdown();

    return 0;
}
