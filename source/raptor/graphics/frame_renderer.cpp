#include "graphics/frame_renderer.hpp"
#include "application/game_camera.hpp"
#include "graphics/renderer.hpp"
#include "graphics/render_scene.hpp"
#include "graphics/scene_graph.hpp"
#include "graphics/asynchronous_loader.hpp"

#include "foundation/numerics.hpp"

#include "graphics/raptor_imgui.hpp"
#include "graphics/frame_graph.hpp"
#include "graphics/gpu_profiler.hpp"

#include "external/glm/mat2x2.hpp"
#include "external/glm/matrix.hpp"

#include "external/tracy/tracy/Tracy.hpp"

#include "../shaders/shared_structs.h"

namespace raptor {

// FrameRenderer //////////////////////////////////////////////////////////
void FrameRenderer::init( Allocator* resident_allocator_, Renderer* renderer_,
                          FrameGraph* frame_graph_, SceneGraph* scene_graph_,
                          RenderScene* scene_ ) {
    resident_allocator = resident_allocator_;
    renderer = renderer_;
    frame_graph = frame_graph_;
    scene_graph = scene_graph_;
    scene = scene_;
    render_passes.init( resident_allocator, 16 );

    render_config.meshlets.gpu_mesh_shaders_extension_present = renderer->gpu->mesh_shaders_extension_present;
    render_config.meshlets.use_meshlets = renderer->gpu->mesh_shaders_extension_present;
    render_config.meshlets.use_meshlets_emulation = !render_config.meshlets.use_meshlets;

    // Resolution defaults
    upscale_settings.enabled = false;
    calculate_resolution_info( renderer->gpu->swapchain_width, renderer->gpu->swapchain_height );
}

void FrameRenderer::add_render_pass( cstring name, FrameGraphRenderPass* render_pass ) {
    render_pass->name = name;

    render_passes.push( render_pass );
}

void FrameRenderer::declare_frame_graph_structure( FrameGraph& frame_graph ) {

    FrameGraphResourceContext resource_context{ renderer, &frame_graph, &render_blackboard, &render_config, scene };

    for ( FrameGraphRenderPass* pass : render_passes ) {
        if ( !pass->enabled ) {
            continue;
        }

        pass->declare_frame_graph_node( resource_context );
        frame_graph.builder->register_render_pass( pass->name, pass );
    }
}

void FrameRenderer::compile_passes_psos() {

    FrameGraphResourceContext resource_context{ renderer, frame_graph, &render_blackboard, &render_config, scene };

    for ( FrameGraphRenderPass* pass : render_passes ) {
        if ( !pass->enabled ) {
            continue;
        }
        pass->update_psos( resource_context, PipelineUpdatePhase::Create );
    }

    if ( post ) {
        post->update_psos( renderer, frame_graph, PipelineUpdatePhase::Create );
    }
}

void FrameRenderer::calculate_resolution_info( u32 swapchain_width, u32 swapchain_height ) {

    resolution_info.swapchain_width = swapchain_width;
    resolution_info.swapchain_height = swapchain_height;

    if ( upscale_settings.enabled ) {
        resolution_info.render_width = ( u32 )( swapchain_width * upscale_settings.render_scale );
        resolution_info.render_height = ( u32 )( swapchain_height * upscale_settings.render_scale );
    }
    else {
        resolution_info.render_width = swapchain_width;
        resolution_info.render_height = swapchain_height;
    }

    // Align to 8 for wave operations
   // resolution_info.render_width = ( resolution_info.render_width + 7 ) & ~7u;
   // resolution_info.render_height = ( resolution_info.render_height + 7 ) & ~7u;

    // Update resolution info in blackboard
    render_blackboard.render_width = resolution_info.render_width;
    render_blackboard.render_height = resolution_info.render_height;
    render_blackboard.swapchain_width = resolution_info.swapchain_width;
    render_blackboard.swapchain_height = resolution_info.swapchain_height;
}

void FrameRenderer::on_resize( GpuDevice& gpu, u32 new_width, u32 new_height ) {

    calculate_resolution_info( new_width, new_height );

    if ( gpu_culling ) {
        gpu_culling->on_resize( renderer, &render_blackboard, new_width, new_height );
    }

    if ( lighting ) {
        lighting->on_resize( renderer, &render_blackboard, new_width, new_height );
    }

    if ( meshlets ) {
        meshlets->on_resize( renderer, &render_blackboard, new_width, new_height );
    }

    if ( post ) {
        post->on_resize( renderer, &render_blackboard, new_width, new_height );
    }

    if ( debug_draw ) {
        debug_draw->on_resize( renderer, &render_blackboard, new_width, new_height );
    }

    if ( point_shadows ) {
        point_shadows->on_resize( renderer, &render_blackboard, new_width, new_height );
    }
}

void FrameRenderer::shutdown() {

    gpu_materials.shutdown();
    gpu_mesh_bounds.shutdown();
    gpu_mesh_aabbs.shutdown();
    gpu_mesh_instances.shutdown();

    FrameGraphResourceContext resource_context{ renderer, frame_graph, &render_blackboard, &render_config, scene };

    for ( u32 i = 0; i < render_passes.size; ++i ) {
        render_passes[ i ]->destroy_gpu_resources( resource_context );
        render_passes[ i ]->update_psos( resource_context, PipelineUpdatePhase::Destroy );
        resident_allocator->deallocate( render_passes[ i ] );
        render_passes[ i ] = nullptr;
    }

    if ( point_shadows ) {
        point_shadows->destroy_gpu_resources( renderer, &render_blackboard );
    }

    if ( lighting ) {
        lighting->destroy_gpu_resources( renderer, &render_blackboard );
    }

    if ( meshes ) {
        meshes->destroy_gpu_resources( renderer, &render_blackboard );
    }

    if ( meshlets ) {
        meshlets->destroy_gpu_resources( renderer, &render_blackboard );
    }

    if ( gpu_culling ) {
        gpu_culling->destroy_gpu_resources( renderer, &render_blackboard );
    }

    if ( post ) {
        post->update_psos( renderer, frame_graph, PipelineUpdatePhase::Destroy );
        post->destroy_gpu_resources( renderer, &render_blackboard );
    }

    if ( debug_draw ) {
        debug_draw->destroy_gpu_resources( renderer, &render_blackboard );
    }

    if ( ray_tracing ) {
        ray_tracing->destroy_gpu_resources( renderer, &render_blackboard );
    }

    GpuDevice* gpu = renderer->gpu;
    gpu->destroy_buffer( render_blackboard.meshes_sb );
    gpu->destroy_buffer( render_blackboard.mesh_bounds_sb );
    gpu->destroy_buffer( render_blackboard.mesh_aabbs_sb );
    gpu->destroy_buffer( render_blackboard.mesh_instances_sb );

    gpu->destroy_buffer( render_blackboard.physics_cb );

    for ( u32 i = 0; i < k_max_frames; ++i ) {
        gpu->destroy_buffer( render_blackboard.meshlet_emulation_instances_indirect_count_sb[ i ] );
    }

    if ( gpu->fragment_shading_rate_present ) {
        gpu->destroy_image( render_blackboard.fragment_shading_rate_image );
        gpu->destroy_image_view( render_blackboard.fragment_shading_rate_image_view );
    }

    render_passes.shutdown();
}

//
//
glm::vec4 normalize_plane( glm::vec4 plane ) {
    f32 len = glm::length( glm::vec3( plane.x, plane.y, plane.z ) );
    return  plane * (1.0f / len);
}

f32 linearize_depth( f32 depth, f32 z_far, f32 z_near ) {
    return z_near * z_far / ( z_far + depth * ( z_near - z_far ) );
}

//
//
static void copy_gpu_material_data( GpuDevice& gpu, GpuMaterialData& gpu_mesh_data, const Mesh& mesh ) {
    gpu_mesh_data = {};

    gpu_mesh_data.textures[ 0 ] = mesh.pbr_material.diffuse_texture_index;
    gpu_mesh_data.textures[ 1 ] = mesh.pbr_material.roughness_texture_index;
    gpu_mesh_data.textures[ 2 ] = mesh.pbr_material.normal_texture_index;
    gpu_mesh_data.textures[ 3 ] = mesh.pbr_material.occlusion_texture_index;
    gpu_mesh_data.alpha_texture_index = mesh.pbr_material.alpha_texture_index;

    gpu_mesh_data.emissive = { mesh.pbr_material.emissive_factor.x, mesh.pbr_material.emissive_factor.y, mesh.pbr_material.emissive_factor.z, (float)mesh.pbr_material.emissive_texture_index };

    gpu_mesh_data.base_color_factor = mesh.pbr_material.base_color_factor;
    gpu_mesh_data.metallic_roughness_occlusion_factor.x = mesh.pbr_material.metallic;
    gpu_mesh_data.metallic_roughness_occlusion_factor.y = mesh.pbr_material.roughness;
    gpu_mesh_data.metallic_roughness_occlusion_factor.z = mesh.pbr_material.occlusion;
    gpu_mesh_data.alpha_cutoff = mesh.pbr_material.alpha_cutoff;

    gpu_mesh_data.flags = mesh.pbr_material.flags;

    gpu_mesh_data.mesh_index = mesh.gpu_mesh_index;
    gpu_mesh_data.meshlet_offset = mesh.meshlet_offset;
    gpu_mesh_data.meshlet_count = mesh.meshlet_count;
    gpu_mesh_data.meshlet_index_count = mesh.meshlet_index_count;

    gpu_mesh_data.position_buffer_offset = mesh.position_offset;
    gpu_mesh_data.uv0_buffer_offset = mesh.texcoord_offset;
    gpu_mesh_data.index_buffer_offset = mesh.index_offset_bytes;
    gpu_mesh_data.normal_buffer_offset = mesh.normal_offset;

    gpu_mesh_data.position_buffer_index = mesh.position_buffer.index();
    gpu_mesh_data.uv0_buffer_index = mesh.texcoord_buffer.index();
    gpu_mesh_data.index_buffer_index = mesh.index_buffer.index();
    gpu_mesh_data.normal_buffer_index = mesh.normal_buffer.index();

    gpu_mesh_data.position_buffer = gpu.get_buffer_device_address( mesh.position_buffer ) + mesh.position_offset;
    gpu_mesh_data.uv_buffer = mesh.texcoord_buffer.is_valid() ? gpu.get_buffer_device_address( mesh.texcoord_buffer ) + mesh.texcoord_offset : u64_max;
    gpu_mesh_data.index_buffer = gpu.get_buffer_device_address( mesh.index_buffer ) + mesh.index_offset_bytes;
    gpu_mesh_data.normals_buffer = gpu.get_buffer_device_address( mesh.normal_buffer ) + mesh.normal_offset;

    gpu_mesh_data.vertex_count = mesh.position_count;
}

//
//
static void copy_gpu_mesh_transform( GpuMeshInstanceData& gpu_mesh_data, RenderScene& render_scene, const MeshInstance& mesh_instance, const f32 global_scale, const SceneGraph* scene_graph ) {
    gpu_mesh_data = {};

    if ( scene_graph ) {

        RASSERTM( scene_graph->updated_nodes.get_bit( mesh_instance.scene_graph_node_index ) == 0,
                  "Call SceneGraph::update_matrices() before calling this method!" );

        // Apply global scale matrix
        // NOTE: for left-handed systems (as defined in cglm) need to invert positive and negative Z.
        const glm::mat4 scale_matrix = glm::scale( glm::mat4( 1.0f ), glm::vec3( global_scale, global_scale, global_scale ) );
        gpu_mesh_data.world = scale_matrix * scene_graph->world_matrices[ mesh_instance.scene_graph_node_index ];

       // gpu_mesh_data.inverse_world = glm::inverse( glm::transpose( gpu_mesh_data.world ) );
    } else {
        gpu_mesh_data.world = glm::mat4(1.0f);
        //gpu_mesh_data.inverse_world = glm::mat4(1.0f);
    }

    gpu_mesh_data.mesh_index = render_scene.meshes[ mesh_instance.mesh_index ].gpu_mesh_index;
}

void FrameRenderer::upload_gpu_data( GameCamera& game_camera, glm::vec2 last_clicked_position_left_button, ArenaAllocator* frame_scratch ) {

    // Update scene constant buffer
    RenderScene& render_scene = *scene;
    GpuDevice& gpu = *renderer->gpu;

    // Jittering calculations
    glm::vec2 jitter_values = glm::vec2{ 0.0f, 0.0f };

    switch ( render_config.taa.jitter_type ) {
        case JitterType::Halton:
            jitter_values = halton23_sequence( render_blackboard.jitter_index );
            break;

        case JitterType::R2:
            jitter_values = m_robert_r2_sequence( render_blackboard.jitter_index );
            break;

        case JitterType::InterleavedGradients:
            jitter_values = interleaved_gradient_sequence( render_blackboard.jitter_index );
            break;

        case JitterType::Hammersley:
            jitter_values = hammersley_sequence( render_blackboard.jitter_index, render_config.taa.jitter_period );
            break;
    }
    render_blackboard.jitter_index = ( render_blackboard.jitter_index + 1 ) % render_config.taa.jitter_period;

    glm::vec2 jitter_offsets = glm::vec2{ jitter_values.x * 2 - 1.0f, jitter_values.y * 2 - 1.0f };
    render_blackboard.jitter_offsets = jitter_offsets;

    const f32 jitter_scale = upscale_settings.enabled ? 0.5f : render_config.taa.jitter_scale;
    jitter_offsets.x *= jitter_scale;
    jitter_offsets.y *= jitter_scale;

    frame_data.halton_x = jitter_offsets.x;
    frame_data.halton_y = jitter_offsets.y;

    // Cache previous view projection
    frame_data.previous_view_projection = frame_data.view_projection;
    // Frame 0 jittering or disable jittering as option.
    if ( gpu.absolute_frame == 0 || ( render_config.taa.jittering_enabled == false ) ) {
        frame_data.jitter_xy = glm::vec2{ 0.0f, 0.0f };
    }
    // Cache previous jitter and calculate new one
    frame_data.previous_jitter_xy = frame_data.jitter_xy;

    if ( render_config.taa.jittering_enabled ) {

        const f32 render_width = render_blackboard.render_width * 1.f;
        const f32 render_height = render_blackboard.render_height * 1.f;

        frame_data.jitter_xy = glm::vec2{ frame_data.halton_x / render_width, frame_data.halton_y / render_height };

        game_camera.apply_jittering( jitter_offsets.x / render_width, jitter_offsets.y / render_height );
    }

    frame_data.view_projection = game_camera.camera.view_projection;

    frame_data.inverse_view_projection = glm::inverse( game_camera.camera.view_projection );
    frame_data.inverse_projection = glm::inverse( game_camera.camera.projection );
    frame_data.inverse_view = glm::inverse( game_camera.camera.view );
    frame_data.world_to_camera = game_camera.camera.view;
    frame_data.camera_position = glm::vec4{ game_camera.camera.position.x, game_camera.camera.position.y, game_camera.camera.position.z, 1.0f };
    frame_data.camera_direction = game_camera.camera.direction;
    frame_data.dither_image_view_index = dither_4x4_texture ? dither_4x4_texture->image_view.index() : 0;
    frame_data.current_frame = (u32)gpu.absolute_frame;
    frame_data.forced_metalness = render_config.forced_metalness;
    frame_data.forced_roughness = render_config.forced_roughness;

    FrameGraphResource* depth_resource = (FrameGraphResource*)frame_graph->get_resource( "depth" );
    if ( depth_resource ) {
        frame_data.depth_texture_index = depth_resource->resource_info.texture.image_view.index();
    }

    frame_data.blue_noise_128_rg_image_view_index = blue_noise_texture->image_view.index();
    frame_data.use_tetrahedron_shadows = render_config.use_tetrahedron_shadows;
    frame_data.active_lights = render_scene.active_lights;
    frame_data.z_near = game_camera.camera.near_plane;
    frame_data.z_far = game_camera.camera.far_plane;
    frame_data.projection_00 = game_camera.camera.projection[0][0];
    frame_data.projection_11 = game_camera.camera.projection[1][1];

    GpuCullingRenderConfig& gpu_culling_config = render_config.gpu_culling;
    frame_data.culling_options = 0;
    frame_data.set_frustum_cull_meshes( gpu_culling_config.enable_frustum_cull_meshes );
    frame_data.set_frustum_cull_meshlets( gpu_culling_config.enable_frustum_cull_meshlets );
    frame_data.set_occlusion_cull_meshes( gpu_culling_config.enable_occlusion_cull_meshes );
    frame_data.set_occlusion_cull_meshlets( gpu_culling_config.enable_occlusion_cull_meshlets );
    frame_data.set_freeze_occlusion_camera( gpu_culling_config.freeze_occlusion_camera );
    frame_data.set_shadow_meshlets_cone_cull( gpu_culling_config.shadow_meshlets_cone_cull );
    frame_data.set_shadow_meshlets_sphere_cull( gpu_culling_config.shadow_meshlets_sphere_cull );
    frame_data.set_shadow_meshlets_cubemap_face_cull( gpu_culling_config.shadow_meshlets_cubemap_face_cull );

    frame_data.resolution_x = render_blackboard.render_width * 1.f;
    frame_data.resolution_y = render_blackboard.render_height * 1.f;
    frame_data.aspect_ratio = render_blackboard.render_width * 1.f / render_blackboard.render_height;
    frame_data.num_mesh_instances = render_scene.mesh_instances.size;
    frame_data.volumetric_fog_application_dithering_scale = render_config.volumetric_fog.application_dithering_scale;
    frame_data.volumetric_fog_application_options =
        ( render_config.volumetric_fog.application_apply_opacity_anti_aliasing ? 1 : 0 )
        | ( render_config.volumetric_fog.application_apply_tricubic_filtering ? 2 : 0 );

    // Frustum computations
    GpuCullingRuntimeData& gpu_culling = render_blackboard.gpu_culling;
    if ( !gpu_culling_config.freeze_occlusion_camera ) {
        frame_data.camera_position_debug = frame_data.camera_position;
        frame_data.world_to_camera_debug = frame_data.world_to_camera;
        frame_data.view_projection_debug = frame_data.view_projection;
        gpu_culling.projection_transpose = glm::transpose( game_camera.camera.projection );
    }

    frame_data.frustum_planes[ 0 ] = normalize_plane( gpu_culling.projection_transpose[ 3 ] + gpu_culling.projection_transpose[ 0 ] ); // x + w  < 0;
    frame_data.frustum_planes[ 1 ] = normalize_plane( gpu_culling.projection_transpose[ 3 ] - gpu_culling.projection_transpose[ 0 ] ); // x - w  < 0;
    frame_data.frustum_planes[ 2 ] = normalize_plane( gpu_culling.projection_transpose[ 3 ] + gpu_culling.projection_transpose[ 1 ] ); // y + w  < 0;
    frame_data.frustum_planes[ 3 ] = normalize_plane( gpu_culling.projection_transpose[ 3 ] - gpu_culling.projection_transpose[ 1 ] ); // y - w  < 0;
    frame_data.frustum_planes[ 4 ] = normalize_plane( gpu_culling.projection_transpose[ 3 ] + gpu_culling.projection_transpose[ 2 ] ); // z + w  < 0;
    frame_data.frustum_planes[ 5 ] = normalize_plane( gpu_culling.projection_transpose[ 3 ] - gpu_culling.projection_transpose[ 2 ] ); // z - w  < 0;

    GpuFrameData* uniform_data = gpu.dynamic_buffer_allocate<GpuFrameData>( &render_blackboard.scene_cb_offset );
    if ( uniform_data ) {
        memcpy( uniform_data, &frame_data, sizeof( GpuFrameData ) );
    }

    // Features //////////////////////////////////////////////////////////
    UploadGpuDataContext upload_context{ game_camera, last_clicked_position_left_button, frame_scratch,
                                        renderer, render_blackboard, render_config, frame_graph, &render_scene, frame_data};

    if ( lighting ) {
        lighting->upload_gpu_data( upload_context );
    }

    if ( post ) {
        post->upload_gpu_data( upload_context );
    }

    if ( debug_draw ) {
        debug_draw->upload_gpu_data( upload_context );
    }

    if ( point_shadows ) {
        point_shadows->upload_gpu_data( upload_context );
    }

    if ( ray_tracing ) {
        ray_tracing->upload_gpu_data( upload_context );
    }

    // Cache view data informations
    ViewRuntimeData& main_view = render_blackboard.main_view;
    {
        main_view.view_projection = frame_data.view_projection;
        main_view.inverse_view_projection = frame_data.inverse_view_projection;
        main_view.near = frame_data.z_near;
        main_view.far = frame_data.z_far;
    }

    FrameGraphResourceContext resource_context{ renderer, frame_graph, &render_blackboard, &render_config, scene };

    for ( u32 i = 0; i < render_passes.size; ++i ) {
        render_passes[ i ]->upload_gpu_data( resource_context );
    }

    // UPLOAD DATA TO GPU ////////////////////////////////////////////////
    // Update per mesh material buffer
    // TODO: update only changed stuff, this is now dynamic so it can't be done.
    /*MapBufferParameters cb_map = { render_blackboard.meshes_sb, 0, 0 };
    GpuMaterialData* gpu_mesh_data = ( GpuMaterialData* )gpu.map_buffer( cb_map );
    if ( gpu_mesh_data ) {
        for ( u32 mesh_index = 0; mesh_index < scene->meshes.size; ++mesh_index ) {
            copy_gpu_material_data( gpu, gpu_mesh_data[ mesh_index ], scene->meshes[ mesh_index ] );
        }
        gpu.unmap_buffer( cb_map );
    }

    // Copy mesh bounding spheres
    cb_map.buffer = render_blackboard.mesh_bounds_sb;
    glm::vec4* gpu_bounds_data = ( glm::vec4* )gpu.map_buffer( cb_map );
    if ( gpu_bounds_data ) {
        for ( u32 mesh_index = 0; mesh_index < scene->meshes.size; ++mesh_index ) {
            gpu_bounds_data[ mesh_index ] = scene->meshes[ mesh_index ].bounding_sphere;
        }
        gpu.unmap_buffer( cb_map );
    }*/

    //// Copy mesh aabbs
    //cb_map.buffer = render_blackboard.mesh_aabbs_sb;
    //glm::vec4* gpu_aabbs_data = ( glm::vec4* )gpu.map_buffer( cb_map );
    //if ( gpu_aabbs_data ) {
    //    for ( u32 mesh_index = 0; mesh_index < scene->meshes.size; ++mesh_index ) {
    //        const Mesh& mesh = scene->meshes[ mesh_index ];
    //        gpu_aabbs_data[ mesh_index * 2 ] = glm::vec4( mesh.aabb[ 0 ], 0.f );
    //        gpu_aabbs_data[ mesh_index * 2 + 1 ] = glm::vec4( mesh.aabb[ 1 ], 0.f );
    //    }
    //    gpu.unmap_buffer( cb_map );
    //}

    //// Copy mesh instances data
    //cb_map.buffer = render_blackboard.mesh_instances_sb;
    //GpuMeshInstanceData* gpu_mesh_instance_data = ( GpuMeshInstanceData* )gpu.map_buffer( cb_map );
    //if ( gpu_mesh_instance_data ) {
    //    for ( u32 mi = 0; mi < scene->mesh_instances.size; ++mi ) {
    //        copy_gpu_mesh_transform( gpu_mesh_instance_data[ mi ], *scene, scene->mesh_instances[ mi ], render_config.global_scale, scene_graph );
    //    }

    //    const VkDeviceSize buffer_size = sizeof( GpuMeshInstanceData ) * scene->mesh_instances.size;
    //    gpu.flush_buffer( render_blackboard.mesh_instances_sb, 0, buffer_size );
    //    gpu.unmap_buffer( cb_map );
    //}
}

void FrameRenderer::render( CommandBuffer* gpu_commands, RenderScene* render_scene ) {
}

void FrameRenderer::create_resources( ArenaAllocator* scratch_allocator ) {

    // Update resolution info
    render_blackboard.render_width = resolution_info.render_width;
    render_blackboard.render_height = resolution_info.render_height;
    render_blackboard.swapchain_width = resolution_info.swapchain_width;
    render_blackboard.swapchain_height = resolution_info.swapchain_height;
    render_blackboard.render_scale_factor = resolution_info.swapchain_width * 1.f / resolution_info.render_width;

    scene->prepare_draws( renderer, scratch_allocator, scene_graph );

    // Load common textures
    {
        // Use renderer names buffer to store path and texture names
        StringBuffer& path_buffer = renderer->names_buffer;

        dither_4x4_texture = renderer->create_texture_from_file( path_buffer.append_use_f( "%s/%s", RAPTOR_DATA_FOLDER, "BayerDither4x4.png" ), false, false );
        dither_8x8_texture = renderer->create_texture_from_file( path_buffer.append_use_f( "%s/%s", RAPTOR_DATA_FOLDER, "BayerDither8x8.png" ), false, false );
        blue_noise_texture = renderer->create_texture_from_file( path_buffer.append_use_f( "%s/%s", RAPTOR_DATA_FOLDER, "LDR_RG01_0.png" ), false, false );
    }

    // Create scene resources
    if ( meshes ) {
        meshes->create_geometry_resources( renderer, &render_blackboard, scene );
    }

    if ( meshlets ) {
        meshlets->create_geometry_resources( renderer, &render_blackboard, scene );
    }

    // TODO: these are temporary buffers until we fix the staging
    gpu_materials.init( resident_allocator, scene->meshes.size, scene->meshes.size );
    gpu_mesh_bounds.init( resident_allocator, scene->meshes.size, scene->meshes.size );
    gpu_mesh_aabbs.init( resident_allocator, scene->meshes.size * 2, scene->meshes.size * 2 );
    gpu_mesh_instances.init( resident_allocator, scene->mesh_instances.size, scene->mesh_instances.size );

    // Gpu materials needs meshlets buffer to be created first, so update them later.
    for ( u32 i = 0; i < scene->meshes.size; ++i ) {
        copy_gpu_material_data( *renderer->gpu, gpu_materials[ i ], scene->meshes[ i ] );
        gpu_mesh_bounds[ i ] = scene->meshes[ i ].bounding_sphere;

        gpu_mesh_aabbs[ i * 2 ] = glm::vec4( scene->meshes[ i ].aabb[ 0 ], 0.f );
        gpu_mesh_aabbs[ i * 2 + 1 ] = glm::vec4( scene->meshes[ i ].aabb[ 1 ], 0.f );
    }

    for ( u32 mi = 0; mi < scene->mesh_instances.size; ++mi ) {
        copy_gpu_mesh_transform( gpu_mesh_instances[ mi ], *scene, scene->mesh_instances[ mi ], render_config.global_scale, scene_graph );
    }

    // Create mesh/material data.
    render_blackboard.meshes_sb = renderer->create_buffer_with_upload( {
            .size = sizeof( GpuMaterialData ) * scene->meshes.size,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .name = "meshes_sb"
        }, {
            .data = gpu_materials.data,
            .policy = BufferUploadPolicy::Transfer } );

    // Create mesh bounding spheres.
    render_blackboard.mesh_bounds_sb = renderer->create_buffer_with_upload( {
            .size = sizeof( glm::vec4 ) * scene->meshes.size,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .name = "mesh_bounds_sb"
        }, {
            .data = gpu_mesh_bounds.data,
            .policy = BufferUploadPolicy::Transfer } );

    // Create mesh AABBs.
    render_blackboard.mesh_aabbs_sb = renderer->create_buffer_with_upload( {
            .size = sizeof( glm::vec4 ) * scene->meshes.size * 2,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .name = "mesh_aabbs_sb"
        }, {
            .data = gpu_mesh_aabbs.data,
            .policy = BufferUploadPolicy::Transfer } );

    // Create mesh instances.
    render_blackboard.mesh_instances_sb = renderer->create_buffer_with_upload( {
        .size = sizeof( GpuMeshInstanceData ) * scene->mesh_instances.size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
            .name = "mesh_instances_sb"
        }, {
            .data = gpu_mesh_instances.data,
            .policy = BufferUploadPolicy::Transfer } );

    // Create indirect buffers, dynamic so need multiple buffering.
    for ( u32 i = 0; i < k_max_frames; ++i ) {
        render_blackboard.meshlet_emulation_instances_indirect_count_sb[ i ] = renderer->gpu->create_buffer( {
            .size = sizeof( u32 ) * 4,
            .usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .name = "meshlet_instances_indirect_count_sb" } );
    }

    /*render_blackboard.physics_cb = renderer->gpu->create_buffer( {
                .type_flags = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                .usage = ResourceUsageType::Immutable,
                .size = (u32)sizeof( PhysicsSceneData ),
                .name = "physics_cb" } );*/


    // Features //////////////////////////////////////////////////////////
    if ( meshes ) {
        meshes->create_gpu_resources( renderer, &render_blackboard, scene );
    }

    if ( debug_draw ) {
        debug_draw->create_gpu_resources( renderer, &render_blackboard, frame_graph );
    }

    if ( gpu_culling ) {
        gpu_culling->create_gpu_resources( renderer, &render_blackboard, scene );
    }

    if ( lighting ) {
        lighting->create_gpu_resources( renderer, &render_blackboard );
    }

    if ( meshlets ) {
        meshlets->create_gpu_resources( renderer, &render_blackboard, scene );
    }

    if ( post ) {
        post->create_gpu_resources( renderer, &render_blackboard, frame_graph );
    }

    if ( point_shadows ) {
        point_shadows->create_gpu_resources( renderer, &render_blackboard, scene );
    }

    FrameGraphResourceContext resource_context{ renderer, frame_graph, &render_blackboard, &render_config, scene };

    for ( u32 i = 0; i < render_passes.size; ++i ) {
        render_passes[ i ]->create_gpu_resources( resource_context );
    }

    // TODO [gabriel]: cleanup code to have dependent resources created in the update_dependent_resources
    // method instead of the prepare draw.
    // For now call individually the debug method to cache ddgi stuff.
    FrameGraphRenderPass* debug_pass = frame_graph->builder->get_render_pass<FrameGraphRenderPass>( "debug_pass" );
    if ( debug_pass != nullptr ) {
        debug_pass->update_dependent_resources( resource_context );
    }
}

void FrameRenderer::update_dependent_resources() {

    FrameGraphResourceContext resource_context{ renderer, frame_graph, &render_blackboard, &render_config, scene };

    for ( u32 i = 0; i < render_passes.size; ++i ) {
        render_passes[ i ]->update_dependent_resources( resource_context );
    }
}

void FrameRenderer::reload_psos() {

    FrameGraphResourceContext resource_context{ renderer, frame_graph, &render_blackboard, &render_config, scene };

    for ( u32 i = 0; i < render_passes.size; ++i ) {
        render_passes[ i ]->update_psos( resource_context, PipelineUpdatePhase::Reload );
    }

    if ( post ) {
        post->update_psos( renderer, frame_graph, PipelineUpdatePhase::Reload );
    }
}

// DrawTask ///////////////////////////////////////////////////////////////
void DrawTask::init( GpuDevice* gpu_, FrameGraph* frame_graph_, Renderer* renderer_,
                     ImGuiService* imgui_, GpuVisualProfiler* gpu_visual_profiler_, RenderScene* scene_,
                     FrameRenderer* frame_renderer_ ) {
    gpu = gpu_;
    frame_graph = frame_graph_;
    renderer = renderer_;
    imgui = imgui_;
    gpu_visual_profiler = gpu_visual_profiler_;
    scene = scene_;
    frame_renderer = frame_renderer_;

    current_frame_index = gpu->current_frame;
}

void DrawTask::ExecuteRange( enki::TaskSetPartition range_, uint32_t threadnum_ ) {
    ZoneScoped;

    using namespace raptor;

    thread_id = threadnum_;

    // TODO: improve getting a command buffer/pool
    GpuProfiler* gpu_profiler = gpu->gpu_profiler;
    CommandBuffer* gfx_cb = gpu->allocate_command_buffer( threadnum_, current_frame_index, CommandQueueType::Graphics );
    gfx_cb->begin();
    gpu_profiler->begin_command_buffer( gfx_cb );

    gfx_cb->push_marker( "Frame" );

    frame_graph->render( current_frame_index, thread_id, renderer, &frame_renderer->main_view, &frame_renderer->render_blackboard, &frame_renderer->render_config );

    gfx_cb->push_marker( "PostProcess" );

    // Choose the final texture to present, if TAA is enabled use the TAA output, otherwise use the main output.
    FrameGraphResource* texture = frame_graph->get_resource( "final" );
    RASSERT( texture != nullptr );

    RenderConfig& render_config = frame_renderer->render_config;
    RenderBlackboard& render_blackboard = frame_renderer->render_blackboard;

    ImageViewHandle final_image_view = texture->resource_info.texture.image_view;
    if ( render_config.taa.enabled && render_blackboard.taa_output_image_view.is_valid() ) {
        final_image_view = render_blackboard.taa_output_image_view;
    }
    ImageHandle final_image = gpu->get_image_view( final_image_view )->parent_image;

    gfx_cb->add_image_barrier( final_image, range_color_full(),
                               { VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                 VK_ACCESS_2_SHADER_READ_BIT,
                                 VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL } );
    gfx_cb->add_image_barrier( gpu->get_current_swapchain_image(), range_color_full(),
                               { VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                 VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL } );
    gfx_cb->add_image_barrier( gpu->get_current_swapchain_depth_image(), range_depth_full(),
                               { VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                 VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL } );
    gfx_cb->flush_barriers();

    gfx_cb->clear( 0.3f, 0.3f, 0.3f, 1.f, 0 );
    gfx_cb->clear_depth_stencil( 1.0f, 0 );

    // Swapchain draw
    gfx_cb->begin_render_pass( { gpu->get_current_swapchain_image_view() }, { VK_ATTACHMENT_LOAD_OP_CLEAR }, { {.3f,.3f,.3f,1.f} },
                               gpu->get_current_swapchain_depth_image_view(), VK_ATTACHMENT_LOAD_OP_CLEAR,
                               gfx_cb->clear_values[ CommandBuffer::k_depth_stencil_clear_index ] );

    gfx_cb->set_fullscreen_scissor();
    gfx_cb->set_fullscreen_viewport();
    gfx_cb->set_depth_bias_enabled( false );

    // Apply fullscreen material

    gfx_cb->bind_pipeline( frame_renderer->render_config.use_slang_shaders ?
                           frame_renderer->post->main_post_pipeline_slang.pipeline :
                           frame_renderer->post->main_post_pipeline.pipeline );
    gfx_cb->bind_descriptor_set(
        { gpu->bindless_descriptor_set, frame_renderer->post->fullscreen_ds },
        { frame_renderer->post->post_cb_offset } );
    gfx_cb->draw( TopologyType::Triangle, 0, 3, final_image_view.index(), 1 );

    imgui->render( *gfx_cb, false );

    gfx_cb->end_render_pass();

    gfx_cb->pop_marker(); // PostProcess marker

    gfx_cb->add_image_barrier( gpu->get_current_swapchain_image(), range_color_full(),
                               { VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                 VK_ACCESS_2_NONE,
                                 VK_IMAGE_LAYOUT_PRESENT_SRC_KHR } );
    // Release ownership
    if ( gpu->vulkan_compute_queue_family != gpu->vulkan_main_queue_family ) {

        gfx_cb->release_image_ownership( texture->resource_info.texture.image, range_color_full(),
                                         gpu->vulkan_compute_queue_family );
    }
    else {
        // If no ownership, perform the transition to be copied from in the following frame,
        // but do it in the same queue so it is enforced/waited in a more safe way.
        gfx_cb->add_image_barrier( texture->resource_info.texture.image, range_color_full(),
                                   { VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                   VK_ACCESS_2_TRANSFER_READ_BIT ,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL });
    }
    gfx_cb->flush_barriers();

    gfx_cb->pop_marker(); // Frame marker

    gpu_visual_profiler->update( *gpu );

    // Send commands to GPU
    gpu_profiler->end_command_buffer( gfx_cb );
    gfx_cb->end();

    // TODO:
    this->gfx_cb = gfx_cb;
}

// DebugDrawRenderingFeature /////////////////////////////////////////////
const uint k_max_lines_3d_per_frame = 90000u;
const uint k_max_lines_2d_px_per_frame = 10000u;

// Derivati
const uint k_max_vertices_3d_per_frame = k_max_lines_3d_per_frame * 2u;
const uint k_max_vertices_2d_px_per_frame = k_max_lines_2d_px_per_frame * 2u;

const uint k_frame_stride_vertices = k_max_vertices_3d_per_frame + k_max_vertices_2d_px_per_frame;

const uint k_total_vertices = k_frame_stride_vertices * k_max_frames;


struct GpuDebugDrawCountsSlice {
    u32 vertex_count_3d;
    u32 vertex_count_2d_px;

