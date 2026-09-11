
#include "graphics/render_passes/pointlight_shadow_pass.hpp"
#include "graphics/render_scene.hpp"
#include "graphics/render_blackboard.hpp"

#include "foundation/numerics.hpp"

#include "external/glm/matrix.hpp"
#include "external/glm/mat4x4.hpp"
#include "external/glm/vec3.hpp"


#include "graphics/render_passes/meshlet_pipelines.hpp"

namespace raptor {


struct ShadowDrawKey {
    u32     light_index;
    u32     face_index;
    u32     mip_level;
    u32     shadow_slot;
};

struct ShadowDrawMeta {
    u32     light_index;
    u32     face_index;
    u32     mip_level;
    u32     shadow_slot;

    u32     meshlet_base;          // base index into shadow_meshlet_instances[]
    u32     meshlet_count;         // number of meshlets for this draw segment

    u32     draw_command_index;    // index into indirect command buffer
    u32     meshlet_allocated;     // debug: allocated meshlets (usually == meshlet_count)
};

struct ShadowMeshletInstance {
    u32     mesh_instance_index;
    u32     global_meshlet_index;
};

struct ShadowDrawDebug {
    u32     allocated_meshlets;
    u32     emitted_meshlets;
    u32     culled_meshlets;

    u32     face_used;
    u32     mip_used;
    u32     padding0;
};

struct ShadowGlobalDebugInfo {
    u32     total_draws_built;
    u32     total_meshlets_allocated;
    u32     total_meshlets_emitted;
    u32     padding1;
};

// PointlightShadowPass2 /////////////////////////////////////////////////
void PointlightShadowPass2::declare_frame_graph_node( FrameGraphResourceContext& context ) {
    FrameGraphBuilder& builder = *context.frame_graph->builder;

    context.frame_graph->add_node_v2( {
        .outputs = {
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Texture,
                .resource_info = {
                    .external = true
                },
                .name = "point_shadows_depth",
            } )
        },
        .scheduling = { CommandQueueType::Graphics, 0 },
        .enabled = true,
        .compute = true,
        .name = "point_shadows_pass" } );
}

void PointlightShadowPass2::update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) {

    Renderer* renderer = context.renderer;

    if ( phase == PipelineUpdatePhase::Destroy ) {
        renderer->destroy_compute_pipeline_state( clear_counters_pipeline );
        renderer->destroy_compute_pipeline_state( build_lists_pipeline );
        renderer->destroy_compute_pipeline_state( build_indirect_cmds_pipeline );
        renderer->destroy_compute_pipeline_state( shadow_resolution_pipeline );

        renderer->destroy_graphics_pipeline_state( meshlet_draw_pipeline );

        return;
    }

    // Compute pipelines
    ComputePipelineTransaction compute_transaction( renderer );

    ComputePipelineState& new_clear_pipeline = compute_transaction.add( clear_counters_pipeline );
    ComputePipelineState& new_list_pipeline = compute_transaction.add( build_lists_pipeline );
    ComputePipelineState& new_commands_pipeline = compute_transaction.add( build_indirect_cmds_pipeline );
    ComputePipelineState& new_resolution_pipeline = compute_transaction.add( shadow_resolution_pipeline );

    renderer->create_compute_pipeline_state(
        {
        .stages = {
            {
                .source_file_path = "glsl/meshlet_shadows.glsl",
                .type = VK_SHADER_STAGE_COMPUTE_BIT,
            }
        },
        .name = "clear_counters" },
        {
            .name = "meshlet_shadows_clear_counters",
            .render_pass_name = "point_shadows_pass",
        },
        "meshlet_shadows",
        context.frame_graph, new_clear_pipeline );

    renderer->create_compute_pipeline_state( {
        .stages = {
            {
                .source_file_path = "glsl/meshlet_shadows.glsl",
                .type = VK_SHADER_STAGE_COMPUTE_BIT,
            }
        },
        .name = "build_lists" },
        {
            .name = "meshlet_shadows_build_lists",
            .render_pass_name = "point_shadows_pass",
        },
        "meshlet_shadows",
        context.frame_graph, new_list_pipeline );

    renderer->create_compute_pipeline_state(
        {
        .stages = {
            {
                .source_file_path = "glsl/meshlet_shadows.glsl",
                .type = VK_SHADER_STAGE_COMPUTE_BIT,
            }
        },
        .name = "build_indirect_cmds" },
        {
            .name = "meshlet_shadows_build_indirect_cmds",
            .render_pass_name = "point_shadows_pass",
        },
        "meshlet_shadows",
        context.frame_graph, new_commands_pipeline );

    renderer->create_compute_pipeline_state(
        {
        .stages = {
            {
                .source_file_path = "glsl/meshlet_shadows.glsl",
                .type = VK_SHADER_STAGE_COMPUTE_BIT,
            }
        },
        .name = "pointshadows_resolution_calculation" },
        {
            .name = "meshlet_shadows_pointshadows_resolution_calculation",
            .render_pass_name = "point_shadows_pass",
        },
        "meshlet_shadows",
        context.frame_graph, new_resolution_pipeline );

    compute_transaction.commit_or_rollback();

    // Graphics pipelines
    GraphicsPipelineTransaction transaction( renderer );

    GraphicsPipelineState& new_draw_pipeline = transaction.add( meshlet_draw_pipeline );

    renderer->create_graphics_pipeline_state(
        {
            .stages = {
                {
                    .source_file_path = "glsl/meshlet_shadows.glsl",
                    .type = VK_SHADER_STAGE_TASK_BIT_EXT,
                },
                {
                    .source_file_path = "glsl/meshlet_shadows.glsl",
                    .type = VK_SHADER_STAGE_MESH_BIT_EXT,
                }
            },
            .name = "meshlet_depth" },
        pc_depth_cubemap_meshlet,
        "meshlet_shadows",
        context.frame_graph, new_draw_pipeline );

    transaction.commit_or_rollback();
}