    u32 pad0;
    u32 pad1;
};

struct GpuDebugDrawCounts {
    GpuDebugDrawCountsSlice counts[ k_max_frames ];

    u32 frame_slot;
    u32 pad0;
};

void DebugDrawRenderingFeature::create_gpu_resources( Renderer* renderer, RenderBlackboard* render_blackboard,
                                                      FrameGraph* frame_graph ) {

    GpuDevice* gpu = renderer->gpu;
    DebugDrawRuntimeData& debug = render_blackboard->debug_draw;

    // Staging line buffers
    lines.init( &MemoryService::instance()->system_allocator, k_max_lines_3d_per_frame );
    lines_2d.init( &MemoryService::instance()->system_allocator, k_max_lines_2d_px_per_frame );

    // GPU-written debug vertices.
    debug.gpu_line_sb = renderer->gpu->create_buffer( {
        .size = k_total_vertices * sizeof( glm::vec4 ),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .name = "debug_line_sb" } );

    // CPU reset + GPU-written counters.
    debug.gpu_line_count_sb = renderer->gpu->create_buffer( {
        .size = sizeof( GpuDebugDrawCounts ),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                            VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .name = "debug_line_count_sb" } );

    // GPU-generated indirect commands.
    debug.gpu_line_commands_sb = renderer->gpu->create_buffer( {
        .size = sizeof( VkDrawIndirectCommand ) * 2,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
        .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .name = "debug_line_commands_sb" } );

    // CPU-written debug vertices.
    debug.cpu_lines_vb = renderer->gpu->create_buffer( {
        .size = sizeof( LineVertex ) * k_max_lines_3d_per_frame,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                            VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .name = "lines_vb" } );

    debug.cpu_lines2d_vb = renderer->gpu->create_buffer( {
        .size = sizeof( LineVertex ) * k_max_lines_2d_px_per_frame,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                            VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .name = "lines_vb_2d" } );
}

void DebugDrawRenderingFeature::destroy_gpu_resources( Renderer* renderer, RenderBlackboard* render_blackboard ) {

    GpuDevice* gpu = renderer->gpu;
    DebugDrawRuntimeData& debug = render_blackboard->debug_draw;

    gpu->destroy_buffer( debug.gpu_line_sb );
    gpu->destroy_buffer( debug.gpu_line_count_sb );
    gpu->destroy_buffer( debug.gpu_line_commands_sb );

    gpu->destroy_buffer( debug.cpu_lines_vb );
    gpu->destroy_buffer( debug.cpu_lines2d_vb );

    lines.shutdown();
    lines_2d.shutdown();
}

void DebugDrawRenderingFeature::on_resize( Renderer* renderer, RenderBlackboard* render_blackboard, u32 new_width, u32 new_height ) {

}

void DebugDrawRenderingFeature::upload_gpu_data( UploadGpuDataContext& context ) {

    GpuDevice* gpu = context.renderer->gpu;
    DebugDrawRuntimeData& debug = context.render_blackboard.debug_draw;

    // Cache line counts
    debug.cpu_lines_count = lines.size;
    debug.cpu_lines2d_count = lines_2d.size;

    // Copy lines to vertex buffer
    if ( lines.size ) {
        const VkDeviceSize mapping_size = sizeof( LineVertex ) * lines.size;

        Buffer* buffer = gpu->get_buffer( debug.cpu_lines_vb );
        memcpy( buffer->mapped_data, lines.data, mapping_size );
        gpu->flush_buffer( debug.cpu_lines_vb, 0, mapping_size );

        lines.clear();
    }

    if ( lines_2d.size ) {
        const VkDeviceSize mapping_size = sizeof( LineVertex ) * lines_2d.size;

        Buffer* buffer = gpu->get_buffer( debug.cpu_lines2d_vb );
        memcpy( buffer->mapped_data, lines_2d.data, mapping_size );
        gpu->flush_buffer( debug.cpu_lines2d_vb, 0, mapping_size );

        lines_2d.clear();
    }

    // Reset debug draw counts
    {
        Buffer* buffer = gpu->get_buffer( debug.gpu_line_count_sb );
        GpuDebugDrawCounts* debug_line_count = ( GpuDebugDrawCounts* )buffer->mapped_data;

        const u32 frame_slot = gpu->current_frame;

        debug_line_count->counts[ frame_slot ].vertex_count_3d = 0;
        debug_line_count->counts[ frame_slot ].vertex_count_2d_px = 0;
        debug_line_count->frame_slot = frame_slot;

        gpu->flush_buffer( debug.gpu_line_count_sb, 0, sizeof( GpuDebugDrawCounts ) );
    }
}

void DebugDrawRenderingFeature::line( const glm::vec3& from, const glm::vec3& to, Color color ) {
    line( from, to, color, color );
}

void DebugDrawRenderingFeature::line_2d( const glm::vec2& from, const glm::vec2& to, Color color ) {
    if ( lines_2d.size >= k_max_lines_2d_px_per_frame - 1 ) {
        return;
    }

    LineVertex& v0 = lines_2d.push_use();
    v0.set( from, color );

    LineVertex& v1 = lines_2d.push_use();
    v1.set( to, color );
}

void DebugDrawRenderingFeature::line( const glm::vec3& from, const glm::vec3& to, Color color0, Color color1 ) {
    if ( lines.size >= k_max_lines_3d_per_frame - 1 ) {
        return;
    }

    LineVertex& v0 = lines.push_use();
    v0.set( from, color0 );

    LineVertex& v1 = lines.push_use();
    v1.set( to, color1 );
}

void DebugDrawRenderingFeature::aabb( const glm::vec3& min, const glm::vec3 max, Color color ) {

    const f32 x0 = min.x;
    const f32 y0 = min.y;
    const f32 z0 = min.z;
    const f32 x1 = max.x;
    const f32 y1 = max.y;
    const f32 z1 = max.z;

    line( { x0, y0, z0 }, { x0, y1, z0 }, color, color );
    line( { x0, y1, z0 }, { x1, y1, z0 }, color, color );
    line( { x1, y1, z0 }, { x1, y0, z0 }, color, color );
    line( { x1, y0, z0 }, { x0, y0, z0 }, color, color );
    line( { x0, y0, z0 }, { x0, y0, z1 }, color, color );
    line( { x0, y1, z0 }, { x0, y1, z1 }, color, color );
    line( { x1, y1, z0 }, { x1, y1, z1 }, color, color );
    line( { x1, y0, z0 }, { x1, y0, z1 }, color, color );
    line( { x0, y0, z1 }, { x0, y1, z1 }, color, color );
    line( { x0, y1, z1 }, { x1, y1, z1 }, color, color );
    line( { x1, y1, z1 }, { x1, y0, z1 }, color, color );
    line( { x1, y0, z1 }, { x0, y0, z1 }, color, color );
}

// MathHelpers

static inline bool is_near_zero( const glm::vec3& v, float eps = 1e-6f ) {
    return glm::dot( v, v ) < eps * eps;
}

static inline glm::vec3 any_orthonormal( const glm::vec3& n_unit ) {
    const glm::vec3 a = ( glm::abs( n_unit.z ) < 0.999f ) ? glm::vec3( 0, 0, 1 )
        : glm::vec3( 0, 1, 0 );
    return glm::normalize( glm::cross( a, n_unit ) );
}

static inline void build_tangent_basis( const glm::vec3& n_unit,
                                        glm::vec3& t, glm::vec3& b ) {
    t = any_orthonormal( n_unit );
    b = glm::normalize( glm::cross( n_unit, t ) );
}

// Core: circle / arc wire
void DebugDrawRenderingFeature::circle_wire( const glm::vec3& center,
                                             const glm::vec3& normal,
                                             float radius, Color color,
                                             int segments ) {
    if ( segments < 3 || radius <= 0.0f || is_near_zero( normal ) ) {
        return;
    }

    const glm::vec3 n = glm::normalize( normal );
    glm::vec3 t, b;
    build_tangent_basis( n, t, b );

    glm::vec3 prev = center + t * radius;
    const float step = glm::two_pi<float>() / float( segments );

    for ( int i = 1; i <= segments; ++i ) {
        const float a = step * float( i );
        const glm::vec3 p = center + ( t * glm::cos( a ) + b * glm::sin( a ) ) * radius;
        line( prev, p, color );
        prev = p;
    }
}

void DebugDrawRenderingFeature::arc_wire( const glm::vec3& center,
                                          const glm::vec3& normal,
                                          float radius, float start_angle_rad,
                                          float end_angle_rad, Color color,
                                          int segments ) {
    if ( segments < 2 || radius <= 0.0f || is_near_zero( normal ) ) {
        return;
    }

    const glm::vec3 n = glm::normalize( normal );
    glm::vec3 t, b;
    build_tangent_basis( n, t, b );

    const float range = end_angle_rad - start_angle_rad;
    const float step = range / float( segments );

    glm::vec3 prev = center + ( t * glm::cos( start_angle_rad ) +
                                b * glm::sin( start_angle_rad ) ) * radius;

    for ( int i = 1; i <= segments; ++i ) {
        const float a = start_angle_rad + step * float( i );
        const glm::vec3 p = center + ( t * glm::cos( a ) + b * glm::sin( a ) ) * radius;
        line( prev, p, color );
        prev = p;
    }
}

// Sphere wire: 3 great circles + optional latitude rings

void DebugDrawRenderingFeature::sphere_wire( const glm::vec3& center, float radius,
                                             Color color, int segments,
                                             int latitude_rings ) {
    if ( radius <= 0.0f ) {
        return;
    }

    circle_wire( center, glm::vec3( 0, 0, 1 ), radius, color, segments );
    circle_wire( center, glm::vec3( 0, 1, 0 ), radius, color, segments );
    circle_wire( center, glm::vec3( 1, 0, 0 ), radius, color, segments );

    if ( latitude_rings <= 0 ) {
        return;
    }

    for ( int r = 1; r <= latitude_rings; ++r ) {
        const float v = float( r ) / float( latitude_rings + 1 );
        const float phi = glm::pi<float>() * ( v - 0.5f ); // (-pi/2..pi/2)
        const float z = glm::sin( phi ) * radius;
        const float rr = glm::cos( phi ) * radius;

        circle_wire( center + glm::vec3( 0, 0, z ), glm::vec3( 0, 0, 1 ),
                     rr, color, segments );
    }
}

// Cone wire (finite): apex -> base_center (axis), base circle, spokes

void DebugDrawRenderingFeature::cone_wire( const glm::vec3& apex,
                                           const glm::vec3& axis,
                                           float base_radius, Color color,
                                           int segments, int side_spokes ) {
    if ( base_radius <= 0.0f || is_near_zero( axis ) ) {
        return;
    }

    const glm::vec3 n = glm::normalize( axis );
    const glm::vec3 base_center = apex + axis;

    circle_wire( base_center, n, base_radius, color, segments );

    glm::vec3 t, b;
    build_tangent_basis( n, t, b );

    side_spokes = glm::clamp( side_spokes, 3, glm::max( 3, segments ) );
    for ( int i = 0; i < side_spokes; ++i ) {
        const float a = glm::two_pi<float>() * ( float( i ) / float( side_spokes ) );
        const glm::vec3 p = base_center + ( t * glm::cos( a ) + b * glm::sin( a ) ) * base_radius;
        line( apex, p, color );
    }

    line( apex, base_center, color );
}

// Cylinder wire: two circles + vertical lines

void DebugDrawRenderingFeature::cylinder_wire( const glm::vec3& center0,
                                               const glm::vec3& center1,
                                               float radius, Color color,
                                               int segments, int vertical_lines ) {
    const glm::vec3 axis = center1 - center0;
    if ( radius <= 0.0f || is_near_zero( axis ) ) {
        return;
    }

    const glm::vec3 n = glm::normalize( axis );

    circle_wire( center0, n, radius, color, segments );
    circle_wire( center1, n, radius, color, segments );

    glm::vec3 t, b;
    build_tangent_basis( n, t, b );

    vertical_lines = glm::clamp( vertical_lines, 3, glm::max( 3, segments ) );
    for ( int i = 0; i < vertical_lines; ++i ) {
        const float a = glm::two_pi<float>() * ( float( i ) / float( vertical_lines ) );
        const glm::vec3 p0 = center0 + ( t * glm::cos( a ) + b * glm::sin( a ) ) * radius;
        const glm::vec3 p1 = center1 + ( t * glm::cos( a ) + b * glm::sin( a ) ) * radius;
        line( p0, p1, color );
    }
}

// Plane wire: grid centered at plane_point, oriented by plane_normal
void DebugDrawRenderingFeature::plane_grid_wire( const glm::vec3& plane_point,
                                                 const glm::vec3& plane_normal,
                                                 float half_extent,
                                                 float cell_size, Color color,
                                                 int bold_every,
                                                 Color bold_color ) {
    if ( half_extent <= 0.0f || cell_size <= 0.0f || is_near_zero( plane_normal ) ) {
        return;
    }

    const glm::vec3 n = glm::normalize( plane_normal );
    glm::vec3 t, b;
    build_tangent_basis( n, t, b );

    const int cells = int( glm::floor( half_extent / cell_size ) );
    const float extent = float( cells ) * cell_size;

    for ( int i = -cells; i <= cells; ++i ) {
        const float o = float( i ) * cell_size;
        const bool is_bold = ( bold_every > 0 ) && ( ( i % bold_every ) == 0 );
        const Color c = is_bold ? bold_color : color;

        const glm::vec3 p0 = plane_point + b * o - t * extent;
        const glm::vec3 p1 = plane_point + b * o + t * extent;
        line( p0, p1, c );

        const glm::vec3 q0 = plane_point + t * o - b * extent;
        const glm::vec3 q1 = plane_point + t * o + b * extent;
        line( q0, q1, c );
    }

    line( plane_point, plane_point + n * ( half_extent * 0.25f ), bold_color );
}

// Frustum wire: from 8 corners (0..3 near ccw, 4..7 far ccw)

void DebugDrawRenderingFeature::frustum_wire( const glm::vec3 corners[ 8 ],
                                              Color color ) {
    line( corners[ 0 ], corners[ 1 ], color );
    line( corners[ 1 ], corners[ 2 ], color );
    line( corners[ 2 ], corners[ 3 ], color );
    line( corners[ 3 ], corners[ 0 ], color );

    line( corners[ 4 ], corners[ 5 ], color );
    line( corners[ 5 ], corners[ 6 ], color );
    line( corners[ 6 ], corners[ 7 ], color );
    line( corners[ 7 ], corners[ 4 ], color );

    line( corners[ 0 ], corners[ 4 ], color );
    line( corners[ 1 ], corners[ 5 ], color );
    line( corners[ 2 ], corners[ 6 ], color );
    line( corners[ 3 ], corners[ 7 ], color );
}

void DebugDrawRenderingFeature::frustum_wire_from_inv_viewproj(
    const glm::mat4& inv_view_proj, Color color ) {

    glm::vec3 c[ 8 ];

    auto unproject = [ & ]( float x, float y, float z ) -> glm::vec3 {
        const glm::vec4 p = inv_view_proj * glm::vec4( x, y, z, 1.0f );
        return glm::vec3( p ) / p.w;
        };

    //// Vulkan NDC: z in [0, 1]
    c[ 0 ] = unproject( -1, -1, 0 );
    c[ 1 ] = unproject( 1, -1, 0 );
    c[ 2 ] = unproject( 1, 1, 0 );
    c[ 3 ] = unproject( -1, 1, 0 );

    c[ 4 ] = unproject( -1, -1, 1 );
    c[ 5 ] = unproject( 1, -1, 1 );
    c[ 6 ] = unproject( 1, 1, 1 );
    c[ 7 ] = unproject( -1, 1, 1 );

    frustum_wire( c, color );
}

// OBB wire: center + orthonormal axes + half extents

void DebugDrawRenderingFeature::obb_wire( const glm::vec3& center,
                                          const glm::vec3& axis_x,
                                          const glm::vec3& axis_y,
                                          const glm::vec3& axis_z,
                                          const glm::vec3& half_extents,
                                          Color color ) {
    const glm::vec3 ex = axis_x * half_extents.x;
    const glm::vec3 ey = axis_y * half_extents.y;
    const glm::vec3 ez = axis_z * half_extents.z;

    glm::vec3 c[ 8 ];
    c[ 0 ] = center - ex - ey - ez;
    c[ 1 ] = center + ex - ey - ez;
    c[ 2 ] = center + ex + ey - ez;
    c[ 3 ] = center - ex + ey - ez;
    c[ 4 ] = center - ex - ey + ez;
    c[ 5 ] = center + ex - ey + ez;
    c[ 6 ] = center + ex + ey + ez;
    c[ 7 ] = center - ex + ey + ez;

    line( c[ 0 ], c[ 1 ], color );
    line( c[ 1 ], c[ 2 ], color );
    line( c[ 2 ], c[ 3 ], color );
    line( c[ 3 ], c[ 0 ], color );

    line( c[ 4 ], c[ 5 ], color );
    line( c[ 5 ], c[ 6 ], color );
    line( c[ 6 ], c[ 7 ], color );
    line( c[ 7 ], c[ 4 ], color );

    line( c[ 0 ], c[ 4 ], color );
    line( c[ 1 ], c[ 5 ], color );
    line( c[ 2 ], c[ 6 ], color );
    line( c[ 3 ], c[ 7 ], color );
}

// Ray + arrow (direction, normals, velocities)

void DebugDrawRenderingFeature::ray_wire( const glm::vec3& origin,
                                          const glm::vec3& dir,
                                          float length, Color color ) {
    if ( length <= 0.0f || is_near_zero( dir ) ) {
        return;
    }

    const glm::vec3 d = glm::normalize( dir );
    line( origin, origin + d * length, color );
}

void DebugDrawRenderingFeature::arrow_wire( const glm::vec3& origin,
                                            const glm::vec3& dir,
                                            float length, float head_length,
                                            float head_radius, Color color,
                                            int head_segments ) {
    if ( length <= 0.0f || is_near_zero( dir ) ) {
        return;
    }

    const glm::vec3 d = glm::normalize( dir );
    const glm::vec3 tip = origin + d * length;
    const glm::vec3 head_base = tip - d * head_length;

    line( origin, head_base, color );

    //// Note: cone_wire expects (apex, axis_to_base_center)
    cone_wire( tip, ( head_base - tip ), head_radius, color, head_segments, 6 );
}

void DebugDrawRenderingFeature::position_cross_wire( const glm::vec3& position,
                                                     float size, Color color ) {
    const float s = size * 0.5f;

    line( position + glm::vec3( -s, 0, 0 ),
          position + glm::vec3( s, 0, 0 ), color );

    line( position + glm::vec3( 0, -s, 0 ),
          position + glm::vec3( 0, s, 0 ), color );

    line( position + glm::vec3( 0, 0, -s ),
          position + glm::vec3( 0, 0, s ), color );
}

void DebugDrawRenderingFeature::axis_gizmo_wire( const glm::vec3& position,
                                                 const glm::vec3& axis_x,
                                                 const glm::vec3& axis_y,
                                                 const glm::vec3& axis_z,
                                                 float size ) {
    line( position, position + axis_x * size, Color::red() );
    line( position, position + axis_y * size, Color::green() );
    line( position, position + axis_z * size, Color::blue() );
}

void DebugDrawRenderingFeature::point_light_wire( const glm::vec3& position,
                                                  float radius, Color color,
                                                  float cross_size ) {
    position_cross_wire( position, cross_size, color );

    if ( radius > 0.0f ) {
        sphere_wire( position, radius, color, 24, 0 );
    }
}


// Math Utils ////////////////////////////////////////////////////////////
glm::vec3 extract_scale( const glm::mat4& m ) {

    //// GLM is column-major:
    //// m[0] = X axis (basis)
    //// m[1] = Y axis
    //// m[2] = Z axis

    const glm::vec3 x_axis( m[ 0 ].x, m[ 0 ].y, m[ 0 ].z );
    const glm::vec3 y_axis( m[ 1 ].x, m[ 1 ].y, m[ 1 ].z );
    const glm::vec3 z_axis( m[ 2 ].x, m[ 2 ].y, m[ 2 ].z );

    glm::vec3 scale;
    scale.x = glm::length( x_axis );
    scale.y = glm::length( y_axis );
    scale.z = glm::length( z_axis );

    return scale;
}

// CGLM converted method from:
// 2D Polyhedral Bounds of a Clipped, Perspective - Projected 3D Sphere
// By Michael Mara Morgan McGuire
void get_bounds_for_axis( const glm::vec3& a, // Bounding axis (camera space)
                          const glm::vec3& C, // Sphere center (camera space)
                          float r, // Sphere radius
                          float nearZ, // Near clipping plane (negative)
                          glm::vec3& L, // Tangent point (camera space)
                          glm::vec3& U ) { // Tangent point (camera space)

    const glm::vec2 c{ glm::dot( a, C ), C.z }; // C in the a-z frame
    glm::vec2 bounds[ 2 ]; // In the a-z reference frame
    const float tSquared = glm::dot( c, c ) - ( r * r );
    const bool cameraInsideSphere = ( tSquared <= 0 );
    // (cos, sin) of angle theta between c and a tangent vector
    glm::vec2 v = cameraInsideSphere ? glm::vec2{ 0.0f, 0.0f } : glm::vec2{ sqrt( tSquared ), r } / glm::length( c );
    // Does the near plane intersect the sphere?
    const bool clipSphere = ( c.y + r >= nearZ );
    // Square root of the discriminant; NaN (and unused)
    // if the camera is in the sphere
    float k = sqrt( ( r * r ) - ( ( nearZ - c.y ) * ( nearZ - c.y ) ) );
    for ( int i = 0; i < 2; ++i ) {
        if ( !cameraInsideSphere ) {
            glm::mat2 transform{ v.x, -v.y,
                              v.y, v.x };

            bounds[ i ] = transform * ( c * v.x );
        }

        const bool clipBound = cameraInsideSphere || ( bounds[ i ].y > nearZ );

        if ( clipSphere && clipBound ) {
            bounds[ i ] = glm::vec2{ c.x + k, nearZ };
        }

        // Set up for the lower bound
        v.y = -v.y; k = -k;
    }
    // Transform back to camera space
    L = a * bounds[ 1 ].x;
    L.z = bounds[ 1 ].y;
    U = a * bounds[ 0 ].x;
    U.z = bounds[ 0 ].y;
}

// Analytic 2D bounds of a clipped, perspective-projected 3D sphere
// Adapted to RH view space (camera looks along -Z, z<0 in front).
//
// a_vs: axis in view space (unit vector, e.g. (1,0,0) or (0,1,0))
// C_vs: sphere center in view space (z<0 in front)
// r:    sphere radius
// near_plane: positive near plane distance
// L_vs, U_vs: tangent points in view space along axis a_vs
void get_bounds_for_axis_rh( const glm::vec3& a_vs,
                             const glm::vec3& C_vs,
                             float r,
                             float near_plane,
                             glm::vec3& L_vs,
                             glm::vec3& U_vs ) {

    // Convert to +Z-forward camera space (McGuire convention)
    // In this space, z_plus > 0 for visible points
    glm::vec3 C_plus{ C_vs.x, C_vs.y, -C_vs.z };
    float nearZ = near_plane; // positive

    const glm::vec3& a = a_vs; // axis is unchanged (we don't flip it)

    // C in the 2D (a, z) frame
    glm::vec2 c{ glm::dot( a, C_plus ), C_plus.z };

    glm::vec2 bounds[ 2 ];  // in (a,z) frame
    const float tSquared = glm::dot( c, c ) - ( r * r );
    const bool cameraInsideSphere = ( tSquared <= 0.0f );

    // (cos ?, sin ?) of angle between c and each tangent direction
    glm::vec2 v;
    if ( cameraInsideSphere ) {
        v = glm::vec2{ 0.0f, 0.0f };
    } else {
        float t = sqrtf( tSquared );
        float len_c = glm::length( c );
        v = glm::vec2{ t, r } / len_c;
    }

    // Does the near plane intersect the sphere?
    const bool clipSphere = ( c.y + r >= nearZ );

    // Discriminant for intersection with near plane
    float k = sqrtf( r * r - ( ( nearZ - c.y ) * ( nearZ - c.y ) ) );

    for ( int i = 0; i < 2; ++i ) {
        if ( !cameraInsideSphere ) {
            // Rotation matrix from v = (cos ?, sin ?)
            glm::mat2 transform{ v.x, -v.y,
                             v.y,  v.x };

            // Scale c by cos ? (v.x), then rotate
            bounds[ i ] = transform * ( c * v.x );
        }

        const bool clipBound = cameraInsideSphere || ( bounds[ i ].y > nearZ );

        if ( clipSphere && clipBound ) {
            // Replace bound by its intersection with the near plane
            bounds[ i ] = glm::vec2{ c.x + k, nearZ };
        }

        // Prepare for the second bound (lower)
        v.y = -v.y;
        k = -k;
    }

    // Transform back to +Z-forward camera space
    glm::vec3 L_plus = a * bounds[ 1 ].x;
    L_plus.z = bounds[ 1 ].y;

    glm::vec3 U_plus = a * bounds[ 0 ].x;
    U_plus.z = bounds[ 0 ].y;

    // Convert back to RH view space (z_vs = -z_plus)
    L_vs = glm::vec3{ L_plus.x, L_plus.y, -L_plus.z };
    U_vs = glm::vec3{ U_plus.x, U_plus.y, -U_plus.z };
}


glm::vec3 project( const glm::mat4& P, const glm::vec3& Q ) {
    glm::vec4 v = P * glm::vec4{ Q.x, Q.y, Q.z, 1.0f };
    v = v / v.w;

    return glm::vec3{ v.x, v.y, v.z };
}

void project_aabb_cubemap_positive_x( const glm::vec3 aabb[ 2 ], f32& s_min, f32& s_max, f32& t_min, f32& t_max ) {
    f32 rd_min = 1.f / glm::max( FLT_EPSILON, aabb[ 0 ].x );
    f32 rd_max = 1.f / glm::max( FLT_EPSILON, aabb[ 1 ].x );

    s_min = glm::min( -aabb[ 1 ].z * rd_min, -aabb[ 1 ].z * rd_max );
    s_max = glm::max( -aabb[ 0 ].z * rd_min, -aabb[ 0 ].z * rd_max );

    t_min = glm::min( -aabb[ 1 ].y * rd_min, -aabb[ 1 ].y * rd_max );
    t_max = glm::max( -aabb[ 0 ].y * rd_min, -aabb[ 0 ].y * rd_max );
}

void project_aabb_cubemap_negative_x( const glm::vec3 aabb[ 2 ], f32& s_min, f32& s_max, f32& t_min, f32& t_max ) {
    f32 rd_min = 1.f / glm::max( FLT_EPSILON, -aabb[ 0 ].x );
    f32 rd_max = 1.f / glm::max( FLT_EPSILON, -aabb[ 1 ].x );

    s_min = glm::min( aabb[ 0 ].z * rd_min, aabb[ 0 ].z * rd_max );
    s_max = glm::max( aabb[ 1 ].z * rd_min, aabb[ 1 ].z * rd_max );

    t_min = glm::min( -aabb[ 1 ].y * rd_min, -aabb[ 1 ].y * rd_max );
    t_max = glm::max( -aabb[ 0 ].y * rd_min, -aabb[ 0 ].y * rd_max );
}

void project_aabb_cubemap_positive_y( const glm::vec3 aabb[ 2 ], f32& s_min, f32& s_max, f32& t_min, f32& t_max ) {
    f32 rd_min = 1.f / glm::max( FLT_EPSILON, aabb[ 0 ].y );
    f32 rd_max = 1.f / glm::max( FLT_EPSILON, aabb[ 1 ].y );

    s_min = glm::min( -aabb[ 1 ].x * rd_min, -aabb[ 1 ].x * rd_max );
    s_max = glm::max( -aabb[ 0 ].x * rd_min, -aabb[ 0 ].x * rd_max );
    t_min = glm::min( -aabb[ 1 ].z * rd_min, -aabb[ 1 ].z * rd_max );
    t_max = glm::max( -aabb[ 0 ].z * rd_min, -aabb[ 0 ].z * rd_max );
}

void project_aabb_cubemap_negative_y( const glm::vec3 aabb[ 2 ], f32& s_min, f32& s_max, f32& t_min, f32& t_max ) {
    f32 rd_min = 1.f / glm::max( FLT_EPSILON, -aabb[ 0 ].y );
    f32 rd_max = 1.f / glm::max( FLT_EPSILON, -aabb[ 1 ].y );

    s_min = glm::min( aabb[ 0 ].x * rd_min, aabb[ 0 ].x * rd_max );
    s_max = glm::max( aabb[ 1 ].x * rd_min, aabb[ 1 ].x * rd_max );

    t_min = glm::min( -aabb[ 1 ].z * rd_min, -aabb[ 1 ].z * rd_max );
    t_max = glm::max( -aabb[ 0 ].z * rd_min, -aabb[ 0 ].z * rd_max );
}

void project_aabb_cubemap_positive_z( const glm::vec3 aabb[ 2 ], f32& s_min, f32& s_max, f32& t_min, f32& t_max ) {
    f32 rd_min = 1.f / glm::max( FLT_EPSILON, aabb[ 0 ].z );
    f32 rd_max = 1.f / glm::max( FLT_EPSILON, aabb[ 1 ].z );

    s_min = glm::min( -aabb[ 1 ].x * rd_min, -aabb[ 1 ].x * rd_max );
    s_max = glm::max( -aabb[ 0 ].x * rd_min, -aabb[ 0 ].x * rd_max );

    t_min = glm::min( -aabb[ 1 ].y * rd_min, -aabb[ 1 ].y * rd_max );
    t_max = glm::max( -aabb[ 0 ].y * rd_min, -aabb[ 0 ].y * rd_max );
}

void project_aabb_cubemap_negative_z( const glm::vec3 aabb[ 2 ], f32& s_min, f32& s_max, f32& t_min, f32& t_max ) {
    f32 rd_min = 1.f / glm::max( FLT_EPSILON, -aabb[ 0 ].z );
    f32 rd_max = 1.f / glm::max( FLT_EPSILON, -aabb[ 1 ].z );

    s_min = glm::min( aabb[ 0 ].x * rd_min, aabb[ 0 ].x * rd_max );
    s_max = glm::max( aabb[ 1 ].x * rd_min, aabb[ 1 ].x * rd_max );

    t_min = glm::min( -aabb[ 1 ].y * rd_min, -aabb[ 1 ].y * rd_max );
    t_max = glm::max( -aabb[ 0 ].y * rd_min, -aabb[ 0 ].y * rd_max );
}

// Numerical sequences ////////////////////////////////////////////////////
f32 halton( i32 i, i32 b ) {
    // Creates a halton sequence of values between 0 and 1.
    // https://en.wikipedia.org/wiki/Halton_sequence
    // Used for jittering based on a constant set of 2D points.
    f32 f = 1.0f;
    f32 r = 0.0f;
    while ( i > 0 ) {
        f = f / f32( b );
        r = r + f * f32( i % b );
        i = i / b;
    }
    return r;
}

// https://blog.demofox.org/2017/10/31/animating-noise-for-integration-over-time/
f32 interleaved_gradient_noise( glm::vec2 pixel, i32 index ) {
    pixel = pixel + f32( index ) * 5.588238f;
    const f32 noise = fmodf( 52.9829189f * fmodf( 0.06711056f * pixel.x + 0.00583715f * pixel.y, 1.0f ), 1.0f );
    return noise;
}

glm::vec2 halton23_sequence( i32 index ) {
    return glm::vec2{ halton( index, 2 ), halton( index, 3 ) };
}

// http://extremelearning.com.au/unreasonable-effectiveness-of-quasirandom-sequences/
glm::vec2 m_robert_r2_sequence( i32 index ) {
    const f32 g = 1.32471795724474602596f;
    const f32 a1 = 1.0f / g;
    const f32 a2 = 1.0f / ( g * g );

    const f32 x = fmod( 0.5f + a1 * index, 1.0f );
    const f32 y = fmod( 0.5f + a2 * index, 1.0f );
    return glm::vec2{ x, y };
}

glm::vec2 interleaved_gradient_sequence( i32 index ) {
    return glm::vec2{ interleaved_gradient_noise( {1.f, 1.f}, index ), interleaved_gradient_noise( {1.f, 2.f}, index ) };
}

// Computes a radical inverse with base 2 using crazy bit-twiddling from "Hacker's Delight"
inline f32 radical_inverse_base2( u32 bits ) {
    bits = ( bits << 16u ) | ( bits >> 16u );
    bits = ( ( bits & 0x55555555u ) << 1u ) | ( ( bits & 0xAAAAAAAAu ) >> 1u );
    bits = ( ( bits & 0x33333333u ) << 2u ) | ( ( bits & 0xCCCCCCCCu ) >> 2u );
    bits = ( ( bits & 0x0F0F0F0Fu ) << 4u ) | ( ( bits & 0xF0F0F0F0u ) >> 4u );
    bits = ( ( bits & 0x00FF00FFu ) << 8u ) | ( ( bits & 0xFF00FF00u ) >> 8u );
    return f32( bits ) * 2.3283064365386963e-10f; // / 0x100000000
}

// Returns a single 2D point in a Hammersley sequence of length "numSamples", using base 1 and base 2
glm::vec2 hammersley_sequence( i32 index, i32 num_samples ) {
    return glm::vec2{ index * 1.f / num_samples, radical_inverse_base2( u32( index ) ) };
}

VkTransformMatrixKHR to_vk_transform_matrix( const glm::mat4& m ) {

    // GLM is column-major, but VkTransformMatrixKHR is row-major, so we need to transpose the matrix.
    VkTransformMatrixKHR transform{};

    transform.matrix[ 0 ][ 0 ] = m[ 0 ][ 0 ];
    transform.matrix[ 0 ][ 1 ] = m[ 1 ][ 0 ];
    transform.matrix[ 0 ][ 2 ] = m[ 2 ][ 0 ];
    transform.matrix[ 0 ][ 3 ] = m[ 3 ][ 0 ];

    transform.matrix[ 1 ][ 0 ] = m[ 0 ][ 1 ];
    transform.matrix[ 1 ][ 1 ] = m[ 1 ][ 1 ];
    transform.matrix[ 1 ][ 2 ] = m[ 2 ][ 1 ];
    transform.matrix[ 1 ][ 3 ] = m[ 3 ][ 1 ];

    transform.matrix[ 2 ][ 0 ] = m[ 0 ][ 2 ];
    transform.matrix[ 2 ][ 1 ] = m[ 1 ][ 2 ];
    transform.matrix[ 2 ][ 2 ] = m[ 2 ][ 2 ];
    transform.matrix[ 2 ][ 3 ] = m[ 3 ][ 2 ];

    return transform;
}

// MeshesRenderFeature ///////////////////////////////////////////////////

void MeshesRenderingFeature::create_geometry_resources( Renderer* renderer, RenderBlackboard* render_blackboard, RenderScene* scene ) {
    MeshRuntimeData& geometry_data = render_blackboard->geometry_data;
    MeshBuffers& mesh_buffers = scene->mesh_buffers;
    GpuDevice* gpu = renderer->gpu;

    VkBufferUsageFlags flags = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if ( gpu->ray_tracing_present ) {
        flags |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    }

    // Geometry buffers ////////////////////////////////////////////////////////////

    if ( mesh_buffers.vertices.size > 0 ) {
        const VkDeviceSize buffer_size = mesh_buffers.vertices.size * sizeof( glm::vec3 );

        geometry_data.position_buffer_gpu = renderer->create_buffer_with_upload( {
            .size = buffer_size,
            .usage = flags | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .name = "vertex_buffer"
        }, {
            .data = mesh_buffers.vertices.data,
            .policy = BufferUploadPolicy::Transfer
        } );
    }

    if ( mesh_buffers.normals.size > 0 ) {
        const VkDeviceSize buffer_size = mesh_buffers.normals.size * sizeof( glm::vec3 );

        geometry_data.normal_buffer_gpu = renderer->create_buffer_with_upload( {
            .size = buffer_size,
            .usage = flags | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .name = "normal_buffer"
        }, {
            .data = mesh_buffers.normals.data,
            .policy = BufferUploadPolicy::Transfer
        } );
    }

    if ( mesh_buffers.tangents.size > 0 ) {
        const VkDeviceSize buffer_size = mesh_buffers.tangents.size * sizeof( glm::vec4 );

        geometry_data.tangent_buffer_gpu = renderer->create_buffer_with_upload( {
            .size = buffer_size,
            .usage = flags | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .name = "tangent_buffer"
        }, {
            .data = mesh_buffers.tangents.data,
            .policy = BufferUploadPolicy::Transfer
        } );
    }

    if ( mesh_buffers.tex_coords.size > 0 ) {
        const VkDeviceSize buffer_size = mesh_buffers.tex_coords.size * sizeof( glm::vec2 );

        geometry_data.texcoord_buffer_gpu = renderer->create_buffer_with_upload( {
            .size = buffer_size,
            .usage = flags | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .name = "texcoord_buffer"
        }, {
            .data = mesh_buffers.tex_coords.data,
            .policy = BufferUploadPolicy::Transfer
        } );
    }

    if ( mesh_buffers.joints.size > 0 ) {
        const VkDeviceSize buffer_size = mesh_buffers.joints.size * sizeof( glm::ivec4 );

        geometry_data.joints_buffer_gpu = renderer->create_buffer_with_upload( {
            .size = buffer_size,
            .usage = flags | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .name = "joints_buffer"
        }, {
            .data = mesh_buffers.joints.data,
            .policy = BufferUploadPolicy::Transfer
        } );
    }

    if ( mesh_buffers.weights.size > 0 ) {
        const VkDeviceSize buffer_size = mesh_buffers.weights.size * sizeof( glm::vec4 );

        geometry_data.weights_buffer_gpu = renderer->create_buffer_with_upload( {
            .size = buffer_size,
            .usage = flags | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .name = "weights_buffer"
        }, {
            .data = mesh_buffers.weights.data,
            .policy = BufferUploadPolicy::Transfer
        } );
    }

    if ( mesh_buffers.indices.size > 0 ) {
        const VkDeviceSize buffer_size = mesh_buffers.indices.size * sizeof( u32 );

        geometry_data.index_buffer_gpu = renderer->create_buffer_with_upload( {
            .size = buffer_size,
            .usage = flags | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .name = "index_buffer"
        }, {
            .data = mesh_buffers.indices.data,
            .policy = BufferUploadPolicy::Transfer
        } );
    }

    for ( Mesh& mesh : scene->meshes ) {
        mesh.position_buffer = geometry_data.position_buffer_gpu;
        mesh.tangent_buffer = geometry_data.tangent_buffer_gpu;
        mesh.normal_buffer = geometry_data.normal_buffer_gpu;
        mesh.texcoord_buffer = geometry_data.texcoord_buffer_gpu;
        mesh.joints_buffer = geometry_data.joints_buffer_gpu;
        mesh.weights_buffer = geometry_data.weights_buffer_gpu;
        mesh.index_buffer = geometry_data.index_buffer_gpu;
    }
}

void MeshesRenderingFeature::create_gpu_resources( Renderer* renderer, RenderBlackboard* render_blackboard, RenderScene* scene ) {
    
}

void MeshesRenderingFeature::destroy_gpu_resources( Renderer* renderer, RenderBlackboard* render_blackboard ) {

    GpuDevice* gpu = renderer->gpu;
    
    MeshRuntimeData& geometry_data = render_blackboard->geometry_data;

    gpu->destroy_buffer( geometry_data.position_buffer_gpu );
    gpu->destroy_buffer( geometry_data.normal_buffer_gpu );
    gpu->destroy_buffer( geometry_data.tangent_buffer_gpu );
    gpu->destroy_buffer( geometry_data.texcoord_buffer_gpu );
    gpu->destroy_buffer( geometry_data.joints_buffer_gpu );
    gpu->destroy_buffer( geometry_data.weights_buffer_gpu );
    gpu->destroy_buffer( geometry_data.index_buffer_gpu );
}

void MeshesRenderingFeature::on_resize( Renderer* renderer, RenderBlackboard* render_blackboard, u32 new_width, u32 new_height ) {

}

void MeshesRenderingFeature::upload_gpu_data( UploadGpuDataContext& context ) {

}


// GpuCullingRenderingFeature ////////////////////////////////////////////

void GpuCullingRenderingFeature::create_gpu_resources( Renderer* renderer, RenderBlackboard* render_blackboard, RenderScene* render_scene ) {

    RenderScene* scene = render_scene;
    GpuDevice* gpu = renderer->gpu;
    GpuCullingRuntimeData& culling = render_blackboard->gpu_culling;

    const VkDeviceSize draw_commands_size = VkDeviceSize( scene->mesh_instances.size ) * sizeof( GpuMeshDrawCommand ) * 2;
    const VkDeviceSize culled_instance_ids_size = VkDeviceSize( scene->mesh_instances.size ) * sizeof( u32 ) * 2;

    const VkBufferUsageFlags gpu_generated_indirect_usage =
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    for ( u32 i = 0; i < k_max_frames; ++i ) {
        // Contains both opaque and transparent commands.
        culling.meshlet_indirect_early_commands_sb[ i ] = gpu->create_buffer( {
            .size = draw_commands_size,
            .usage = gpu_generated_indirect_usage,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .name = "early_draw_commands_sb" } );

        culling.meshlet_indirect_late_commands_sb[ i ] = gpu->create_buffer( {
            .size = draw_commands_size,
            .usage = gpu_generated_indirect_usage,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .name = "late_draw_commands_sb" } );

        culling.meshlet_indirect_early_count_sb[ i ] = gpu->create_buffer( {
            .size = sizeof( GpuMeshDrawCounts ),
            .usage = gpu_generated_indirect_usage,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .name = "early_mesh_count_sb" } );

        culling.meshlet_indirect_late_count_sb[ i ] = gpu->create_buffer( {
            .size = sizeof( GpuMeshDrawCounts ),
            .usage = gpu_generated_indirect_usage,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .name = "late_mesh_count_sb" } );

        culling.meshlet_culled_mesh_instance_ids_sb[ i ] = gpu->create_buffer( {
            .size = culled_instance_ids_size,
            .usage = gpu_generated_indirect_usage,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .name = "meshlet_culled_mesh_instance_ids_sb" } );

        culling.meshlet_culling_handoff_sb[ i ] = gpu->create_buffer( {
            .size = sizeof( u32 ) * 4,
            .usage = gpu_generated_indirect_usage,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .name = "meshlet_culling_handoff_sb" } );
    }
}

void GpuCullingRenderingFeature::destroy_gpu_resources( Renderer* renderer, RenderBlackboard* render_blackboard ) {

    GpuDevice* gpu = renderer->gpu;
    GpuCullingRuntimeData& culling = render_blackboard->gpu_culling;

    for ( u32 i = 0; i < k_max_frames; ++i ) {

        gpu->destroy_buffer( culling.meshlet_indirect_early_commands_sb[ i ] );
        gpu->destroy_buffer( culling.meshlet_culled_mesh_instance_ids_sb[ i ] );
        gpu->destroy_buffer( culling.meshlet_indirect_early_count_sb[ i ] );
        gpu->destroy_buffer( culling.meshlet_indirect_late_commands_sb[ i ] );
        gpu->destroy_buffer( culling.meshlet_indirect_late_count_sb[ i ] );
        gpu->destroy_buffer( culling.meshlet_culling_handoff_sb[ i ] );
    }
}

void GpuCullingRenderingFeature::on_resize( Renderer* renderer, RenderBlackboard* render_blackboard, u32 new_width, u32 new_height ) {

}

void GpuCullingRenderingFeature::upload_gpu_data( UploadGpuDataContext& context ) {

}

// MeshletsRenderingFeature //////////////////////////////////////////////

void MeshletsRenderingFeature::create_geometry_resources( Renderer* renderer, RenderBlackboard* render_blackboard, RenderScene* scene ) {
    GpuDevice* gpu = renderer->gpu;
    
    MeshletsRuntimeData& meshlets = render_blackboard->meshlets;

    if ( scene->meshlets_connectivity_data.size > 0 ) {
        const VkDeviceSize buffer_size = sizeof( u32 ) * scene->meshlets_connectivity_data.size;

        meshlets.meshlets_connectivity_data_sb_gpu = renderer->create_buffer_with_upload( {
            .size = buffer_size,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .name = "meshlets_connectivity_data_sb"
        }, {
            .data = scene->meshlets_connectivity_data.data,
            .policy = BufferUploadPolicy::Transfer
        } );
    }

    if ( scene->meshlets_position_only_data.size > 0 ) {
        const VkDeviceSize buffer_size = sizeof( u32 ) * scene->meshlets_position_only_data.size;

        meshlets.meshlets_position_only_data_sb_gpu = renderer->create_buffer_with_upload( {
            .size = buffer_size,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .name = "meshlets_position_only_data_sb"
        }, {
            .data = scene->meshlets_position_only_data.data,
            .policy = BufferUploadPolicy::Transfer
        } );
    }


    // Meshlet emulation ///////////////////////////////////////////////////////////

    for ( u32 i = 0; i < k_max_frames; ++i ) {

        if ( scene->meshlets_vertex_data.size > 0 ) {
            const VkDeviceSize buffer_size = sizeof( GpuMeshletVertexData ) * scene->meshlets_vertex_data.size;

            meshlets.meshlets_vertex_data_sb_gpu[ i ] = renderer->create_buffer_with_upload( {
                .size = buffer_size,
                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                .name = "meshlet_vertex_data_sb"
            }, {
                .data = scene->meshlets_vertex_data.data,
                .policy = BufferUploadPolicy::Transfer
            } );
        }

        if ( scene->meshlets.size > 0 ) {
            const VkDeviceSize buffer_size = sizeof( GpuMeshlet ) * scene->meshlets.size;

            meshlets.meshlets_sb_gpu[ i ] = renderer->create_buffer_with_upload( {
                .size = buffer_size,
                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                .name = "meshlets_sb"
            }, {
                .data = scene->meshlets.data,
                .policy = BufferUploadPolicy::Transfer
            } );
        }

        // GPU generated meshlet index buffer.
        meshlets.meshlets_emulation_index_buffer_sb[ i ] = gpu->create_buffer( {
            .size = VkDeviceSize( scene->meshlets_index_count ) * sizeof( u32 ) * 8,
            .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .name = "meshlets_emulation_index_buffer"
        } );

        // GPU generated meshlet instance buffers.
        meshlets.meshlets_emulation_instances_sb[ i ] = gpu->create_buffer( {
            .size = VkDeviceSize( scene->meshlets.size * 2 ) * sizeof( u32 ) * 2,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .name = "meshlets_emulation_instances_buffer"
        } );

        meshlets.meshlets_emulation_visible_instances_sb[ i ] = gpu->create_buffer( {
            .size = VkDeviceSize( scene->meshlets.size * 2 ) * sizeof( u32 ) * 2,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .name = "meshlets_emulation_visible_instances_buffer"
        } );

        // TODO: transforms use absolute indices, thus we need all nodes.
        //
        // Keep this on the legacy path for now. ResourceUsageType::Dynamic has
        // special Raptor semantics and is backed by the global dynamic buffer.
        // It should be converted when the V2 dynamic-buffer API is introduced.
        for ( Skin& skin : scene->skins ) {
            const VkDeviceSize joint_buffer_size = skin.inverse_bind_matrices.size * sizeof( glm::mat4 );

            skin.joint_transforms[ i ] = gpu->create_buffer( {
                .size = joint_buffer_size,
                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                    VMA_ALLOCATION_CREATE_MAPPED_BIT,
                .name = "Skin ssbo"
                                                              } );

            Buffer* buffer = gpu->get_buffer( skin.joint_transforms[ i ] );
            memcpy( buffer->mapped_data, skin.inverse_bind_matrices.data, joint_buffer_size );
            gpu->flush_buffer( skin.joint_transforms[ i ], 0, joint_buffer_size );
        }

        if ( scene->meshlets_vertex_positions.size > 0 ) {
            const VkDeviceSize buffer_size = sizeof( GpuMeshletVertexPosition ) * scene->meshlets_vertex_positions.size;

            meshlets.meshlets_vertex_pos_sb_gpu[ i ] = renderer->create_buffer_with_upload( {
                .size = buffer_size,
                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                .name = "meshlet_vertex_pos_sb"
            }, {
                .data = scene->meshlets_vertex_positions.data,
                .policy = BufferUploadPolicy::Transfer
            } );
        }
    }
}
void MeshletsRenderingFeature::create_gpu_resources( Renderer* renderer, RenderBlackboard* render_blackboard, RenderScene* scene ) {
    MeshRuntimeData& geometry_data = render_blackboard->geometry_data;
    MeshBuffers& mesh_buffers = scene->mesh_buffers;
    GpuDevice* gpu = renderer->gpu;
    MeshletsRuntimeData& meshlets = render_blackboard->meshlets;

    DescriptorSetBinder descriptors;

    GpuCullingRuntimeData& culling = render_blackboard->gpu_culling;
    PipelineHandle meshlet_cull_pipeline = renderer->resource_cache.pipelines.get( hash_calculate( "gbuffer_culling" ) );
    ShaderReflectionInfo* shader_reflection = renderer->get_shader_reflection( meshlet_cull_pipeline );

    for ( u32 i = 0; i < k_max_frames; ++i ) {

        descriptors.reset();
        descriptors.ssbos.push( { culling.meshlet_indirect_early_commands_sb[ i ], 6 } );
        descriptors.ssbos.push( { culling.meshlet_indirect_early_count_sb[ i ], 7 } );
        descriptors.name = "gbuffer_meshlet_early_ds";

        meshlets.meshlets_early_draw_descriptor_set[ i ] = renderer->create_descriptor_set( descriptors, shader_reflection, meshlet_cull_pipeline, i, *render_blackboard );

        descriptors.reset();

        descriptors.ssbos.push( { culling.meshlet_indirect_late_commands_sb[ i ], 6 } );
        descriptors.ssbos.push( { culling.meshlet_indirect_late_count_sb[ i ], 7 } );
        descriptors.name = "gbuffer_meshlet_late_ds";

        meshlets.meshlets_late_draw_descriptor_set[ i ] = renderer->create_descriptor_set( descriptors, shader_reflection, meshlet_cull_pipeline, i, *render_blackboard );
    }

    meshlet_cull_pipeline = renderer->resource_cache.pipelines.get( hash_calculate( "gbuffer_culling_slang" ) );
    shader_reflection = renderer->get_shader_reflection( meshlet_cull_pipeline );

    for ( u32 i = 0; i < k_max_frames; ++i ) {

        descriptors.reset();
        descriptors.ssbos.push( { culling.meshlet_indirect_early_commands_sb[ i ], 6 } );
        //descriptors.ssbos.push( { meshlet_indirect_early_count_sb[ i ], 7 } );
        descriptors.name = "gbuffer_meshlet_early_slang_ds";

        meshlets.meshlets_early_draw_descriptor_set_slang[ i ] = renderer->create_descriptor_set( descriptors, shader_reflection, meshlet_cull_pipeline, i, *render_blackboard );

        descriptors.reset();

        descriptors.ssbos.push( { culling.meshlet_indirect_late_commands_sb[ i ], 6 } );
        //descriptors.ssbos.push( { meshlet_indirect_late_count_sb[ i ], 7 } );
        descriptors.name = "gbuffer_meshlet_late_slang_ds";

        meshlets.meshlets_late_draw_descriptor_set_slang[ i ] = renderer->create_descriptor_set( descriptors, shader_reflection, meshlet_cull_pipeline, i, *render_blackboard );
    }

    // TODO:
    //u32 meshlet_emulation_index = meshlet_technique->get_pass_index( "emulation_gbuffer_culling" );
    //GpuTechniquePass& meshlet_emulation_pass = meshlet_technique->passes[ meshlet_emulation_index ];
    //DescriptorSetLayoutHandle meshlet_emulation_layout = renderer->gpu->get_descriptor_set_layout( meshlet_emulation_pass.pipeline, k_material_descriptor_set_index );

    //for ( u32 i = 0; i < k_max_frames; ++i ) {

    //    descriptors.reset();

    //    descriptors.ssbos.push( { render_blackboard.meshlet_indirect_early_commands_sb[ i ], 6 } );
    //    descriptors.ssbos.push( { render_blackboard.meshlet_indirect_early_count_sb[ i ], 7 } );
    //    descriptors.ssbos.push( { render_blackboard.meshlets_emulation_instances_sb[ i ], (u16)meshlet_emulation_pass.get_binding_index( "MeshletInstances" ) } );
    //    //ds_creation.buffer( meshlet_indirect_early_commands_sb[ i ], 6 ).buffer( meshlet_indirect_early_count_sb[ i ], 7 )
    //    //.buffer( meshlets_instances_sb[ i ], meshlet_emulation_pass.get_binding_index( "MeshletInstances" ) ).set_layout( meshlet_emulation_layout );

    //    render_blackboard.meshlets_emulation_draw_descriptor_set[ i ] = create_descriptor_set( descriptors, meshlet_emulation_pass, meshlet_emulation_layout, i );
    //}

    PipelineHandle meshlet_transparent_pipeline = renderer->resource_cache.pipelines.get( hash_calculate( "transparent_no_cull" ) );
    shader_reflection = renderer->get_shader_reflection( meshlet_transparent_pipeline );

    for ( u32 i = 0; i < k_max_frames; ++i ) {

        renderer->gpu->destroy_descriptor_set( meshlets.meshlets_transparent_draw_descriptor_set[ i ] );

        descriptors.reset();

        descriptors.ssbos.push( { culling.meshlet_indirect_early_commands_sb[ i ], 6 } );
        descriptors.ssbos.push( { culling.meshlet_indirect_early_count_sb[ i ], 7 } );
        descriptors.name = "gbuffer_meshlet_transparent_ds";

        meshlets.meshlets_transparent_draw_descriptor_set[ i ] = renderer->create_descriptor_set( descriptors, shader_reflection, meshlet_transparent_pipeline, i, *render_blackboard );
        //render_blackboard.meshlets_transparent_draw_descriptor_set[ i ] = create_descriptor_set( descriptors, transparent_pass, transparent_layout, i );
    }

    if ( scene->skins.size > 0 ) {
        PipelineHandle skinning_pipeline = renderer->resource_cache.pipelines.get( hash_calculate( "gbuffer_skinning" ) );
        shader_reflection = renderer->get_shader_reflection( skinning_pipeline );

        for ( u32 i = 0; i < k_max_frames; ++i ) {
            meshlets.skinning_descriptor_set[ i ].init( renderer->resident_allocator, scene->skins.size, scene->skins.size );

            for ( u32 s = 0; s < scene->skins.size; ++s ) {
                renderer->gpu->destroy_descriptor_set( meshlets.skinning_descriptor_set[ i ][ s ] );

                descriptors.reset();

                descriptors.ssbos.push( { scene->skins[ s ].joint_transforms[ i ], 3 } );
                descriptors.name = "gbuffer_skinning_ds";

                meshlets.skinning_descriptor_set[ i ][ s ] = renderer->create_descriptor_set( descriptors, shader_reflection, skinning_pipeline, i, *render_blackboard );
            }
        }
    }
}

void MeshletsRenderingFeature::destroy_gpu_resources( Renderer* renderer, RenderBlackboard* render_blackboard ) {

    GpuDevice* gpu = renderer->gpu;
    MeshletsRuntimeData& meshlets = render_blackboard->meshlets;


    gpu->destroy_buffer( meshlets.meshlets_connectivity_data_sb_gpu );
    gpu->destroy_buffer( meshlets.meshlets_position_only_data_sb_gpu );


    for ( u32 i = 0; i < k_max_frames; ++i ) {
        gpu->destroy_buffer( meshlets.meshlets_sb_gpu[ i ] );
        gpu->destroy_buffer( meshlets.meshlets_vertex_pos_sb_gpu[ i ] );
        gpu->destroy_buffer( meshlets.meshlets_vertex_data_sb_gpu[ i ] );

        gpu->destroy_buffer( meshlets.meshlets_emulation_index_buffer_sb[ i ] );
        gpu->destroy_buffer( meshlets.meshlets_emulation_instances_sb[ i ] );
        gpu->destroy_buffer( meshlets.meshlets_emulation_visible_instances_sb[ i ] );

        gpu->destroy_descriptor_set( meshlets.meshlets_early_draw_descriptor_set[ i ] );
        gpu->destroy_descriptor_set( meshlets.meshlets_early_draw_descriptor_set_slang[ i ] );
        gpu->destroy_descriptor_set( meshlets.meshlets_late_draw_descriptor_set[ i ] );
        gpu->destroy_descriptor_set( meshlets.meshlets_late_draw_descriptor_set_slang[ i ] );
        gpu->destroy_descriptor_set( meshlets.meshlets_transparent_draw_descriptor_set[ i ] );
        gpu->destroy_descriptor_set( meshlets.meshlets_emulation_draw_descriptor_set[ i ] );

        for ( u32 s = 0; s < meshlets.skinning_descriptor_set[ i ].size; ++s ) {
            renderer->gpu->destroy_descriptor_set( meshlets.skinning_descriptor_set[ i ][ s ] );
        }
        meshlets.skinning_descriptor_set[ i ].shutdown();
    }
}

void MeshletsRenderingFeature::on_resize( Renderer* renderer, RenderBlackboard* render_blackboard, u32 new_width, u32 new_height ) {
    PipelineHandle meshlet_transparent_pipeline = renderer->resource_cache.pipelines.get( hash_calculate( "transparent_no_cull" ) );
    ShaderReflectionInfo* shader_reflection = renderer->get_shader_reflection( meshlet_transparent_pipeline );
    DescriptorSetBinder descriptors;

    for ( u32 i = 0; i < k_max_frames; ++i ) {

        renderer->gpu->destroy_descriptor_set( render_blackboard->meshlets.meshlets_transparent_draw_descriptor_set[ i ] );

        descriptors.reset();

        descriptors.ssbos.push( { render_blackboard->gpu_culling.meshlet_indirect_early_commands_sb[ i ], 6 } );
        descriptors.ssbos.push( { render_blackboard->gpu_culling.meshlet_indirect_early_count_sb[ i ], 7 } );

        render_blackboard->meshlets.meshlets_transparent_draw_descriptor_set[ i ] = renderer->create_descriptor_set( descriptors, shader_reflection, meshlet_transparent_pipeline, i, *render_blackboard );
        //render_blackboard.meshlets_transparent_draw_descriptor_set[ i ] = create_descriptor_set( descriptors, transparent_pass, transparent_layout, i );
    }
}

void MeshletsRenderingFeature::upload_gpu_data( UploadGpuDataContext& context ) {

}

// LightingRenderFeature /////////////////////////////////////////////////

struct SortedLight {