void PointlightShadowPass2::upload_gpu_data( FrameGraphResourceContext& context ) {
    recreate_lightcount_dependent_resources( context );
}

// Push constant structs
struct BuildShadowListsPushConstants {
    u32 key_count;
    u32 mesh_instance_count;
    u32 tiles_per_key;
    u32 draw_base[ k_shadow_mip_count ];
};

struct BuildShadowIndirectPushConstants {
    u32 meshlets_per_task_wg;
    u32 draw_base[ k_shadow_mip_count ];
};

struct ShadowDrawPushConstants {
    u32 draw_meta_base;
};

void PointlightShadowPass2::render( FrameGraphRenderContext& context ) {

    // Skip rendering if shadows are disabled
    if ( context.render_config->shadows.disable_shadows ) {
        return;
    }

    Renderer* renderer = context.renderer;
    GpuDevice& gpu = *renderer->gpu;
    RenderScene* scene = context.render_view->scene;

    u32 active_lights = scene->active_lights;
    u32 num_keys = active_lights;

    const u32 frame_index = context.current_frame_index;
    const u32 instances_per_tile = 256;
    const u32 tiles_per_key = ceilu32( scene->mesh_instances.size / float( instances_per_tile ) );
    CommandBuffer* cb = context.gpu_commands;
    PointlightShadowsRuntimeData& shadows = context.render_blackboard->point_shadows;

    {
        // Clear resolution buffer
        BufferHandle resolution_buffer = shadows.shadow_resolutions[ frame_index ];

        cb->fill_buffer( resolution_buffer, 0, sizeof( u32 ) * active_lights, 0 );

        cb->add_buffer_barrier( resolution_buffer, 0, VK_WHOLE_SIZE,
                                { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT } );
        cb->add_memory_barrier( VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT );
        cb->flush_barriers();

        // Calculate shadow resolutions
        cb->bind_pipeline( shadow_resolution_pipeline.pipeline );

        cb->bind_descriptor_set( { renderer->gpu->bindless_descriptor_set, shadow_resolution_descriptor_set[ frame_index ] },
                                 { context.render_blackboard->scene_cb_offset } );

        u32 depth_pyramid_texture_index = scene->mesh_draw_counts.depth_pyramid_texture_index;

        cb->push_constants( shadow_resolution_pipeline.pipeline, 0, sizeof( depth_pyramid_texture_index ), &depth_pyramid_texture_index );

        const u32 tile_size = 64;

        const u32 tile_x_count = ceilu32( context.render_blackboard->render_width / float( tile_size ) );
        const u32 tile_y_count = ceilu32( context.render_blackboard->render_height / float( tile_size ) );

        const u32 group_x = ceilu32( tile_x_count / 8.0f );
        const u32 group_y = ceilu32( tile_y_count / 8.0f );

        cb->dispatch( group_x, group_y, 1 );

        // Copy to readback
        cb->add_buffer_barrier( shadows.shadow_resolutions[ frame_index ], 0, VK_WHOLE_SIZE,
                                { VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT } );

        cb->add_buffer_barrier( shadows.shadow_resolutions_readback[ frame_index ], 0, VK_WHOLE_SIZE,
                                { VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT } );

        cb->add_memory_barrier( VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT );

        cb->flush_barriers();

        cb->copy_buffer( shadows.shadow_resolutions[ frame_index ], 0, shadows.shadow_resolutions_readback[ frame_index ], 0, sizeof( u32 ) * active_lights );

        shadows.shadow_resolution_readback_valid[ frame_index ] = true;
    }

    u32 mip_key_count[ k_shadow_mip_count ]{};

    // Build shadow keys, one key per light
    BufferHandle draw_keys = draw_keys_sb[ frame_index ];
    ShadowDrawKey* gpu_keys = (ShadowDrawKey*)gpu.map_buffer( { draw_keys } );
    if ( gpu_keys ) {

        for ( u32 i = 0; i < active_lights; i++ ) {
            const Light& light = scene->lights[ i ];
            ShadowDrawKey& key = gpu_keys[ i ];

            key.light_index = i;
            key.face_index = 0;
            key.mip_level = light.shadow_mip_level;
            key.shadow_slot = 0;

            RASSERT( key.mip_level < k_shadow_mip_count );
            ++mip_key_count[ key.mip_level ];
        }

        gpu.unmap_buffer( { draw_keys } );
    }

    // Calculate draw capacity per mip level, used to calculate offsets into the indirect draw buffer
    u32 draw_capacity[ k_shadow_mip_count ];

    for ( u32 mip = 0; mip < k_shadow_mip_count; ++mip ) {
        draw_capacity[ mip ] = mip_key_count[ mip ] * tiles_per_key * 6;
    }

    u32 draw_base[ k_shadow_mip_count ];
    draw_base[ 0 ] = 0;
    draw_base[ 1 ] = draw_base[ 0 ] + draw_capacity[ 0 ];
    draw_base[ 2 ] = draw_base[ 1 ] + draw_capacity[ 1 ];

    const u32 max_draws = num_keys * tiles_per_key * 6;

    RASSERT( draw_base[ 2 ] + draw_capacity[ 2 ] == max_draws );

    // Clear counters
    cb->bind_pipeline( clear_counters_pipeline.pipeline );

    cb->add_buffer_barrier( draw_keys, 0, VK_WHOLE_SIZE,
                            { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_READ_BIT } );

    cb->add_buffer_barrier( indirect_draw_count_sb[ frame_index ], 0, VK_WHOLE_SIZE,
                            { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_WRITE_BIT } );

    cb->add_buffer_barrier( meshlet_instance_cursor_sb[ frame_index ], 0, VK_WHOLE_SIZE,
                            { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_WRITE_BIT } );

    cb->add_buffer_barrier( global_debug_info_sb[ frame_index ], 0, VK_WHOLE_SIZE,
                            { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_WRITE_BIT } );
    cb->flush_barriers();

    cb->bind_descriptor_set( { renderer->gpu->bindless_descriptor_set, clear_counters_ds[ frame_index ] },
                              { context.render_blackboard->scene_cb_offset } );

    cb->dispatch( 1, 1, 1 );

    cb->barrier_instant_compute_write_to_compute_read();

    // Build lists
    cb->push_marker( " Build lists " );
    cb->add_buffer_barrier( meshlet_instances_sb[ frame_index ], 0, VK_WHOLE_SIZE,
                            { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_WRITE_BIT } );

    cb->add_buffer_barrier( draw_meta_sb[ frame_index ], 0, VK_WHOLE_SIZE,
                            { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_WRITE_BIT } );

    cb->flush_barriers();

    cb->bind_pipeline( build_lists_pipeline.pipeline );

    cb->bind_descriptor_set( { renderer->gpu->bindless_descriptor_set, build_lists_ds[ frame_index ] },
                              { context.render_blackboard->scene_cb_offset } );

    //u32 push_constants[ 4 ];
    //push_constants[ 0 ] = num_keys;
    //push_constants[ 1 ] = scene->mesh_instances.size;
    //push_constants[ 2 ] = tiles_per_key;
    //push_constants[ 3 ] = 0;

    BuildShadowListsPushConstants build_lists_push{
        .key_count = num_keys,
        .mesh_instance_count = ( u32 )scene->mesh_instances.size,
        .tiles_per_key = tiles_per_key,
        .draw_base = { draw_base[ 0 ], draw_base[ 1 ], draw_base[ 2 ] }
    };

    cb->push_constants( build_lists_pipeline.pipeline, 0, sizeof( build_lists_push ), &build_lists_push );
    cb->dispatch( tiles_per_key, num_keys, 1 );

    cb->barrier_instant_compute_write_to_compute_read();
    cb->pop_marker();

    // Build indirect commands
    cb->add_buffer_barrier( task_indirect_cmds_sb[ frame_index ], 0, VK_WHOLE_SIZE,
                            { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_WRITE_BIT } );

    cb->flush_barriers();

    cb->bind_pipeline( build_indirect_cmds_pipeline.pipeline );

    cb->bind_descriptor_set( { renderer->gpu->bindless_descriptor_set, build_indirect_cmds_ds[ frame_index ] },
                              { context.render_blackboard->scene_cb_offset } );

    BuildShadowIndirectPushConstants indirect_push{
        .meshlets_per_task_wg = 32,
        .draw_base = { draw_base[ 0 ], draw_base[ 1 ], draw_base[ 2 ] }
    };

    u32 max_bucket_draws = 0;

    for ( u32 mip = 0; mip < k_shadow_mip_count; ++mip ) {
        max_bucket_draws = raptor::max( max_bucket_draws, draw_capacity[ mip ] );
    }

    const u32 group_x = raptor::ceilu32( max_bucket_draws / 64.0f );

    cb->push_constants( build_indirect_cmds_pipeline.pipeline, 0,
                        sizeof( indirect_push ), &indirect_push );

    cb->dispatch( group_x, k_shadow_mip_count, 1 );

    // Draw
    if ( update_sparse_binding( context ) ) {

    }

    {
        // Calculate and cache sparse image memory stats
        SparseImageMemoryStats stats = gpu.get_sparse_image_memory_stats( shadow_maps_pool, cubemap_shadow_array_image );

        shadows.resident_pages = stats.resident_pages;
        shadows.max_mip0_pages = stats.max_mip0_pages;

        shadows.resident_memory = stats.resident_bytes;
        shadows.allocated_memory = stats.allocated_bytes;
        shadows.max_mip0_memory = stats.max_mip0_bytes;
    }

    Image* depth_texture_array = gpu.get_image( cubemap_shadow_array_image );
    const u32 layer_count = 6 * scene->active_lights;

    u32 width = depth_texture_array->width;
    u32 height = depth_texture_array->height;
    // Perform manual clear of active lights shadowmaps.
    {
        //util_add_image_barrier_ext( gpu, gpu_commands->vk_command_buffer, depth_texture_array, RESOURCE_STATE_COPY_DEST, 0, 1, 0, layer_count, true );
        const VkImageSubresourceRange range = raptor::range_depth( 0, k_shadow_mip_count, 0, layer_count );
        //cb->add_image_barrier( cubemap_shadow_array_image, range, { VK_PIPELINE_STAGE_2_TRANSFER_BIT,  VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL } );
        //cb->flush_barriers();

        //// TODO: Clearing 256 cubemaps is incredibly slow, for the future try with point sprites at far with depth test always.
        //VkClearRect clear_rect;
        //clear_rect.baseArrayLayer = 0;
        //clear_rect.layerCount = layer_count;
        //clear_rect.rect.extent.width = width;
        //clear_rect.rect.extent.height = height;
        //clear_rect.rect.offset.x = 0;
        //clear_rect.rect.offset.y = 0;

        //VkClearDepthStencilValue clear_depth_stencil_value;
        //clear_depth_stencil_value.depth = 1.f;

        //VkImageSubresourceRange clear_range;
        //clear_range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        //clear_range.baseArrayLayer = 0;
        //clear_range.baseMipLevel = 0;
        //clear_range.levelCount = 1;
        //clear_range.layerCount = layer_count;
        //vkCmdClearDepthStencilImage( cb->vk_command_buffer, depth_texture_array->vk_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_depth_stencil_value, 1, &clear_range );

        cb->add_image_barrier( cubemap_shadow_array_image, range,
                              { VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL } );

        // Indirect cmd flushes
        cb->add_buffer_barrier( task_indirect_cmds_sb[ frame_index ], 0, VK_WHOLE_SIZE,
                                { VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                                  VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT } );

        cb->add_buffer_barrier( indirect_draw_count_sb[ frame_index ], 0, VK_WHOLE_SIZE,
                                { VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                                  VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT } );

        cb->flush_barriers();
    }

    // Update view projection matrices and camera spheres.
    // NOTE: this operation can be slow on CPU if many lights are casting shadows, thus
    // a GPU implementation is also given.
    if ( scene->shadow_constants_cpu_update ) {

        Buffer* view_projections_cb = gpu.get_buffer( pointlight_view_projections_sb[ frame_index ] );
        Buffer* light_spheres_cb = gpu.get_buffer( pointlight_spheres_sb[ frame_index ] );

        glm::mat4* gpu_view_projections = ( glm::mat4* )view_projections_cb->mapped_data;
        glm::vec4* gpu_light_spheres = ( glm::vec4* )light_spheres_cb->mapped_data;

        const glm::mat4 left_handed_scale_matrix = glm::scale( glm::mat4( 1.0f ), { 1,1,-1 } );

        if ( gpu_view_projections && gpu_light_spheres ) {

            // Assumes:
            // - GLM_FORCE_DEPTH_ZERO_TO_ONE
            // - GLM_CLIP_CONTROL_RH_ZO
            // - Right-handed lookAt (glm::lookAt is RH by default with RH flags)

            static const glm::vec3 k_dirs[ 6 ] = {
                glm::vec3( +1,  0,  0 ), // +X
                glm::vec3( -1,  0,  0 ), // -X
                glm::vec3( 0, +1,  0 ), // +Y
                glm::vec3( 0, -1,  0 ), // -Y
                glm::vec3( 0,  0, +1 ), // +Z
                glm::vec3( 0,  0, -1 ), // -Z
            };

            // These ups avoid singularities at +/-Y and keep a consistent orientation.
            // (Common choice for cubemap rendering)
            static const glm::vec3 k_ups[ 6 ] = {
                glm::vec3( 0, -1,  0 ), // +X
                glm::vec3( 0, -1,  0 ), // -X
                glm::vec3( 0,  0, +1 ), // +Y
                glm::vec3( 0,  0, -1 ), // -Y
                glm::vec3( 0, -1,  0 ), // +Z
                glm::vec3( 0, -1,  0 ), // -Z
            };

            for ( u32 l = 0; l < scene->active_lights; ++l ) {
                const Light& light = scene->lights[ l ];
                const glm::vec3 p = light.world_position;

                gpu_light_spheres[ l ] = glm::vec4( p, light.radius );

                const float near_z = 0.01f;
                const float far_z = light.radius;

                // With GLM_FORCE_DEPTH_ZERO_TO_ONE + RH_ZO this should be Vulkan-correct.
                glm::mat4 proj = glm::perspective( glm::radians( 90.0f ), 1.0f, near_z, far_z );
                // Flip Y in clip space to comepensate negative viewport
                proj[ 1 ][ 1 ] *= -1.0f;

                for ( u32 f = 0; f < 6; ++f ) {
                    const glm::mat4 view = glm::lookAt( p, p + k_dirs[ f ], k_ups[ f ] );
                    gpu_view_projections[ l * 6 + f ] = proj * view;
                }
            }

            gpu.flush_buffer( pointlight_view_projections_sb[ frame_index ], 0, sizeof( glm::mat4 ) * scene->active_lights * 6 );
            gpu.flush_buffer( pointlight_spheres_sb[ frame_index ], 0, sizeof( glm::vec4 ) * scene->active_lights );
        }
    }

    cb->push_marker( "Draw meshlets" );

    const ShadowRenderConfig& shadow_config = context.render_config->shadows;
    // Draw meshlets using indirect commands, one draw per mip level.
    for ( u32 mip = 0; mip < k_shadow_mip_count; ++mip ) {

        // Do not touch sparse subresources that are not used.
        if ( mip_key_count[ mip ] == 0 ) {
            continue;
        }

        cb->begin_render_pass( { }, { VK_ATTACHMENT_LOAD_OP_DONT_CARE }, {},
            cubemap_shadow_mip_views[ mip ], VK_ATTACHMENT_LOAD_OP_CLEAR,
            { .depthStencil = { 1.0f } }, ImageViewHandle(), layer_count, 0 );

        // Fullscreen is mip-aware and thus calculate the correct viewport size for the mip level.
        cb->set_fullscreen_viewport();
        cb->set_fullscreen_scissor();

        cb->bind_pipeline( meshlet_draw_pipeline.pipeline );

        cb->bind_descriptor_set( { renderer->gpu->bindless_descriptor_set, meshlet_draw_ds[ frame_index ] },
                                 { context.render_blackboard->scene_cb_offset } );

        cb->set_depth_bias_enabled( true );

        // Change bias slope to be mip aware
        const f32 mip_scale = shadow_config.use_slope_mip_scale ? 1.0f / f32( 1u << mip ) : 1.0f;
        cb->set_depth_bias( shadow_config.depth_bias_constant, shadow_config.depth_bias_clamp,
                            shadow_config.depth_bias_slope * mip_scale );

        ShadowDrawPushConstants draw_push{
            .draw_meta_base = draw_base[ mip ]
        };

        cb->push_constants( meshlet_draw_pipeline.pipeline, 0, sizeof( draw_push ), &draw_push );

        const u32 argument_offset = draw_base[ mip ] * sizeof( VkDrawMeshTasksIndirectCommandEXT );
        const u32 count_offset = mip * sizeof( u32 );

        cb->draw_mesh_task_indirect_count( task_indirect_cmds_sb[ frame_index ], argument_offset,
                                           indirect_draw_count_sb[ frame_index ], count_offset,
                                           draw_capacity[ mip ], sizeof( VkDrawMeshTasksIndirectCommandEXT ) );

        cb->end_render_pass();
    }

    cb->pop_marker();
}

void PointlightShadowPass2::create_gpu_resources( FrameGraphResourceContext& context ) {

    Renderer* renderer = context.renderer;
    RenderScene* scene = context.render_scene;
    GpuDevice& gpu = *renderer->gpu;

    const u32 key_count = k_num_lights;
    const u32 tiles_per_key = ceilu32( scene->mesh_instances.size / 256.f );
    const u32 max_draws = key_count * tiles_per_key * 6;
    const u32 total_meshlets_scene = scene->meshlets.size;
    const u32 instances_capacity_total = key_count * total_meshlets_scene * 6;

    for ( u32 i = 0; i < k_max_frames; ++i ) {

        draw_keys_sb[ i ] = gpu.create_buffer( {
            .size = sizeof( ShadowDrawKey ) * key_count,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
            .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .name = "shadow_draw_key_sb" } );

        draw_meta_sb[ i ] = gpu.create_buffer( {
            .size = sizeof( ShadowDrawMeta ) * max_draws,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .name = "shadows.draw_meta_sb" } );

        task_indirect_cmds_sb[ i ] = gpu.create_buffer( {
            .size = sizeof( VkDrawMeshTasksIndirectCommandEXT ) * max_draws,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .name = "shadows.task_indirect_cmds_sb" } );

        meshlet_instances_sb[ i ] = gpu.create_buffer( {
            .size = sizeof( ShadowMeshletInstance ) * instances_capacity_total,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .name = "shadows.meshlet_instances_sb" } );

        indirect_draw_count_sb[ i ] = gpu.create_buffer( {
            .size = sizeof( u32 ) * k_shadow_mip_count,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .name = "shadows.indirect_draw_count_sb" } );

        meshlet_instance_cursor_sb[ i ] = gpu.create_buffer( {
            .size = sizeof( u32 ),
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .name = "shadows.meshlet_instance_cursor_sb" } );

        draw_debug_sb[ i ] = gpu.create_buffer( {
            .size = sizeof( ShadowDrawDebug ) * max_draws,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .name = "shadows.draw_debug_sb" } );

        global_debug_info_sb[ i ] = gpu.create_buffer( {
            .size = sizeof( ShadowGlobalDebugInfo ),
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .name = "shadows.global_debug_info_sb" } );

        // These buffers will be persistently mapped to write on the CPU.
        pointlight_view_projections_sb[ i ] = gpu.create_buffer( {
            .size = sizeof( glm::mat4 ) * 6 * k_num_lights,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
            .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .name = "pointlight_pass_view_projections" } );

        pointlight_spheres_sb[ i ] = gpu.create_buffer( {
            .size = sizeof( glm::vec4 ) * k_num_lights,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
            .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .name = "pointlight_pass_spheres" } );
    }

    //// Create cubemap debug texture
    //// TODO(marco): these textures should only be created once, we only need to change
    //// the pages that are bound
    //texture_creation.reset().set_size( layer_width, layer_height, 1 ).set_format_type( depth_texture_format, TextureType::Texture2D )
    //    .set_flags( TextureFlags::RenderTarget_mask ).set_name( "cubemap_array_debug" );
    //cubemap_debug_face_image = gpu.create_image( texture_creation );

    //cubemap_debug_face_image_view = gpu.create_image_view( {
    //    .parent_image = cubemap_debug_face_image, .view_type = VK_IMAGE_VIEW_TYPE_2D,
    //    .sub_resource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 }, .name = texture_creation.name } );

    //gpu.add_image_view_to_bindless( cubemap_debug_face_image_view );


    recreate_lightcount_dependent_resources( context );

    // Descriptor sets creation
    RenderBlackboard& render_blackboard = *context.render_blackboard;

    DescriptorSetBinder descriptors;

    // TODO: use one descriptor set for all ?
    ShaderReflectionInfo* shader_reflection = nullptr;
    PointlightShadowsRuntimeData& shadows = render_blackboard.point_shadows;

    for ( u32 i = 0; i < k_max_frames; ++i ) {

        shader_reflection = renderer->get_shader_reflection( clear_counters_pipeline.pipeline );
        descriptors.reset();
        add_descriptors( descriptors, shader_reflection, shadows, i );
        descriptors.name = "point_shadows_clear_ds";
        clear_counters_ds[ i ] = renderer->create_descriptor_set( descriptors, shader_reflection, clear_counters_pipeline.pipeline, i, render_blackboard );

        shader_reflection = renderer->get_shader_reflection( build_lists_pipeline.pipeline );
        descriptors.reset();
        add_descriptors( descriptors, shader_reflection, shadows, i );
        descriptors.name = "point_shadows_build_lists_ds";
        build_lists_ds[ i ] = renderer->create_descriptor_set( descriptors, shader_reflection, build_lists_pipeline.pipeline, i, render_blackboard );

        shader_reflection = renderer->get_shader_reflection( build_indirect_cmds_pipeline.pipeline );
        descriptors.reset();
        add_descriptors( descriptors, shader_reflection, shadows, i );
        descriptors.name = "point_shadows_build_indirect_ds";
        build_indirect_cmds_ds[ i ] = renderer->create_descriptor_set( descriptors, shader_reflection, build_indirect_cmds_pipeline.pipeline, i, render_blackboard );

        shader_reflection = renderer->get_shader_reflection( meshlet_draw_pipeline.pipeline );
        descriptors.reset();
        add_descriptors( descriptors, shader_reflection, shadows, i );
        descriptors.name = "point_shadows_meshlet_draw_ds";
        meshlet_draw_ds[ i ] = renderer->create_descriptor_set( descriptors, shader_reflection, meshlet_draw_pipeline.pipeline, i, render_blackboard );

        shader_reflection = renderer->get_shader_reflection( shadow_resolution_pipeline.pipeline );
        descriptors.reset();
        add_descriptors( descriptors, shader_reflection, shadows, i );
        descriptors.name = "point_shadows_shadow_resolution_ds";
        shadow_resolution_descriptor_set[ i ] = renderer->create_descriptor_set( descriptors, shader_reflection, shadow_resolution_pipeline.pipeline, i, render_blackboard );
    }
}