    u32             light_index;
    f32             projected_z;
    f32             projected_z_min;
    f32             projected_z_max;
}; // struct SortedLight

static int sorting_light_fn( const void* a, const void* b ) {
    const SortedLight* la = (const SortedLight*)a;
    const SortedLight* lb = (const SortedLight*)b;

    if ( la->projected_z < lb->projected_z ) return -1;
    else if ( la->projected_z > lb->projected_z ) return 1;
    return 0;
}

struct ClusteringLightingTileInfo {
    u32         tile_x_count = 0;
    u32         tile_y_count = 0;
    u32         tiles_entry_count = 0;
    u32         buffer_size = 0;
}; // struct ClusteringLightingTileInfo

ClusteringLightingTileInfo calculate_light_tile_buffer_size( u32 width, u32 height ) {

    ClusteringLightingTileInfo info;
    info.tile_x_count = ceilu32( width * 1.f / LightingRenderingFeature::k_tile_size );
    info.tile_y_count = ceilu32( height * 1.f / LightingRenderingFeature::k_tile_size );
    info.tiles_entry_count = info.tile_x_count * info.tile_y_count * LightingRenderingFeature::k_light_mask_u32_count;
    info.buffer_size = info.tiles_entry_count * sizeof( u32 );

    return info;
}

void LightingRenderingFeature::upload_gpu_data( UploadGpuDataContext& context ) {

    Renderer* renderer = context.renderer;
    RenderBlackboard& render_blackboard = context.render_blackboard;
    RenderConfig& render_config = context.render_config;
    FrameGraph* frame_graph = context.frame_graph;
    GpuDevice& gpu = *renderer->gpu;
    GameCamera& game_camera = context.game_camera;

    // Lighting Constant Buffer
    GpuLightingData* gpu_lighting_data = gpu.dynamic_buffer_allocate<GpuLightingData>( &render_blackboard.lighting.lighting_constants_cb_offset );
    if ( gpu_lighting_data ) {

        gpu_lighting_data->cubemap_shadows_index = render_blackboard.point_shadows.cubemap_shadows_index;
        gpu_lighting_data->debug_show_light_tiles = render_config.lighting.debug_show_light_tiles ? 1 : 0;
        gpu_lighting_data->debug_show_tiles = render_config.lighting.debug_show_tiles ? 1 : 0;
        gpu_lighting_data->debug_show_bins = render_config.lighting.debug_show_bins ? 1 : 0;
        gpu_lighting_data->disable_shadows = render_config.shadows.disable_shadows ? 1 : 0;
        gpu_lighting_data->debug_modes = (u32)render_config.lighting.lighting_debug_modes;
        gpu_lighting_data->debug_texture_index = render_blackboard.lighting_debug_texture_index;
        gpu_lighting_data->gi_intensity = render_config.gi_intensity;
        gpu_lighting_data->brdf_lut_texture_index = render_blackboard.brdf_lut_image_view.is_valid() ? render_blackboard.brdf_lut_image_view.index() : renderer->gpu->dummy_image_view.index();

        gpu_lighting_data->shadow_visibility_texture_index = renderer->gpu->dummy_image_view.index();
        gpu_lighting_data->indirect_lighting_texture_index = renderer->gpu->dummy_image_view.index();
        gpu_lighting_data->bilateral_weights_texture_index = renderer->gpu->dummy_image_view.index();
        gpu_lighting_data->reflections_texture_index = renderer->gpu->dummy_image_view.index();


        FrameGraphResource* resource = frame_graph->get_resource( "shadow_visibility" );
        if ( resource ) {
            gpu_lighting_data->shadow_visibility_texture_index = resource->resource_info.texture.image_view.index();
        }

        resource = (FrameGraphResource*)frame_graph->get_resource( "indirect_lighting" );
        if ( resource ) {
            gpu_lighting_data->indirect_lighting_texture_index = resource->resource_info.texture.image_view.index();
        }

        resource = (FrameGraphResource*)frame_graph->get_resource( "bilateral_weights" );
        if ( resource ) {
            gpu_lighting_data->bilateral_weights_texture_index = resource->resource_info.texture.image_view.index();
        }

        resource = (FrameGraphResource*)frame_graph->get_resource( "reflections_denoised_output" );
        if ( resource ) {
            gpu_lighting_data->reflections_texture_index = resource->resource_info.texture.image_view.index();
        }

        // Volumetric fog data
        // TODO: parametrize it
        gpu_lighting_data->volumetric_fog_texture_index = render_config.volumetric_fog.texture_index;
        gpu_lighting_data->volumetric_fog_num_slices = render_config.volumetric_fog.slices;
        gpu_lighting_data->volumetric_fog_near = game_camera.camera.near_plane;
        gpu_lighting_data->volumetric_fog_far = game_camera.camera.far_plane;
        // linear_depth_to_uv_optimize offloads this calculations here:
        const float one_over_log_f_over_n = 1.0f / log2( game_camera.camera.far_plane / game_camera.camera.near_plane );
        gpu_lighting_data->volumetric_fog_distribution_scale = render_config.volumetric_fog.slices * one_over_log_f_over_n;
        gpu_lighting_data->volumetric_fog_distribution_bias = -( render_config.volumetric_fog.slices * log2( game_camera.camera.near_plane ) * one_over_log_f_over_n );

        if ( render_config.raytraced_shadows.enabled ) {
            raptor::Color raytraced_light_color_type_packed;
            raytraced_light_color_type_packed.set( render_config.raytraced_shadows.light_color.x, render_config.raytraced_shadows.light_color.y, render_config.raytraced_shadows.light_color.z, ( f32 )render_config.raytraced_shadows.light_type );
            gpu_lighting_data->raytraced_shadow_light_color_type = raytraced_light_color_type_packed.abgr;
            gpu_lighting_data->raytraced_shadow_light_radius = render_config.raytraced_shadows.light_radius;
            gpu_lighting_data->raytraced_shadow_light_position = render_config.raytraced_shadows.light_type == 0 ? render_config.raytraced_shadows.light_direction : render_config.raytraced_shadows.light_position;
            gpu_lighting_data->raytraced_shadow_light_intensity = render_config.raytraced_shadows.light_intensity;
        }
        else {
            gpu_lighting_data->raytraced_shadow_light_color_type = 0;
            gpu_lighting_data->raytraced_shadow_light_radius = 0.f;
            gpu_lighting_data->raytraced_shadow_light_position = glm::vec3( 0.f );
            gpu_lighting_data->raytraced_shadow_light_intensity = 0.f;
        }
    }

    // TODO: refactor!
    // Lighting Clustering data calculation/upload
    ArenaAllocator* scratch_allocator = context.scratch_allocator;
    sizet current_marker = scratch_allocator->get_marker();

    RenderScene* scene = context.scene;
    GpuFrameData& scene_data = context.scene_data;

    Array<SortedLight> sorted_lights;
    sorted_lights.init( scratch_allocator, scene->active_lights, scene->active_lights );

    // Sort lights based on Z ////////////////////////////////////////////
    glm::mat4& world_to_camera = scene_data.world_to_camera;
    float z_far = scene_data.z_far;
    for ( u32 i = 0; i < scene->active_lights; ++i ) {
        Light& light = scene->lights[ i ];

        glm::vec4 p{ light.world_position.x, light.world_position.y, light.world_position.z, 1.0f };

        glm::vec4 view_p = world_to_camera * p;
        // > 0 in front of camera
        f32 center_d = -view_p.z;

        // Sphere range along view direction
        f32 d_min = center_d - light.radius;   // near side of sphere
        f32 d_max = center_d + light.radius;   // far side of sphere

        d_min = raptor::max( d_min, scene_data.z_near );

        SortedLight& sorted_light = sorted_lights[ i ];
        sorted_light.light_index = i;

        const float dz = z_far - scene_data.z_near;

        sorted_light.projected_z = ( center_d - scene_data.z_near ) / dz;
        sorted_light.projected_z_min = ( d_min - scene_data.z_near ) / dz;
        sorted_light.projected_z_max = ( d_max - scene_data.z_near ) / dz;

        //rprint( "Light Z %f, Zmin %f, Zmax %f\n", sorted_light.projected_z, sorted_light.projected_z_min, sorted_light.projected_z_max );
    }

    qsort( sorted_lights.data, scene->active_lights, sizeof( SortedLight ), sorting_light_fn );

    // Calculate lights LUT //////////////////////////////////////////////
    // NOTE(marco): it might be better to use logarithmic slices to have better resolution
    // closer to the camera. We could also use a different far plane and discard any lights
    // that are too far
    const f32 bin_size = 1.0f / k_light_z_bins;

    Array<u32> light_z_bins;
    light_z_bins.init( scratch_allocator, k_light_z_bins, k_light_z_bins );

    Array<u32> z_bin_range_per_light;
    z_bin_range_per_light.init( scratch_allocator, scene->active_lights, scene->active_lights );

    // Calculate Light Bin Ranges ////////////////////////////////////////
    for ( u32 i = 0; i < scene->active_lights; ++i ) {
        const SortedLight& light = sorted_lights[ i ];

        if ( light.projected_z_min < 0.0f && light.projected_z_max < 0.0f ) {
            // NOTE(marco): this light is behind the camera
            z_bin_range_per_light[ i ] = u32_max;

            continue;
        }

        u32 min_bin = raptor::max( 0, raptor::floori32( light.projected_z_min * k_light_z_bins ) );
        u32 max_bin = raptor::max( 0, raptor::ceili32( light.projected_z_max * k_light_z_bins ) );

        min_bin = raptor::clamp<u32>( min_bin, 0, k_light_z_bins - 1 );
        max_bin = raptor::clamp<u32>( max_bin, 0, k_light_z_bins - 1 );

        z_bin_range_per_light[ i ] = ( min_bin & 0xffff ) | ( ( max_bin & 0xffff ) << 16 );
        //rprint( "Light %u min %u, max %u, linear z min %f max %f\n", i, min_bin, max_bin, light.projected_z_min * z_far, light.projected_z_max * z_far );
    }

    // Calculate light z bins ////////////////////////////////////////////
    for ( u32 bin = 0; bin < k_light_z_bins; ++bin ) {
        u32 min_light_id = k_num_lights + 1;
        u32 max_light_id = 0;

        f32 bin_min = bin_size * bin;
        f32 bin_max = bin_min + bin_size;

        for ( u32 i = 0; i < scene->active_lights; ++i ) {
            const SortedLight& light = sorted_lights[ i ];
            const u32 light_bins = z_bin_range_per_light[ i ];

            if ( light_bins == u32_max ) {
                continue;
            }

            const u32 min_bin = light_bins & 0xffff;
            const u32 max_bin = light_bins >> 16;

            if ( bin >= min_bin && bin <= max_bin ) {
                if ( i < min_light_id ) {
                    min_light_id = i;
                }

                if ( i > max_light_id ) {
                    max_light_id = i;
                }
            }
            // OLD: left as a reference if new implementation breaks too much.
            //if ( ( light.projected_z >= bin_min && light.projected_z <= bin_max ) ||
            //     ( light.projected_z_min >= bin_min && light.projected_z_min <= bin_max ) ||
            //     ( light.projected_z_max >= bin_min && light.projected_z_max <= bin_max ) ) {
            //    if ( i < min_light_id ) {
            //        min_light_id = i;

            //        //rprint( "Light in bin %u\n", bin );
            //    }

            //    if ( i > max_light_id ) {
            //        max_light_id = i;
            //    }
            //}
        }

        //if (min_light_id != k_num_lights + 1)
            //rprint( "Bin %u, light ids min %u, max %u\n", bin, min_light_id, max_light_id );

        light_z_bins[ bin ] = min_light_id | ( max_light_id << 16 );
    }

    ClusteringLightingTileInfo tile_info = calculate_light_tile_buffer_size( (u32)scene_data.resolution_x, (u32)scene_data.resolution_y );

    // Light Clustering //////////////////////////////////////////////////
    Array<u32> light_tiles_bits;
    light_tiles_bits.init( scratch_allocator, tile_info.tiles_entry_count, tile_info.tiles_entry_count );
    memset( light_tiles_bits.data, 0, tile_info.buffer_size );

    float near_z = scene_data.z_near;
    float tile_size_inv = 1.0f / k_tile_size;

    u32 tile_stride = tile_info.tile_x_count * k_light_mask_u32_count;

    // Assing light to light tiles
    for ( u32 i = 0; i < scene->active_lights; ++i ) {
        const u32 light_index = sorted_lights[ i ].light_index;
        Light& light = scene->lights[ light_index ];

        glm::vec4 pos{ light.world_position.x, light.world_position.y, light.world_position.z, 1.0f };
        float radius = light.radius;

        glm::vec4 view_space_pos = game_camera.camera.view * pos;

        float center_d = -view_space_pos.z;
        float d_min = center_d - radius;
        float d_max = center_d + radius;

        bool camera_visible =
            ( d_max >= game_camera.camera.near_plane ) &&
            ( d_min <= game_camera.camera.far_plane );

        if ( !camera_visible && render_config.lighting.skip_invisible_lights ) {
            continue;
        }

        //rprint( "Camera vis %u view z %f\n", camera_visible ? 1 : 0, view_space_pos.z );
        glm::vec4 aabb_ndc = { 0, 0, 0, 0 };

        if ( render_config.lighting.use_mcguire_method ) {

            glm::vec3 C_vs{ view_space_pos.x, view_space_pos.y, view_space_pos.z };

            // Horizontal bounds
            glm::vec3 left_vs, right_vs;
            get_bounds_for_axis_rh( glm::vec3{ 1.0f, 0.0f, 0.0f }, C_vs, radius,
                                    game_camera.camera.near_plane,
                                    left_vs, right_vs );

            // Vertical bounds
            glm::vec3 top_vs, bottom_vs;
            get_bounds_for_axis_rh( glm::vec3{ 0.0f, 1.0f, 0.0f }, C_vs, radius,
                                    game_camera.camera.near_plane,
                                    top_vs, bottom_vs );

            // Project to clip space
            glm::vec4 left_clip = game_camera.camera.projection * glm::vec4( left_vs, 1.0f );
            glm::vec4 right_clip = game_camera.camera.projection * glm::vec4( right_vs, 1.0f );
            glm::vec4 top_clip = game_camera.camera.projection * glm::vec4( top_vs, 1.0f );
            glm::vec4 bottom_clip = game_camera.camera.projection * glm::vec4( bottom_vs, 1.0f );

            glm::vec4 left_ndc = left_clip / left_clip.w;
            glm::vec4 right_ndc = right_clip / right_clip.w;
            glm::vec4 top_ndc = top_clip / top_clip.w;
            glm::vec4 bottom_ndc = bottom_clip / bottom_clip.w;

            // NDC rect (xy = min, zw = max)
            aabb_ndc.x = glm::min( left_ndc.x, right_ndc.x );   // min X
            aabb_ndc.z = glm::max( left_ndc.x, right_ndc.x );   // max X
            aabb_ndc.y = -1.f * glm::max( top_ndc.y, bottom_ndc.y );  // min Y
            aabb_ndc.w = -1.f * glm::min( top_ndc.y, bottom_ndc.y );  // max Y

        } else {

            // Use the 8 corners of the bounding box to find the screen-space AABB
            glm::vec2 ndc_min{ 1.0f,  1.0f };
            glm::vec2 ndc_max{ -1.0f, -1.0f };

            // World-space cube corners
            for ( u32 c = 0; c < 8; ++c ) {
                glm::vec3 offset{
                    ( c & 1 ) ? radius : -radius,   // x
                    ( c & 2 ) ? radius : -radius,   // y
                    ( c & 4 ) ? radius : -radius    // z
                };

                glm::vec3 corner_ws = glm::vec3( pos.x, pos.y, pos.z ) + offset;
                glm::vec4 corner_ws4 = glm::vec4( corner_ws, 1.0f );

                // view space -> clip space
                glm::vec4 corner_vs4 = game_camera.camera.view * corner_ws4;
                glm::vec4 corner_clip4 = game_camera.camera.projection * corner_vs4;

                glm::vec4 corner_ndc4 = corner_clip4 / corner_clip4.w;

                // clamp in [-1, 1] to avoid issues with screen coordinates
                float x = glm::clamp( corner_ndc4.x, -1.0f, 1.0f );
                float y = glm::clamp( corner_ndc4.y, -1.0f, 1.0f );

                ndc_min.x = glm::min( ndc_min.x, x );
                ndc_min.y = glm::min( ndc_min.y, y );
                ndc_max.x = glm::max( ndc_max.x, x );
                ndc_max.y = glm::max( ndc_max.y, y );
            }

            aabb_ndc.x = ndc_min.x;
            aabb_ndc.z = ndc_max.x;

            // Inverted Y aabb
            aabb_ndc.y = -1.f * ndc_max.y;
            aabb_ndc.w = -1.f * ndc_min.y;
        }

        const f32 position_len = glm::length( glm::vec3( view_space_pos.x, view_space_pos.y, view_space_pos.z ) );
        const bool camera_inside = ( position_len - radius ) < game_camera.camera.near_plane;

        if ( camera_inside && render_config.lighting.enable_camera_inside ) {
            aabb_ndc = { -1,-1, 1, 1 };
        }

        if ( render_config.lighting.force_fullscreen_light_aabb ) {
            aabb_ndc = { -1,-1, 1, 1 };
        }

        //rprint( "aabb %f %f %f %f\n", aabb_ndc.x, aabb_ndc.y, aabb_ndc.z, aabb_ndc.w );

        // NOTE(marco): xy = top-left, zw = bottom-right
        glm::vec4 aabb_screen{ ( aabb_ndc.x * 0.5f + 0.5f ) * ( render_blackboard.render_width - 1 ),
                           ( aabb_ndc.y * 0.5f + 0.5f ) * ( render_blackboard.render_height - 1 ),
                           ( aabb_ndc.z * 0.5f + 0.5f ) * ( render_blackboard.render_width - 1 ),
                           ( aabb_ndc.w * 0.5f + 0.5f ) * ( render_blackboard.render_height - 1 ) };

        f32 width = aabb_screen.z - aabb_screen.x;
        f32 height = aabb_screen.w - aabb_screen.y;

        if ( width < 0.0001f || height < 0.0001f ) {
            continue;
        }

        float min_x = aabb_screen.x;
        float min_y = aabb_screen.y;

        float max_x = min_x + width;
        float max_y = min_y + height;

        if ( min_x > render_blackboard.render_width || min_y > render_blackboard.render_height ) {
            continue;
        }

        if ( max_x < 0.0f || max_y < 0.0f ) {
            continue;
        }

        min_x = max( min_x, 0.0f );
        min_y = max( min_y, 0.0f );

        max_x = min( max_x, (float)render_blackboard.render_width );
        max_y = min( max_y, (float)render_blackboard.render_height );

        u32 first_tile_x = (u32)( min_x * tile_size_inv );
        u32 last_tile_x = min( tile_info.tile_x_count - 1, (u32)( max_x * tile_size_inv ) );

        u32 first_tile_y = (u32)( min_y * tile_size_inv );
        u32 last_tile_y = min( tile_info.tile_y_count - 1, (u32)( max_y * tile_size_inv ) );

        for ( u32 y = first_tile_y; y <= last_tile_y; ++y ) {
            for ( u32 x = first_tile_x; x <= last_tile_x; ++x ) {
                u32 array_index = y * tile_stride + x;

                u32 u32_index = i / 32;
                u32 bit_index = i % 32;

                light_tiles_bits[ array_index + u32_index ] |= ( 1 << bit_index );
            }
        }
    }

    LightingRuntimeData& lighting = render_blackboard.lighting;
    MapBufferParameters cb_map{};

    // Upload light list /////////////////////////////////////////////////
    cb_map.buffer = lighting.lights_list_sb[ gpu.current_frame ];
    GpuLight* gpu_lights_data = (GpuLight*)gpu.map_buffer( cb_map );
    if ( gpu_lights_data ) {
        for ( u32 i = 0; i < scene->active_lights; ++i ) {
            Light& light = scene->lights[ i ];
            GpuLight& gpu_light = gpu_lights_data[ i ];

            gpu_light.world_position = light.world_position;
            gpu_light.radius = light.radius;
            gpu_light.color = light.color;
            gpu_light.intensity = light.intensity;
            gpu_light.shadow_map_resolution = light.shadow_map_resolution;
            // NOTE: calculation used to retrieve depth for cubemaps.
            // near = 0.01f as a static value, if you change here change also
            // method vector_to_depth_value in lighting.h in the shaders!
            gpu_light.rcp_n_minus_f = 1.0f / ( 0.01f - light.radius );
        }

        gpu.unmap_buffer( cb_map );
    }

    // Upload light indices //////////////////////////////////////////////
    cb_map.buffer = lighting.lights_indices_sb[ gpu.current_frame ];

    u32* gpu_light_indices = (u32*)gpu.map_buffer( cb_map );
    if ( gpu_light_indices ) {
        // TODO: improve
        //memcpy( gpu_light_indices, light_z_bins.data, light_z_bins.size * sizeof( u32 ) );
        for ( u32 i = 0; i < scene->active_lights; ++i ) {
            gpu_light_indices[ i ] = sorted_lights[ i ].light_index;
        }

        gpu.unmap_buffer( cb_map );
    }

    // Upload lights Z-Bins /////////////////////////////////////////////////
    cb_map.buffer = lighting.light_z_bins_sb[ gpu.current_frame ];
    u32* gpu_z_bins_data = (u32*)gpu.map_buffer( cb_map );
    if ( gpu_z_bins_data ) {
        memcpy( gpu_z_bins_data, light_z_bins.data, light_z_bins.size * sizeof( u32 ) );

        gpu.unmap_buffer( cb_map );
    }

    // Update light tiles ////////////////////////////////////////////////
    MapBufferParameters light_tiles_cb_map = { lighting.lights_tiles_sb[ gpu.current_frame ], 0, 0 };
    u32* light_tiles_data = (u32*)gpu.map_buffer( light_tiles_cb_map );
    if ( light_tiles_data ) {
        Buffer* buffer = gpu.get_buffer( lighting.lights_tiles_sb[ gpu.current_frame ] );

        RASSERT( buffer->size >= light_tiles_bits.size * sizeof( u32 ) );
        memcpy( light_tiles_data, light_tiles_bits.data, light_tiles_bits.size * sizeof( u32 ) );

        gpu.unmap_buffer( light_tiles_cb_map );
    }

    scratch_allocator->free_marker( current_marker );
}

// TODO - integrate
#if 0
static void lighting_calculate_shadow_resolution() {
    //// Clustered shadows: point-light resolution estimate ///////////////////////
    // Notes:
    // - Right-handed view space: camera looks down -Z.
    // - GLM_FORCE_DEPTH_ZERO_TO_ONE: clip z in [0, 1].
    // - Screen coordinates: origin top-left.

    const u32 z_count = 32;
    const f32 tile_size = 64.0f;
    const f32 tile_pixels = tile_size * tile_size;

    const u32 tile_x_count = raptor::ceilu32( frame_data.resolution_x / tile_size );
    const u32 tile_y_count = raptor::ceilu32( frame_data.resolution_y / tile_size );

    const f32 tile_radius = ( tile_size * 0.5f ) * glm::sqrt( 2.0f ); // half-diagonal
    const f32 tile_radius_sq = tile_radius * tile_radius;

    static Camera last_camera{};
    if ( !freeze_occlusion_camera ) {
        last_camera = game_camera.camera;
    }

    const glm::mat4 inv_proj = glm::inverse( last_camera.projection );
    const glm::mat4 inv_view = glm::inverse( last_camera.view );

    auto unproject_screen_to_view_far = [ & ]( const glm::vec2& screen_xy ) -> glm::vec3 {
        const f32 ndc_x = ( screen_xy.x / frame_data.resolution_x ) * 2.0f - 1.0f;
        const f32 ndc_y = ( 1.0f - ( screen_xy.y / frame_data.resolution_y ) ) * 2.0f - 1.0f;

        // For ZO: far plane is clip_z = 1.
        const glm::vec4 clip{ ndc_x, ndc_y, 1.0f, 1.0f };
        glm::vec4 view4 = inv_proj * clip;

        if ( view4.w != 0.0f ) {
            view4 *= ( 1.0f / view4.w );
        }

        return glm::vec3{ view4.x, view4.y, view4.z };
    };

    auto ray_dir_for_tile = [ & ]( const glm::vec2& screen_xy ) -> glm::vec3 {
        const glm::vec3 view_far = unproject_screen_to_view_far( screen_xy );
        const f32 len = glm::length( view_far );
        if ( len > 0.0f ) {
            return view_far / len;
        }
        return glm::vec3{ 0.0f, 0.0f, -1.0f };
    };

    auto intersect_ray_with_z_plane = [ & ]( const glm::vec3& ray_dir, f32 z_plane ) -> glm::vec3 {
        // Ray origin is (0,0,0) in view space.
        // Plane is z = z_plane.
        const f32 denom = ray_dir.z;
        if ( fabsf( denom ) < 1.0e-8f ) {
            // Parallel: return something degenerate; caller should handle.
            return glm::vec3{ 0.0f, 0.0f, z_plane };
        }

        const f32 t = z_plane / denom;
        return ray_dir * t;
    };

    auto sphere_intersects_aabb = []( const glm::vec3& c, f32 r, const glm::vec3& bmin, const glm::vec3& bmax ) -> bool {
        glm::vec3 q;
        q.x = glm::clamp( c.x, bmin.x, bmax.x );
        q.y = glm::clamp( c.y, bmin.y, bmax.y );
        q.z = glm::clamp( c.z, bmin.z, bmax.z );

        const glm::vec3 d = c - q;
        return glm::dot( d, d ) <= ( r * r );
    };

    const f32 z_near = frame_data.z_near;
    const f32 z_far = frame_data.z_far;
    const f32 z_ratio = z_far / z_near;
    const f32 z_bin_range = 1.0f / f32( z_count );

    const u32 light_count = scene->active_lights;

    // Precompute view-space centers for point lights (more stable than AABB-min/max transforms).
    Array<glm::vec4> light_center_radius_vs;
    light_center_radius_vs.init( allocator, light_count, light_count );

    for ( u32 l = 0; l < light_count; ++l ) {
        Light& light = scene->lights[ l ];

        light.shadow_map_resolution = 0.0f;
        light.tile_x = 0;
        light.tile_y = 0;
        light.solid_angle = 0.0f;

        const glm::vec4 pos_ws{ light.world_position.x, light.world_position.y, light.world_position.z, 1.0f };
        const glm::vec4 pos_vs4 = last_camera.view * pos_ws;

        light_center_radius_vs[ l ] = glm::vec4{ pos_vs4.x, pos_vs4.y, pos_vs4.z, light.radius };
    }

    for ( u32 z = 0; z < z_count; ++z ) {
        // Cluster slice distances (positive distances from camera).
        const f32 tile_near_d = z_near * powf( z_ratio, f32( z ) * z_bin_range );
        const f32 tile_far_d = z_near * powf( z_ratio, f32( z + 1 ) * z_bin_range );

        // Convert to RH view-space z planes (negative in front of camera).
        const f32 z_plane_near = -tile_near_d;
        const f32 z_plane_far = -tile_far_d;

        for ( u32 y = 0; y < tile_y_count; ++y ) {
            for ( u32 x = 0; x < tile_x_count; ++x ) {
                const f32 min_x = f32( x ) * tile_size;
                const f32 min_y = f32( y ) * tile_size;
                const f32 max_x = f32( x + 1 ) * tile_size;
                const f32 max_y = f32( y + 1 ) * tile_size;

                const glm::vec2 tile_center{ ( min_x + max_x ) * 0.5f, ( min_y + max_y ) * 0.5f };

                // Build two rays from tile corners (min/max).
                const glm::vec3 dir_min = ray_dir_for_tile( glm::vec2{ min_x, min_y } );
                const glm::vec3 dir_max = ray_dir_for_tile( glm::vec2{ max_x, max_y } );

                // Intersect each ray with near/far z planes.
                const glm::vec3 min_near = intersect_ray_with_z_plane( dir_min, z_plane_near );
                const glm::vec3 min_far = intersect_ray_with_z_plane( dir_min, z_plane_far );
                const glm::vec3 max_near = intersect_ray_with_z_plane( dir_max, z_plane_near );
                const glm::vec3 max_far = intersect_ray_with_z_plane( dir_max, z_plane_far );

                glm::vec3 cluster_aabb_min_vs = glm::min( glm::min( min_near, min_far ), glm::min( max_near, max_far ) );
                glm::vec3 cluster_aabb_max_vs = glm::max( glm::max( min_near, min_far ), glm::max( max_near, max_far ) );

                // World AABB for debug.
                glm::vec4 cluster_aabb_min_ws4{ cluster_aabb_min_vs, 1.0f };
                glm::vec4 cluster_aabb_max_ws4{ cluster_aabb_max_vs, 1.0f };
                cluster_aabb_min_ws4 = inv_view * cluster_aabb_min_ws4;
                cluster_aabb_max_ws4 = inv_view * cluster_aabb_max_ws4;

                bool intersects_light_any = false;

                for ( u32 l = 0; l < light_count; ++l ) {
                    Light& light = scene->lights[ l ];

                    const glm::vec4 cr_vs4 = light_center_radius_vs[ l ];
                    const glm::vec3 c_vs{ cr_vs4.x, cr_vs4.y, cr_vs4.z };
                    const f32 r = cr_vs4.w;

                    if ( !sphere_intersects_aabb( c_vs, r, cluster_aabb_min_vs, cluster_aabb_max_vs ) ) {
                        continue;
                    }

                    intersects_light_any = true;

                    // Project light center to screen (keep same top-left convention as unproject).
                    const glm::vec4 c_ws4{ light.world_position.x, light.world_position.y, light.world_position.z, 1.0f };
                    glm::vec4 c_clip = last_camera.view_projection * c_ws4;

                    if ( c_clip.w == 0.0f ) {
                        continue;
                    }

                    const f32 inv_w = 1.0f / c_clip.w;
                    const f32 ndc_x = c_clip.x * inv_w;
                    const f32 ndc_y = c_clip.y * inv_w;

                    const f32 screen_x = ( ndc_x * 0.5f + 0.5f ) * frame_data.resolution_x;
                    const f32 screen_y = ( 1.0f - ( ndc_y * 0.5f + 0.5f ) ) * frame_data.resolution_y;

                    const glm::vec2 sphere_screen{ screen_x, screen_y };

                    const f32 d = glm::distance( sphere_screen, tile_center );

                    // Avoid negative under sqrt: if tile center is inside tile-radius, solid angle saturates to 2PI.
                    const f32 diff = glm::max( d * d - tile_radius_sq, 0.0f );

                    // Solid angle for a sphere "seen" from tile center proxy
                    // solid_angle = 2PI (1 - sqrt(d^2 - r^2)/d). Here r is tile_radius in screen space.
                    f32 solid_angle = 0.0f;
                    if ( d > 1.0e-6f ) {
                        solid_angle = ( 2.0f * rpi ) * ( 1.0f - ( sqrtf( diff ) / d ) );
                    } else {
                        solid_angle = ( 2.0f * rpi );
                    }

                    // Shadow-map face resolution heuristic (cubemap: 6 faces).
                    const f32 resolution = sqrtf( ( 4.0f * rpi * tile_pixels ) / ( 6.0f * glm::max( solid_angle, 1.0e-8f ) ) );

                    if ( resolution > light.shadow_map_resolution ) {
                        light.shadow_map_resolution = resolution;
                        light.tile_x = x;
                        light.tile_y = y;
                        light.solid_angle = solid_angle;
                    }
                }

                if ( enable_light_cluster_debug && intersects_light_any ) {
                    debug_draw.aabb(
                        glm::vec3{ cluster_aabb_min_ws4.x, cluster_aabb_min_ws4.y, cluster_aabb_min_ws4.z },
                        glm::vec3{ cluster_aabb_max_ws4.x, cluster_aabb_max_ws4.y, cluster_aabb_max_ws4.z },
                        { Color::get_distinct_color( z ) }
                    );
                }
            }
        }
    }
}
#endif // 0

void LightingRenderingFeature::create_gpu_resources( Renderer* renderer, RenderBlackboard* render_blackboard ) {

    for ( u32 i = 0; i < k_max_frames; ++i ) {

        render_blackboard->lighting.lights_list_sb[ i ] = renderer->gpu->create_buffer( {
            .size = sizeof( GpuLight ) * k_num_lights,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
            .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .name = "light_array" } );

        render_blackboard->lighting.light_z_bins_sb[ i ] = renderer->gpu->create_buffer( {
            .size = sizeof( u32 ) * k_light_z_bins,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
            .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .name = "light_z_bins" } );

        render_blackboard->lighting.lights_indices_sb[ i ] = renderer->gpu->create_buffer( {
            .size = sizeof( u32 ) * k_num_lights,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
            .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .name = "light_indices_sb" } );

        const ClusteringLightingTileInfo tile_info = calculate_light_tile_buffer_size(
        renderer->gpu->swapchain_width, renderer->gpu->swapchain_height );

        render_blackboard->lighting.lights_tiles_sb[ i ] = renderer->gpu->create_buffer( {
            .size = tile_info.buffer_size,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
            .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .name = "light_tiles" } );
    }
}

void LightingRenderingFeature::destroy_gpu_resources( Renderer* renderer, RenderBlackboard* render_blackboard ) {

    GpuDevice* gpu = renderer->gpu;
    LightingRuntimeData& lighting = render_blackboard->lighting;

    for ( u32 i = 0; i < k_max_frames; ++i ) {
        gpu->destroy_buffer( lighting.light_z_bins_sb[ i ] );
        gpu->destroy_buffer( lighting.lights_tiles_sb[ i ] );
        gpu->destroy_buffer( lighting.lights_indices_sb[ i ] );
        gpu->destroy_buffer( lighting.lights_list_sb[ i ] );

    }
}

void LightingRenderingFeature::on_resize( Renderer* renderer, RenderBlackboard* render_blackboard, u32 new_width, u32 new_height ) {

    GpuDevice& gpu = *renderer->gpu;
    LightingRuntimeData& lighting = render_blackboard->lighting;

    for ( u32 i = 0; i < k_max_frames; ++i ) {

        gpu.destroy_buffer( lighting.lights_tiles_sb[ i ] );

        ClusteringLightingTileInfo tile_info = calculate_light_tile_buffer_size( new_width, new_height );

        lighting.lights_tiles_sb[ i ] = renderer->gpu->create_buffer( {
            .size = tile_info.buffer_size,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
            .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .name = "light_tiles" } );
    }
}

// PostProcessRenderingFeature ///////////////////////////////////////////

void PostProcessRenderingFeature::update_psos( Renderer* renderer, FrameGraph* frame_graph, PipelineUpdatePhase phase ) {

    if ( phase == PipelineUpdatePhase::Destroy ) {
        renderer->destroy_graphics_pipeline_state( passthrough_pipeline );
        renderer->destroy_graphics_pipeline_state( main_post_pipeline );
        renderer->destroy_graphics_pipeline_state( main_post_pipeline_slang );

        return;
    }

    GraphicsPipelineTransaction transaction( renderer );

    GraphicsPipelineState& new_pipeline = transaction.add( main_post_pipeline );
    GraphicsPipelineState& new_pipeline_slang = transaction.add( main_post_pipeline_slang );
    GraphicsPipelineState& new_pipeline_pass = transaction.add( passthrough_pipeline );

    renderer->create_graphics_pipeline_state(
        {
        .stages = {
            {
                .source_file_path = "glsl/postprocess.glsl",
                .type = VK_SHADER_STAGE_VERTEX_BIT,
            },
            {
                .source_file_path = "glsl/postprocess.glsl",
                .type = VK_SHADER_STAGE_FRAGMENT_BIT,
            },
        },
        .name = "main_triangle" },
        {
            .name = "passthrough",
            .render_pass_name = "swapchain",
        },
        "passthrough", frame_graph, new_pipeline_pass );

    renderer->create_graphics_pipeline_state(
        {
        .stages = {
            {
                .source_file_path = "glsl/postprocess.glsl",
                .type = VK_SHADER_STAGE_VERTEX_BIT,
            },
            {
                .source_file_path = "glsl/postprocess.glsl",
                .type = VK_SHADER_STAGE_FRAGMENT_BIT,
            },
        },
        .name = "main_post" },
        {
            .name = "main_post",
            .render_pass_name = "swapchain",
        },
        "main_post", frame_graph, new_pipeline );

    renderer->create_graphics_pipeline_state(
        {
        .stages = {
            {
                .source_file_path = "slang/postprocess.slang",
                .type = VK_SHADER_STAGE_VERTEX_BIT,
            },
            {
                .source_file_path = "slang/postprocess.slang",
                .type = VK_SHADER_STAGE_FRAGMENT_BIT,
            },
        },
        .name = "main_post_slang", .slang_input = 1 },
        {
            .name = "main_post_slang",
            .render_pass_name = "swapchain",
        },
        "main_post_slang", frame_graph, new_pipeline_slang );

    transaction.commit_or_rollback();
}

void PostProcessRenderingFeature::create_gpu_resources( Renderer* renderer, RenderBlackboard* render_blackboard, FrameGraph* frame_graph ) {

    DescriptorSetLayoutHandle descriptor_set_layout = renderer->gpu->get_descriptor_set_layout( main_post_pipeline.pipeline, k_material_descriptor_set_index );
    fullscreen_ds = renderer->gpu->create_descriptor_set( {
        .dynamic_buffers = {{11, sizeof( GpuPostProcessConstants )}},
        .layout = descriptor_set_layout, .name = "post_process_ds" });
}

void PostProcessRenderingFeature::destroy_gpu_resources( Renderer* renderer, RenderBlackboard* render_blackboard ) {

    renderer->gpu->destroy_descriptor_set( fullscreen_ds );
}

void PostProcessRenderingFeature::on_resize( Renderer* renderer, RenderBlackboard* render_blackboard, u32 new_width, u32 new_height ) {

}

void PostProcessRenderingFeature::upload_gpu_data( UploadGpuDataContext& context ) {

    Renderer* renderer = context.renderer;
    FrameGraph* frame_graph = context.frame_graph;
    RenderBlackboard& render_blackboard = context.render_blackboard;
    RenderConfig& render_config = context.render_config;
    GpuDevice& gpu = *renderer->gpu;

    // Update per mesh material buffer
    // TODO: update only changed stuff, this is now dynamic so it can't be done.
    GpuPostProcessConstants* gpu_constants = gpu.dynamic_buffer_allocate<GpuPostProcessConstants>( &post_cb_offset );
    if ( gpu_constants ) {

        FrameGraphResource* texture = frame_graph->get_resource( "final" );
        RASSERT( texture != nullptr );
        // TODO: proper handling.
        ImageViewHandle input_image_view = texture->resource_info.texture.image_view;
        if ( render_config.taa.enabled && render_blackboard.taa_output_image_view.is_valid() ) {
            input_image_view = render_blackboard.taa_output_image_view;
        }

        gpu_constants->tonemap_type = render_config.post.tonemap_mode;
        gpu_constants->exposure = render_config.post.exposure;
        gpu_constants->sharpening_amount = render_config.post.sharpening_amount;
        gpu_constants->input_texture_index = input_image_view.index();

        gpu_constants->enable_zoom = render_config.post.enable_zoom ? 1 : 0;
        gpu_constants->zoom_scale = (f32)render_config.post.zoom_scale;

        gpu_constants->resolution_x = texture->resource_info.texture.width;
        gpu_constants->resolution_y = texture->resource_info.texture.height;

        // Search for bloom texture
        FrameGraphResource* bloom_texture = frame_graph->get_resource( "bloom" );
        if ( bloom_texture != nullptr ) {
            gpu_constants->bloom_texture_index = bloom_texture->resource_info.texture.image_view.index();
            gpu_constants->bloom_amount = render_config.post.bloom_amount;
        } else {
            // If not present, just disable bloom
            gpu_constants->bloom_texture_index = u32_max;
            gpu_constants->bloom_amount = 0.0f;
        }

        /*if ( !render_config.post.block_zoom_input ) {
            gpu_constants->mouse_uv = glm::vec2{ context.last_clicked_position_left_button.x / gpu.swapchain_width,
                                             context.last_clicked_position_left_button.y / gpu.swapchain_height };
        }*/
    }
}

// PointlightShadowsRenderingFeature /////////////////////////////////////

void PointlightShadowsRenderingFeature::create_gpu_resources( Renderer* renderer, RenderBlackboard* render_blackboard,
                                                              RenderScene* scene ) {


}

void PointlightShadowsRenderingFeature::destroy_gpu_resources( Renderer* renderer, RenderBlackboard* render_blackboard ) {

}

void PointlightShadowsRenderingFeature::on_resize( Renderer* renderer, RenderBlackboard* render_blackboard, u32 new_width, u32 new_height ) {

}

void PointlightShadowsRenderingFeature::upload_gpu_data( UploadGpuDataContext& context ) {

}

// RayTracingRenderFeature ///////////////////////////////////////////////
void RayTracingRenderFeature::init( Allocator* allocator, Renderer* renderer ) {

    ray_tracing_scene.blases.init( allocator, 16 );
    blas_meshes_to_build.init( allocator, 16 );
}

void RayTracingRenderFeature::shutdown( Renderer* renderer ) {

    // BLASES and TLAS are destroyed in destroy_gpu_resources, here we just clear the arrays.
    ray_tracing_scene.blases.shutdown();
    blas_meshes_to_build.shutdown();
}

void RayTracingRenderFeature::add_meshes_to_build_from_scene( RenderScene* scene ) {

    ray_tracing_scene.blases.clear();
    ray_tracing_scene.blases.set_size( scene->meshes.size );
    ray_tracing_scene.blases.set_capacity( scene->meshes.size );

    for ( u32 i = 0; i < scene->meshes.size; ++i ) {
        ray_tracing_scene.blases[ i ] = {};
        blas_meshes_to_build.push( i );
    }

    tlas_needs_rebuild = true;
}

void RayTracingRenderFeature::build_acceleration_structures_from_scene( Renderer* renderer, RenderScene* scene, ArenaAllocator* scratch, f32 global_scale ) {

    // Create BLAS for each mesh. We create one BLAS per mesh, but we could batch multiple meshes in the same BLAS if needed.
    // BLAS build will be queued and executed later.
    for ( i32 i = blas_meshes_to_build.size - 1; i >= 0; i-- ) {
        u32 mesh_index = blas_meshes_to_build[ i ];
        Mesh& mesh = scene->meshes[ mesh_index ];

        // Always put a handle, so that blas and meshes have the same index.
        if ( mesh.position_buffer.is_invalid() || mesh.index_buffer.is_invalid() ) {

            continue;
        }

        // Check that both position and index buffers are valid, otherwise skip this mesh (we can't build a BLAS without them).
        Buffer* position_buffer = renderer->gpu->get_buffer( mesh.position_buffer );
        if ( !position_buffer->ready ) {
            continue;
        }

        Buffer* index_buffer = renderer->gpu->get_buffer( mesh.index_buffer );
        if ( !index_buffer->ready ) {
            continue;
        }

        // Both position and index buffers are valid and ready, we can build the BLAS for this mesh
        BLASGeometry geometry{};
        geometry.index_buffer = mesh.index_buffer;
        geometry.index_buffer_offset = mesh.index_offset_bytes;
        geometry.index_type = mesh.index_type;
        geometry.max_vertex = mesh.position_count - 1;
        geometry.position_format = VK_FORMAT_R32G32B32_SFLOAT;
        geometry.primitive_count = mesh.index_count / 3;
        geometry.vertex_buffer = mesh.position_buffer;
        geometry.vertex_buffer_offset = mesh.position_offset;
        geometry.vertex_stride = sizeof( f32 ) * 3;

        BLASHandle blas = renderer->gpu->create_blas( { .geometries = {geometry}, .name = "mesh_blas" } );
        ray_tracing_scene.blases[ mesh_index ] = blas;

        blas_meshes_to_build.delete_swap( i );
    }

    if ( blas_meshes_to_build.size > 0 || !tlas_needs_rebuild ) {
        return;
    }

    // Create TLAS with all mesh instances.
    u64 marker = scratch->get_marker();
    Array<TLASGeometryInstance> blas_instances;
    blas_instances.init( scratch, scene->mesh_instances.size, scene->mesh_instances.size );

    SceneGraph* scene_graph = scene->scene_graph;

    const glm::mat4 scale_matrix = glm::scale( glm::mat4( 1.0f ), glm::vec3( global_scale ) );

    for ( u32 i = 0; i < scene->mesh_instances.size; ++i ) {
        const MeshInstance& mesh_instance = scene->mesh_instances[ i ];
        BLASHandle blas = ray_tracing_scene.blases[ mesh_instance.mesh_index ];

        const glm::mat4& world_transform = scale_matrix * scene_graph->world_matrices[ mesh_instance.scene_graph_node_index ];

        TLASGeometryInstance& blas_instance = blas_instances[ i ];
        blas_instance.blas = blas;
        blas_instance.transform = to_vk_transform_matrix( world_transform );
        blas_instance.instance_custom_index = i;
        blas_instance.sbt_record_offset = 0;
        blas_instance.mask = 0xff;
        blas_instance.flags = VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
    }

    ray_tracing_scene.tlas = renderer->gpu->create_tlas( { .instances = blas_instances.as_cspan(), .name = "scene_tlas"});

    tlas_needs_rebuild = false;

    scratch->free_marker( marker );
}

void RayTracingRenderFeature::build_acceleration_structures_from_static_scene( Renderer* renderer, RenderScene* scene, ArenaAllocator* scratch, f32 global_scale ) {

    if ( !tlas_needs_rebuild ) {
        return;
    }

    // Check if all static meshes are uploaded
    for ( u32 i = 0; i < scene->meshes.size; ++i ) {
        Mesh& mesh = scene->meshes[ i ];
        if ( mesh.position_buffer.is_invalid() || mesh.index_buffer.is_invalid() ) {
            continue;
        }
        Buffer* position_buffer = renderer->gpu->get_buffer( mesh.position_buffer );
        if ( !position_buffer->ready ) {
            return;
        }
        Buffer* index_buffer = renderer->gpu->get_buffer( mesh.index_buffer );
        if ( !index_buffer->ready ) {
            return;
        }
    }

    // Static meshes are ready, build BLAS for each mesh and TLAS for the scene.

    ArenaScope scratch_scope( scratch );
    Array<BLASGeometry> geometries;
    Array<VkTransformMatrixKHR> geometry_transforms;

    geometries.init( scratch, scene->mesh_instances.size );
    geometry_transforms.init( scratch, scene->mesh_instances.size );

    sizet geometry_transform_buffer_size = sizeof( VkTransformMatrixKHR ) * scene->mesh_instances.size;

    ray_tracing_scene.geometry_transform_buffer = renderer->gpu->create_buffer( {
        .size = geometry_transform_buffer_size,
        .usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                            VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .name = "geometry_transform_buffer" } );

    const glm::mat4 scale_matrix = glm::scale( glm::mat4( 1.0f ), glm::vec3( global_scale ) );

    for ( u32 i = 0; i < scene->mesh_instances.size; ++i ) {
        const MeshInstance& mesh_instance = scene->mesh_instances[ i ];
        const Mesh& mesh = scene->meshes[ mesh_instance.mesh_index ];

        if ( mesh.position_buffer.is_invalid() || mesh.index_buffer.is_invalid() ) {
            continue;
        }

        Buffer* position_buffer = renderer->gpu->get_buffer( mesh.position_buffer );
        Buffer* index_buffer = renderer->gpu->get_buffer( mesh.index_buffer );

        if ( !position_buffer->ready || !index_buffer->ready ) {
            continue;
        }

        const glm::mat4 world_transform = scale_matrix * scene->scene_graph->world_matrices[ mesh_instance.scene_graph_node_index ];
        geometry_transforms.push( to_vk_transform_matrix( world_transform ) );

        const u32 geometry_index = geometries.size;

        BLASGeometry geometry {};
        geometry.index_buffer = mesh.index_buffer;
        geometry.index_buffer_offset = mesh.index_offset_bytes;
        geometry.index_type = mesh.index_type;
        geometry.max_vertex = mesh.position_count - 1;
        geometry.position_format = VK_FORMAT_R32G32B32_SFLOAT;
        geometry.primitive_count = mesh.index_count / 3;
        geometry.vertex_buffer = mesh.position_buffer;
        geometry.vertex_buffer_offset = mesh.position_offset;
        geometry.vertex_stride = sizeof( f32 ) * 3;
        geometry.transform_buffer = ray_tracing_scene.geometry_transform_buffer;
        geometry.transform_buffer_offset = sizeof( VkTransformMatrixKHR ) * geometry_index;
        //geometry.geometry.triangles.transformData.deviceAddress = renderer->gpu->get_buffer_device_address( geometry_transform_buffer );
        //geometry.transform = to_vk_transform_matrix( world_transform );

        geometries.push( geometry );
    }

    Buffer* transform_buffer = renderer->gpu->get_buffer( ray_tracing_scene.geometry_transform_buffer );
    memcpy( transform_buffer->mapped_data, geometry_transforms.data, geometry_transform_buffer_size );
    renderer->gpu->flush_buffer( ray_tracing_scene.geometry_transform_buffer, 0, geometry_transform_buffer_size );

    BLASHandle blas = renderer->gpu->create_blas( { .geometries = {geometries.as_cspan()}, .name = "mesh_blas" } );
    ray_tracing_scene.blases.push( blas );

    TLASGeometryInstance blas_instance = {};
    blas_instance.blas = blas;
    blas_instance.transform = to_vk_transform_matrix( glm::mat4( 1.0f ) );
    blas_instance.instance_custom_index = 0;
    blas_instance.sbt_record_offset = 0;
    blas_instance.mask = 0xff;
    blas_instance.flags = VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
    ray_tracing_scene.tlas = renderer->gpu->create_tlas( { .instances = {blas_instance}, .name = "scene_tlas"});

    tlas_needs_rebuild = false;
}

void RayTracingRenderFeature::build_or_update_tlas( Renderer* renderer, RenderScene* scene, ArenaAllocator* scratch, CommandBuffer* cb ) {

}

void RayTracingRenderFeature::destroy_gpu_resources( Renderer* renderer, RenderBlackboard* render_blackboard ) {

    for ( u32 i = 0; i < ray_tracing_scene.blases.size; ++i ) {
        renderer->gpu->destroy_blas( ray_tracing_scene.blases[ i ] );
    }

    if ( ray_tracing_scene.tlas.is_valid() ) {
        renderer->gpu->destroy_tlas( ray_tracing_scene.tlas );
    }

    ray_tracing_scene.blases.clear();
    ray_tracing_scene.tlas = {};

    renderer->gpu->destroy_buffer( ray_tracing_scene.geometry_transform_buffer );
}

void RayTracingRenderFeature::upload_gpu_data( UploadGpuDataContext& context ) {

    // Expose TLAS handle in the blackboard, so that ray tracing shaders can access it.
    context.render_blackboard.tlas = ray_tracing_scene.tlas;
}

} // namespace raptor