void PointlightShadowPass2::add_descriptors( DescriptorSetBinder& descriptors, ShaderReflectionInfo* shader_reflection,
                                             PointlightShadowsRuntimeData& shadows, u32 frame_index ) {

    u16 binding = shader_reflection->get_binding_index( "shadow_draw_key_sb" );
    if ( binding != u16_max ) {
        descriptors.ssbos.push( { draw_keys_sb[ frame_index ], binding } );
    }

    binding = shader_reflection->get_binding_index( "shadow_draw_meta_sb" );
    if ( binding != u16_max ) {
        descriptors.ssbos.push( { draw_meta_sb[ frame_index ], binding } );
    }

    binding = shader_reflection->get_binding_index( "shadow_meshlet_instances_sb" );
    if ( binding != u16_max ) {
        descriptors.ssbos.push( { meshlet_instances_sb[ frame_index ], binding } );
    }

    binding = shader_reflection->get_binding_index( "shadow_indirect_cmds_sb" );
    if ( binding != u16_max ) {
        descriptors.ssbos.push( { task_indirect_cmds_sb[ frame_index ], binding } );
    }

    binding = shader_reflection->get_binding_index( "shadow_indirect_count_sb" );
    if ( binding != u16_max ) {
        descriptors.ssbos.push( { indirect_draw_count_sb[ frame_index ], binding } );
    }

    binding = shader_reflection->get_binding_index( "shadow_meshlet_alloc_sb" );
    if ( binding != u16_max ) {
        descriptors.ssbos.push( { meshlet_instance_cursor_sb[ frame_index ], binding } );
    }

    binding = shader_reflection->get_binding_index( "shadow_draw_debug_sb" );
    if ( binding != u16_max ) {
        descriptors.ssbos.push( { draw_debug_sb[ frame_index ], binding } );
    }

    binding = shader_reflection->get_binding_index( "shadow_global_debug_sb" );
    if ( binding != u16_max ) {
        descriptors.ssbos.push( { global_debug_info_sb[ frame_index ], binding } );
    }

    binding = shader_reflection->get_binding_index( "shadow_camera_spheres_sb" );
    if ( binding != u16_max ) {
        descriptors.ssbos.push( { pointlight_spheres_sb[ frame_index ], binding } );
    }

    binding = shader_reflection->get_binding_index( "shadow_views_sb" );
    if ( binding != u16_max ) {
        descriptors.ssbos.push( { pointlight_view_projections_sb[ frame_index ], binding } );
    }

    binding = shader_reflection->get_binding_index( "shadow_resolutions" );
    if ( binding != u16_max ) {
        descriptors.ssbos.push( { shadows.shadow_resolutions[ frame_index ], binding } );
    }

    binding = shader_reflection->get_binding_index( "ShadowResolutions" );
    if ( binding != u16_max ) {
        descriptors.ssbos.push( { shadows.shadow_resolutions[ frame_index ], binding } );
    }
}


void PointlightShadowPass2::destroy_gpu_resources( FrameGraphResourceContext& context ) {

    Renderer* renderer = context.renderer;
    GpuDevice& gpu = *renderer->gpu;

    for ( size_t i = 0; i < k_max_frames; i++ ) {
        gpu.destroy_buffer( draw_keys_sb[ i ] );
        gpu.destroy_buffer( draw_meta_sb[ i ] );
        gpu.destroy_buffer( meshlet_instances_sb[ i ] );
        gpu.destroy_buffer( task_indirect_cmds_sb[ i ] );
        gpu.destroy_buffer( indirect_draw_count_sb[ i ] );
        gpu.destroy_buffer( meshlet_instance_cursor_sb[ i ] );
        gpu.destroy_buffer( draw_debug_sb[ i ] );
        gpu.destroy_buffer( global_debug_info_sb[ i ] );
        gpu.destroy_buffer( pointlight_view_projections_sb[ i ] );
        gpu.destroy_buffer( pointlight_spheres_sb[ i ] );

        gpu.destroy_descriptor_set( clear_counters_ds[ i ] );
        gpu.destroy_descriptor_set( build_lists_ds[ i ] );
        gpu.destroy_descriptor_set( build_indirect_cmds_ds[ i ] );
        gpu.destroy_descriptor_set( meshlet_draw_ds[ i ] );
        gpu.destroy_descriptor_set( shadow_resolution_descriptor_set[ i ] );
    }

    for ( u32 mip = 0; mip < k_shadow_mip_count; ++mip ) {
        gpu.destroy_image_view( cubemap_shadow_mip_views[ mip ] );
    }

    gpu.destroy_image( cubemap_shadow_array_image );
    gpu.destroy_image_view( cubemap_shadow_array_image_view );

    gpu.destroy_page_pool( shadow_maps_pool );

    // gpu.destroy_image( cubemap_debug_face_image );
    //gpu.destroy_image_view( cubemap_debug_face_image_view );
}

bool PointlightShadowPass2::update_sparse_binding( FrameGraphRenderContext& context ) {

    GpuDevice& gpu = *context.renderer->gpu;
    RenderScene* scene = context.render_view->scene;

    bool changed = false;

    for ( u32 light = 0; light < scene->active_lights; ++light ) {

        const u32 new_mip = scene->lights[ light ].shadow_mip_level;
        const u32 old_mip = resident_mip_levels[ light ];

        if ( new_mip == old_mip ) {
            continue;
        }

        // Remove old tiled residency.
        if ( old_mip != u32_max ) {

            const u32 resolution = k_shadow_map_resolution >> old_mip;

            for ( u32 face = 0; face < 6; ++face ) {
                gpu.unbind_image_pages( shadow_maps_pool, cubemap_shadow_array_image, 0, 0,
                                        resolution, resolution, light * 6 + face, old_mip );
            }
        }

        // Add new tiled residency.
        // bind_image_pages() already ignores the mip tail.
        const u32 resolution = k_shadow_map_resolution >> new_mip;

        for ( u32 face = 0; face < 6; ++face ) {
            gpu.bind_image_pages( shadow_maps_pool, cubemap_shadow_array_image, 0, 0,
                                  resolution, resolution, light * 6 + face, new_mip );
        }

        resident_mip_levels[ light ] = new_mip;
        changed = true;
    }

    return changed;
}

void PointlightShadowPass2::recreate_lightcount_dependent_resources( FrameGraphResourceContext& context ) {

    Renderer* renderer = context.renderer;
    GpuDevice& gpu = *renderer->gpu;

    const u32 active_lights = context.render_scene->active_lights;

    if ( active_lights == last_active_lights ) {
        return;
    }

    // Destroy resources if they were created
    if ( last_active_lights > 0 ) {

        gpu.destroy_page_pool( shadow_maps_pool );

        gpu.destroy_image_view( cubemap_shadow_array_image_view );

        for ( u32 mip = 0; mip < k_shadow_mip_count; ++mip ) {
            gpu.destroy_image_view( cubemap_shadow_mip_views[ mip ] );
        }

        gpu.destroy_image( cubemap_shadow_array_image );

        shadow_maps_pool = {};
    }

    // Reset resident mip levels to invalid
    for ( u32 i = 0; i < k_num_lights; ++i ) {
        resident_mip_levels[ i ] = u32_max;
    }

    last_active_lights = active_lights;

    // Create new resources
    // Create cube depth array texture
    raptor::ImageCreation texture_creation;
    
    const u32 layer_width = k_shadow_map_resolution;
    const u32 layer_height = k_shadow_map_resolution;
    const u32 layer_count = active_lights * 6;

    VkFormat depth_texture_format = VK_FORMAT_D16_UNORM;

    texture_creation.set_size( layer_width, layer_height, 1 ).set_layers( layer_count ).set_mips( k_shadow_mip_count )
        .set_format_type( depth_texture_format, TextureType::Texture_Cube_Array )
        .set_flags( TextureFlags::RenderTarget_mask | TextureFlags::Sparse_mask ).set_name( "depth_cubemap_array" );
    cubemap_shadow_array_image = gpu.create_image( texture_creation );

    gpu.link_image_sampler( cubemap_shadow_array_image, gpu.global_samplers[ GlobalSamplers::ShadowLinearClamp ] );

    cubemap_shadow_array_image_view = gpu.create_image_view( {
        .parent_image = cubemap_shadow_array_image, .view_type = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY,
        .sub_resource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, k_shadow_mip_count, 0, layer_count }, .name = texture_creation.name } );
    gpu.add_image_view_to_bindless( cubemap_shadow_array_image_view );

    for ( u32 mip = 0; mip < k_shadow_mip_count; ++mip ) {
        cubemap_shadow_mip_views[ mip ] = gpu.create_image_view( {
            .parent_image = cubemap_shadow_array_image,
            .view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
            .sub_resource = { VK_IMAGE_ASPECT_DEPTH_BIT, mip, 1, 0, layer_count },
            .name = texture_creation.name } );
    }

    FrameGraphResource* depth_resource = (FrameGraphResource*)context.frame_graph->get_resource( "point_shadows_depth" );
    RASSERT( depth_resource );

    depth_resource->resource_info.set_external_texture_3d(
        layer_width, layer_height, layer_count,
        depth_texture_format,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
        cubemap_shadow_array_image,
        cubemap_shadow_array_image_view );

    shadow_maps_pool = gpu.allocate_image_pool( cubemap_shadow_array_image, rgiga( 1 ) );

    // Cache shadow depth view index
    context.render_blackboard->point_shadows.cubemap_shadows_index = cubemap_shadow_array_image_view.index();
}

} // namespace raptor
