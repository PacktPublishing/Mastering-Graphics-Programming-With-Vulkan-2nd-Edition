
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
#include "graphics/frame_renderer.hpp"

#include "graphics/render_passes/culling_pass.hpp"
#include "graphics/render_passes/depth_pyramid_pass.hpp"
#include "graphics/render_passes/gbuffer_pass.hpp"
#include "graphics/render_passes/lighting_pass.hpp"
#include "graphics/render_passes/transparent_pass.hpp"
#include "graphics/render_passes/debug_pass.hpp"
#include "graphics/render_passes/pointlight_shadow_pass.hpp"
#include "graphics/render_passes/volumetric_fog_pass.hpp"
#include "graphics/render_passes/temporal_anti_aliasing_pass.hpp"
#include "graphics/render_passes/motion_vector_pass.hpp"

#include "external/glm/mat4x4.hpp"
#include "external/enkiTS/TaskScheduler.h"

#include "foundation/file.hpp"
#include "foundation/time.hpp"
#include "foundation/resource_manager.hpp"

#include "external/imgui/imgui.h"
#include "external/tracy/tracy/Tracy.hpp"

#include <stdio.h>


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
namespace raptor {
namespace chapter5 {

    raptor::DescriptorSetHandle           scene_ds;
    raptor::BufferHandle                  scene_mesh_instances_ssbo;  // Buffer containing all mesh instances data

    struct alignas( 16 ) GpuSceneData {
        glm::mat4                               view_projection;
        glm::vec4                               eye;
        glm::vec4                               light_position;
        f32                                     light_range;
        f32                                     light_intensity;
        f32                                     padding[ 2 ];
    }; // struct GpuFrameData

    //
    //
    struct alignas( 16 ) MeshData {
        glm::mat4       m;
        glm::mat4       inverseM;

        u32         textures[ 4 ]; // diffuse, roughness, normal, occlusion
        glm::vec4   base_color_factor;
        glm::vec4   metallic_roughness_occlusion_factor; // metallic, roughness, occlusion

        float       alpha_cutoff;
        u32         flags;
        u32         padding_[2];
    }; // struct MeshData

    static void upload_material( RenderScene& render_scene, const MeshInstance& mesh_instance, MeshData& mesh_data ) {
        Mesh& mesh = render_scene.meshes[ mesh_instance.mesh_index ];

        mesh_data.textures[ 0 ] = mesh.pbr_material.diffuse_texture_index;
        mesh_data.textures[ 1 ] = mesh.pbr_material.roughness_texture_index;
        mesh_data.textures[ 2 ] = mesh.pbr_material.normal_texture_index;
        mesh_data.textures[ 3 ] = mesh.pbr_material.occlusion_texture_index;
        mesh_data.base_color_factor = mesh.pbr_material.base_color_factor;
        mesh_data.metallic_roughness_occlusion_factor = { mesh.pbr_material.metallic, mesh.pbr_material.roughness, mesh.pbr_material.occlusion, 0.0f };
        mesh_data.alpha_cutoff = mesh.pbr_material.alpha_cutoff;
        mesh_data.flags = mesh.pbr_material.flags;

        mesh_data.m = render_scene.scene_graph->world_matrices[ mesh_instance.scene_graph_node_index ];
        mesh_data.inverseM = glm::inverse( glm::transpose( mesh_data.m ) );
    }

    // Render Passes //////////////////////////////////////////////////////////

    //
    //
    struct HDRColorCopyPass : public FrameGraphRenderPass {
        void declare_frame_graph_node( FrameGraphResourceContext& context ) override {
            FrameGraphBuilder& builder = *context.frame_graph->builder;
            FrameGraphNodeCreation_v2 hdr_copy_node_info {
                .inputs = {
                    /*{
                        .type = FrameGraphResourceType_Texture,
                        .handle = builder.get_output_handle( "transparent_pass", "final" )
                    },*/
                },
                .outputs = {
                    builder.create_output_handle( {
                        .type = FrameGraphResourceType_Attachment,
                        .resource_info{
                            .texture = {
                                .scale_width = 1.0f,
                                .scale_height = 1.0f,
                                .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                .disable_memory_aliasing = true
                            }
                        },
                        .name = "hdr_color_copy",
                    } ),
                },
                .scheduling = { CommandQueueType::Compute, 0 },
                .enabled = true,
                .compute = true,
                .name = k_name,
            };
            context.frame_graph->add_node_v2( hdr_copy_node_info );
        }

        void post_render( FrameGraphRenderContext& context ) override {

            // Avoid copying first frame as source image is not ready
            Renderer* renderer = context.renderer;
            GpuDevice* gpu = renderer->gpu;
            if ( gpu->absolute_frame == 0 ) {
                return;
            }

            CommandBuffer* cb = context.gpu_commands;
            RenderScene* render_scene = context.render_view->scene;

            // Get previous frame final image
            FrameGraphResource* hdr_lighting_resource = context.frame_graph->get_resource( "final" );
            RASSERT( hdr_lighting_resource );
            ImageHandle hdr_lighting_image = hdr_lighting_resource->resource_info.texture.previous_image;
            //Image* hdr_image_data = gpu.get_image( hdr_lighting_resource->resource_info.texture.image );

            FrameGraphResource* hdr_copy_resource = context.frame_graph->get_resource( "hdr_color_copy" );
            RASSERT( hdr_copy_resource );
            ImageHandle hdr_copy_image = hdr_copy_resource->resource_info.texture.image;
            //Image* hdr_copy_image_data = gpu.get_image( hdr_copy_resource->resource_info.texture.image );

            // If we have different queue families, acquire ownership
            if ( gpu->vulkan_compute_queue_family != gpu->vulkan_main_queue_family ) {
                cb->acquire_image_ownership( hdr_lighting_image,
                                             range_color_full(),
                                             gpu->vulkan_main_queue_family, gpu->vulkan_compute_queue_family,
                                             { VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                               VK_ACCESS_2_TRANSFER_READ_BIT,
                                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL } );
                cb->flush_barriers();
            }

            cb->copy_image( hdr_lighting_image, hdr_copy_image,
                ImageSyncState{
                    .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    .access = VK_ACCESS_2_SHADER_READ_BIT,
                    .layout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
                }
            );

            // Done with the texture, release the ownership
            if ( gpu->vulkan_compute_queue_family != gpu->vulkan_main_queue_family ) {
                cb->release_image_ownership( hdr_lighting_image, range_color_full(),
                                             gpu->vulkan_main_queue_family );
                cb->flush_barriers();
            }
            //gpu_commands->copy_image( hdr_lighting_resource->resource_info.texture.image,
            //                          hdr_copy_resource->resource_info.texture.image, RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
        }

        static constexpr cstring    k_name = "hdr_color_copy_pass";
    }; // struct HDRColorCopyPass;

    //
    //
    struct BloomPass : public FrameGraphRenderPass {

        void declare_frame_graph_node( FrameGraphResourceContext& context ) override {
            FrameGraphBuilder& builder = *context.frame_graph->builder;
            FrameGraphNodeCreation_v2 bloom_node_info{
                .inputs = {
                    {
                        .type = FrameGraphResourceType_Texture,
                        .handle = builder.get_output_handle( "hdr_color_copy_pass", "hdr_color_copy" )
                    },
                },
                .outputs = {
                    builder.create_output_handle( {
                        .type = FrameGraphResourceType_Attachment,
                        .resource_info{
                            .external = true
                        },
                        .name = "bloom",
                    } ),
                },
                .scheduling = { CommandQueueType::Compute, 0 },
                .enabled = true,
                .compute = true,
                .name = k_name,
            };
            context.frame_graph->add_node_v2( bloom_node_info );
        }

        void add_ui() override {

        }

        void update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) override {

            Renderer* renderer = context.renderer;

            if ( phase == PipelineUpdatePhase::Destroy ) {
                renderer->destroy_compute_pipeline_state( downsample_pipeline );
                renderer->destroy_compute_pipeline_state( upsample_pipeline );

                return;
            }

            ComputePipelineTransaction transaction( renderer );

            ComputePipelineState& new_downsample_pipeline = transaction.add( downsample_pipeline );
            ComputePipelineState& new_upsample_pipeline = transaction.add( upsample_pipeline );

            renderer->create_compute_pipeline_state( {
                .stages = {
                    {
                        .source_file_path = "glsl/chapter5/post_bloom.glsl",
                        .type = VK_SHADER_STAGE_COMPUTE_BIT,
                    }
                },
                .name = "bloom_downsample" },
                {
                    .name = "bloom_downsample",
                    .render_pass_name = k_name,
                },
                "bloom_downsample", context.frame_graph, new_downsample_pipeline );

            renderer->create_compute_pipeline_state( {
                .stages = {
                    {
                        .source_file_path = "glsl/chapter5/post_bloom.glsl",
                        .type = VK_SHADER_STAGE_COMPUTE_BIT,
                    }
                },
                .name = "bloom_upsample" },
                {
                    .name = "bloom_downsample",
                    .render_pass_name = k_name,
                },
                "bloom_upsample", context.frame_graph, new_upsample_pipeline );

            transaction.commit_or_rollback();
        }

        struct GpuBloomConstants {
            u32         source_texture_index;
            u32         destination_texture_index;

            f32         filter_radius;
            u32         pad111;

            glm::vec2   rcp_source_texture_size;
            glm::vec2   rcp_destination_texture_size;

        };

        void post_render( FrameGraphRenderContext& context ) override {

            // Avoid copying first frame as source image is not ready
            Renderer* renderer = context.renderer;
            GpuDevice* gpu = renderer->gpu;
            if ( gpu->absolute_frame == 0 ) {
                return;
            }

            CommandBuffer* gpu_commands = context.gpu_commands;
            RenderScene* render_scene = context.render_view->scene;

            FrameGraphResource* lighting_resource = context.frame_graph->get_resource( "hdr_color_copy" );
            Image* bloom_image_data = gpu->get_image( bloom_image );

            u32 width = bloom_image_data->width;
            u32 height = bloom_image_data->height;

            // Downsample
            gpu_commands->bind_pipeline( downsample_pipeline.pipeline );

            for ( u32 i = 0; i < bloom_image_views.size; i++ ) {

                u32 mip_w = width >> i;
                u32 mip_h = height >> i;

                //util_add_image_barrier( &gpu, gpu_commands->vk_command_buffer, bloom_image_data->vk_image, RESOURCE_STATE_UNDEFINED, RESOURCE_STATE_UNORDERED_ACCESS, i, 1, false );

                u32 cb_offset = 0;
                GpuBloomConstants* gpu_data = renderer->gpu->dynamic_buffer_allocate<GpuBloomConstants>( &cb_offset );
                if ( gpu_data ) {
                    gpu_data->source_texture_index = ( i == 0 ) ? lighting_resource->resource_info.texture.image_view.index() : bloom_image_views[ i - 1 ].index();
                    gpu_data->destination_texture_index = bloom_image_views[ i ].index();
                    gpu_data->rcp_destination_texture_size = glm::vec2{ 1.f / mip_w, 1.f / mip_h };
                    gpu_data->rcp_source_texture_size = glm::vec2{ 1.f / ( mip_w * 2 ), 1.f / ( mip_h * 2 ) };
                    //rprint( "index %d, offset %d\n", gpu_data->destination_texture_index, cb_offset );
                }

                gpu_commands->bind_descriptor_set( { renderer->gpu->bindless_descriptor_set, descriptor_set },
                                                    { cb_offset } );

                u32 group_x = ( mip_w + 7 ) / 8;
                u32 group_y = ( mip_h + 7 ) / 8;

                gpu_commands->dispatch( group_x, group_y, 1 );

                gpu_commands->barrier_instant_compute_write_to_compute_read();
            }

            // Upsample
            gpu_commands->bind_pipeline( upsample_pipeline.pipeline );

            for ( i32 i = bloom_image_views.size - 1; i > 0; i-- ) {

                //util_add_image_barrier( &gpu, gpu_commands->vk_command_buffer, bloom_image_data->vk_image, RESOURCE_STATE_UNDEFINED, RESOURCE_STATE_UNORDERED_ACCESS, i, 1, false );

                // Mip i
                const u32 src_w = bloom_image_data->width >> i;
                const u32 src_h = bloom_image_data->height >> i;

                // Mip i-1
                const u32 dst_w = bloom_image_data->width >> ( i - 1 );
                const u32 dst_h = bloom_image_data->height >> ( i - 1 );

                u32 cb_offset = 0;
                GpuBloomConstants* gpu_data = renderer->gpu->dynamic_buffer_allocate<GpuBloomConstants>( &cb_offset );
                if ( gpu_data ) {
                    gpu_data->source_texture_index = bloom_image_views[ i ].index();
                    gpu_data->destination_texture_index = bloom_image_views[ i - 1 ].index();
                    gpu_data->rcp_source_texture_size = { 1.f / src_w, 1.f / src_h };
                    gpu_data->rcp_destination_texture_size = { 1.f / dst_w, 1.f / dst_h };
                    gpu_data->filter_radius = 0.005f;
                }

                gpu_commands->bind_descriptor_set( { renderer->gpu->bindless_descriptor_set, descriptor_set },
                                                    { cb_offset } );

                u32 group_x = ( dst_w + 7 ) / 8;
                u32 group_y = ( dst_h + 7 ) / 8;

                gpu_commands->dispatch( group_x, group_y, 1 );

                gpu_commands->barrier_instant_compute_write_to_compute_read();
            }
        }

        // Utility methods ///////////////////////////////////////////////
        u32 calculate_mip_levels( u32 width, u32 height ) {
            u32 mip_levels = 0;
            while ( width >= 16 && height >= 16 ) {
                mip_levels++;
                width /= 2;
                height /= 2;
            }
            return mip_levels;
        }

        void create_image_views_for_mipmaps( GpuDevice& gpu, FrameGraph* frame_graph, u32 mip_levels ) {
            // Create views for each mipmap
            ImageViewCreation image_view_creation{
                .parent_image = bloom_image,
                .view_type = VK_IMAGE_VIEW_TYPE_2D, };
            for ( u32 i = 0; i < mip_levels; i++ ) {
                image_view_creation.sub_resource = { VK_IMAGE_ASPECT_COLOR_BIT, i, 1, 0, 1 };
                bloom_image_views.push( gpu.create_image_view( image_view_creation ) );
                gpu.add_image_view_to_bindless( bloom_image_views[ i ] );
            }
        }

        void set_external_framegraph_resource( FrameGraph* frame_graph, GpuDevice& gpu ) {
            FrameGraphResource* bloom_tex = frame_graph->get_resource( "bloom" );
            RASSERT( bloom_tex );

            bloom_tex->resource_info.set_external_texture_2d(
                gpu.get_image( bloom_image )->width,
                gpu.get_image( bloom_image )->height,
                VK_FORMAT_R16G16B16A16_SFLOAT,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                bloom_image,
                bloom_image_views[ 0 ] );
        }

        void on_resize( FrameGraphResourceContext& context, u32 new_width, u32 new_height ) override {

            Renderer* renderer = context.renderer;
            FrameGraph* frame_graph = context.frame_graph;
            GpuDevice& gpu = *renderer->gpu;

            FrameGraphNode* node = frame_graph->get_node( k_name );
            if ( node == nullptr ) {
                RASSERT( false );
                return;
            }

            // Remove old image views from bindless and destroy them
            // NOTE: image can be just re-created, but we may have different mip levels and
            // thus different image views.
            for ( u32 i = 0; i < bloom_image_views.size; i++ ) {
                gpu.remove_image_view_from_bindless( bloom_image_views[ i ] );
                gpu.destroy_image_view( bloom_image_views[ i ] );
            }

            bloom_image_views.clear();

            // Resize image
            u32 mip_levels = calculate_mip_levels( new_width / 2, new_height / 2 );
            gpu.resize_image( bloom_image, new_width / 2, new_height / 2, mip_levels );

            create_image_views_for_mipmaps( gpu, frame_graph, mip_levels );
            set_external_framegraph_resource( frame_graph, gpu );
        }

        void create_gpu_resources( FrameGraphResourceContext& context ) override {

            Renderer* renderer = context.renderer;
            GpuDevice& gpu = *renderer->gpu;
            RenderBlackboard& render_blackboard = *context.render_blackboard;

            FlatHashMapIterator it = renderer->resource_cache.pipelines.find( hash_calculate( "bloom_downsample" ) );
            RASSERT( it.is_valid() );

            PipelineHandle pipeline = renderer->resource_cache.pipelines.get( it );
            DescriptorSetLayoutHandle layout_handle = gpu.get_descriptor_set_layout( pipeline, k_material_descriptor_set_index );
            ShaderReflectionInfo* reflection_info = renderer->get_shader_reflection( pipeline );

            FrameGraphResource* lighting_resource = context.frame_graph->get_resource( "final" );

            u32 mip_levels = calculate_mip_levels( render_blackboard.render_width / 2, render_blackboard.render_height / 2 );

            // Create image
            ImageCreation image_creation{ };
            image_creation.set_format_type( VK_FORMAT_R16G16B16A16_SFLOAT, TextureType::Enum::Texture2D )
                .set_flags( TextureFlags::Compute_mask )
                .set_size( render_blackboard.render_width / 2, render_blackboard.render_height / 2, 1 )
                .set_name( "bloom" ).set_mips( mip_levels );

            bloom_image = gpu.create_image( image_creation );

            RASSERT( bloom_image_views.size == 0 );
            create_image_views_for_mipmaps( gpu, context.frame_graph, mip_levels );
            set_external_framegraph_resource( context.frame_graph, gpu );

            descriptor_set = gpu.create_descriptor_set( {
                .dynamic_buffers = { {.binding = renderer->get_binding_index( reflection_info, "bloom_locals" ), .size = sizeof( GpuBloomConstants ) }},
                .layout = layout_handle } );
        }

        void destroy_gpu_resources( FrameGraphResourceContext& context ) override {
            Renderer* renderer = context.renderer;

            renderer->gpu->destroy_image( bloom_image );

            for ( u32 i = 0; i < bloom_image_views.size; i++ ) {
                renderer->gpu->destroy_image_view( bloom_image_views[ i ] );
            }

            renderer->gpu->destroy_descriptor_set( descriptor_set );
        }

        ImageHandle                 bloom_image;
        StaticArray<ImageViewHandle, 16> bloom_image_views;

        ComputePipelineState        downsample_pipeline;
        ComputePipelineState        upsample_pipeline;

        DescriptorSetHandle         descriptor_set;

        static constexpr cstring    k_name = "bloom_pass";
    }; // struct BloomPass

}; // chapter5
}; // raptor

//
//
int main( int argc, char** argv ) {

    if ( argc < 2 ) {
        printf( "Usage: chapter5 [path to glTF model]\n");
        InjectDefault3DModel();
    }

    using namespace raptor;
    // Init services
    MemoryServiceConfiguration memory_configuration;
    memory_configuration.maximum_dynamic_size = rgiga( 2ull );

    MemoryService::instance()->init( &memory_configuration );
    Allocator* allocator = &MemoryService::instance()->system_allocator;

    enki::TaskSchedulerConfig config;
    // In this example we create more threads than the hardware can run,
    // because the IO thread will spend most of it's time idle or blocked
    // and therefore not scheduled for CPU time by the OS
    config.numTaskThreadsToCreate += 1;
    enki::TaskScheduler task_scheduler;

    task_scheduler.Initialize( config );

    // window
    WindowConfiguration wconf{ 1280, 800, "Chapter 5: GPU-Driven Rendering", &MemoryService::instance()->system_allocator};
    raptor::Window window;
    window.init( &wconf );

    InputService input;
    input.init( allocator );

    // Callback register: input needs to react to OS messages.
    window.register_os_messages_callback( input_os_messages_callback, &input );

    // graphics
    GpuDeviceCreation dc;
    dc.debug_options.set_default();
    dc.enable_bindless = true;
    dc.set_window( window.width, window.height, window.platform_handle ).set_allocator( &MemoryService::instance()->system_allocator )
      .set_num_threads( task_scheduler.GetNumTaskThreads() );
    dc.resource_pool_creation.buffers = 1024;
    dc.descriptor_pool_creation.storage_buffer = 512;

    GpuDevice gpu;
    gpu.init( dc );

    ResourceManager rm;
    rm.init( allocator, nullptr );

    GpuVisualProfiler gpu_profiler;
    gpu_profiler.init( allocator, gpu.gpu_timestamp_frequency, 100, dc.gpu_time_queries_per_frame );

    RendererResourcePoolCreation rrpc{ };
    rrpc.buffers = dc.resource_pool_creation.buffers;

    Renderer renderer;
    renderer.init( { &gpu, allocator, rrpc } );
    renderer.set_loaders( &rm );

    ImGuiService* imgui = ImGuiService::instance();
    ImGuiServiceConfiguration imgui_config{ &gpu, &renderer, window.platform_handle };
    imgui->init( &imgui_config );

    GameCamera game_camera;
    game_camera.camera.init_perpective( 0.1f, 1000.f, 60.f, wconf.width * 1.f / wconf.height );
    game_camera.init( true, 20.f, 6.f, 0.1f );

    time_service_init();

    SceneGraph scene_graph;
    scene_graph.init( allocator, 4 );

    RenderScene render_scene{ };
    render_scene.init( &scene_graph, allocator, &renderer );

    FrameGraphBuilder frame_graph_builder;
    frame_graph_builder.init( &gpu );

    FrameGraph frame_graph;
    frame_graph.init( &frame_graph_builder );

    FrameRenderer frame_renderer;
    frame_renderer.init( allocator, &renderer, &frame_graph, &scene_graph, &render_scene );
    frame_renderer.set_upscale_settings( { .render_scale = 0.66f, .enabled = false } );
    frame_renderer.calculate_resolution_info( gpu.swapchain_width, gpu.swapchain_height );

    frame_graph_builder.set_resolution_info( &frame_renderer.resolution_info );

    // NEW: rendering features
    // Add locally declared rendering features to the frame renderer to enable them
    GpuCullingRenderingFeature gpu_culling_feature;
    frame_renderer.gpu_culling = &gpu_culling_feature;

    MeshesRenderingFeature meshes;
    frame_renderer.meshes = &meshes;

    MeshletsRenderingFeature meshlets_feature;
    frame_renderer.meshlets = &meshlets_feature;

    LightingRenderingFeature lighting_feature;
    frame_renderer.lighting = &lighting_feature;

    PostProcessRenderingFeature post_feature;
    frame_renderer.post = &post_feature;

    DebugDrawRenderingFeature debug_draw_feature;
    frame_renderer.debug_draw = &debug_draw_feature;

    // Explicitly disable shadows
    frame_renderer.render_config.shadows.disable_shadows = true;

    // Debug image views
    ImageViewDebugger image_view_debugger;
    image_view_debugger.init( allocator, &gpu );

    ArenaAllocator temp_frame_allocator;
    temp_frame_allocator.init( rmega( 4 ) );

    // Load frame graph
    {
        // Special node to determine final output
        FrameGraphNodeCreation present_node_info {
            .name = "present",
        };
        frame_graph.add_node( present_node_info );

        //frame_renderer.add_render_pass( "volumetric_fog_pass", rnewa( VolumetricFogPass, allocator, 64 ) );
        //frame_renderer.add_render_pass( "point_shadows_pass", rnewa( PointlightShadowPass2, allocator, 64 ) );
        frame_renderer.add_render_pass( "mesh_occlusion_early_pass", rnewa( CullingEarlyPass, allocator, 64 ) );
        frame_renderer.add_render_pass( "gbuffer_pass_early", rnewa( GBufferPass, allocator, 64 ) );
        frame_renderer.add_render_pass( "depth_pyramid_pass", rnewa( DepthPyramidPass, allocator, 64 ) );
        frame_renderer.add_render_pass( "mesh_occlusion_late_pass", rnewa( CullingLatePass, allocator, 64 ) );
        frame_renderer.add_render_pass( "gbuffer_pass_late", rnewa( LateGBufferPass, allocator, 64 ) );
        frame_renderer.add_render_pass( "lighting_pass", rnewa( LightingPass, allocator, 64 ) );
        frame_renderer.add_render_pass( "transparent_pass", rnewa( TransparentPass, allocator, 64 ) );
        frame_renderer.add_render_pass( "debug_draw_pass", rnewa( DebugDrawPass, allocator, 64 ) );
        frame_renderer.add_render_pass( "hdr_color_copy_pass", rnewa( chapter5::HDRColorCopyPass, allocator, 64 ) );
        frame_renderer.add_render_pass( "bloom_pass", rnewa( chapter5::BloomPass, allocator, 64 ) );
        frame_renderer.add_render_pass( "motion_vector_pass", rnewa( MotionVectorPass, allocator, 64 ) );
        //frame_renderer.add_render_pass( "temporal_anti_aliasing_pass", rnewa( TemporalAntiAliasingPass, allocator, 64 ) );

        frame_renderer.declare_frame_graph_structure( frame_graph );

        frame_graph.compile();

        /*FrameGraphNode* point_shadows_pass_node = frame_graph.get_node( "point_shadows_pass" );
        if ( point_shadows_pass_node ) {
            point_shadows_pass_node->render_pass_output.reset().depth( VK_FORMAT_D16_UNORM, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL );
        }*/

        frame_renderer.compile_passes_psos();
    }

    Directory cwd{ };
    directory_current(&cwd);

    ArenaAllocator temp_allocator;
    temp_allocator.init( rmega( 4 ) );

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

        render_scene.add_and_load_model( file_name, file_base_path, &temp_allocator );
    }

    // Initial matrix update to have correct world matrices
    scene_graph.update_matrices();

    temp_allocator.shutdown();

    // Create scene resources
    {
        // Create scene mesh instances buffer with all material data
        chapter5::scene_mesh_instances_ssbo = gpu.create_buffer( {
            .size = sizeof( chapter5::MeshData ) * render_scene.mesh_instances.size,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
            .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .name = "mesh_instances_sb" } );

        FlatHashMapIterator it = renderer.resource_cache.pipelines.find( hash_calculate( "gbuffer_culling" ) );
        RASSERT( it.is_valid() );

        PipelineHandle gbuffer_pipeline = renderer.resource_cache.pipelines.get( it );
        DescriptorSetLayoutHandle gbuffer_layout_handle = gpu.get_descriptor_set_layout( gbuffer_pipeline, k_material_descriptor_set_index );
        ShaderReflectionInfo* gbuffer_reflection_info = renderer.get_shader_reflection( gbuffer_pipeline );


        ShaderReflectionInfo* reflection_info = gbuffer_reflection_info;

        DescriptorSetCreation ds_creation{ };
        ds_creation.layout = gbuffer_layout_handle;
        ds_creation.ssbos = {
            {.buffer = chapter5::scene_mesh_instances_ssbo,
             .binding = renderer.get_binding_index( reflection_info, "MeshData" ) },
        };
        ds_creation.dynamic_buffers = {
            {.binding = renderer.get_binding_index( reflection_info, "LocalConstants" ), .size = sizeof( GpuFrameData ) },
        };

        //chapter5::scene_ds = gpu.create_descriptor_set( ds_creation );
    }

    // Calculate main view render items
    {
        RenderView& main_view = frame_renderer.main_view;
        main_view.opaque_items.init( allocator, 16 );
        main_view.transparent_items.init( allocator, 16 );
        main_view.scene = &render_scene;

        // Iterate over mesh instances and create render items
        for ( u32 i = 0; i < render_scene.mesh_instances.size; i++ ) {
            MeshInstance& mesh_instance = render_scene.mesh_instances[ i ];
            Mesh& mesh = render_scene.meshes[ mesh_instance.mesh_index ];

            RenderItem render_mesh{ };
            render_mesh.mesh_instance = &mesh_instance;

            if ( mesh.is_transparent() ) {
                main_view.transparent_items.push( render_mesh );
            } else {
                main_view.opaque_items.push( render_mesh );
            }
        }
    }

    // NOTE(marco): restore working directory
    directory_change( cwd.path );

    ArenaAllocator* scratch_allocator = MemoryService::instance()->get_thread_allocator();
    frame_renderer.create_resources( scratch_allocator );

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
    async_load_task.async_loader = renderer.async_loader;
    task_scheduler.AddPinnedTask( &async_load_task );

    i64 begin_frame_tick = time_now();
    i64 absolute_begin_frame_tick = begin_frame_tick;

    glm::vec3 light_position = glm::vec3{ 0.0f, 4.0f, 0.0f };

    float light_radius = 20.0f;
    float light_intensity = 80.0f;
    glm::vec2 last_clicked_position = glm::vec2{ 1280 / 2.0f, 800 / 2.0f };

    bool update_mesh_data = true;

    // Setup common options
    //frame_renderer.render_config.lighting.disable_shadows = true;

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

            static bool checksz = true;
            if ( renderer.async_loader->file_load_requests.size == 0 && checksz ) {
                checksz = false;
                rprint( "Finished uploading textures in %f seconds\n", time_from_seconds( absolute_begin_frame_tick ) );
            }
        }

        window.handle_os_messages();
        input.new_frame();

        if ( window.resized ) {
            gpu.resize( window.width, window.height );
            window.resized = false;
            frame_renderer.on_resize( gpu, window.width, window.height );

            frame_graph.on_resize( &renderer, &frame_renderer.render_blackboard,
                                   &frame_renderer.render_config,
                                   frame_renderer.resolution_info.render_width,
                                   frame_renderer.resolution_info.render_height );
            game_camera.camera.set_aspect_ratio( ( f32 )window.width / ( f32 )window.height );
        }
        // This MUST be AFTER os messages!
        imgui->new_frame();

        const i64 current_tick = time_now();
        f32 delta_time = ( f32 )time_delta_seconds( begin_frame_tick, current_tick );
        begin_frame_tick = current_tick;

        input.update( delta_time );
        game_camera.update( &input, window.width, window.height, delta_time );
        window.center_mouse( game_camera.mouse_dragging );

        {
            ZoneScopedN( "ImGui Recording" );

            if ( ImGui::Begin( "Raptor ImGui" ) ) {
                ImGui::Checkbox( "Use Slang Shaders", &frame_renderer.render_config.use_slang_shaders );
                ImGui::InputFloat( "Scene global scale", &frame_renderer.render_config.global_scale, 0.001f );
                ImGui::InputFloat3( "Camera position", &game_camera.camera.position[0] );
                ImGui::InputFloat3( "Camera target movement", &game_camera.target_movement[0] );
                ImGui::Separator();

                static bool fullscreen = false;
                if ( ImGui::Checkbox( "Fullscreen", &fullscreen ) ) {
                    window.set_fullscreen( fullscreen );
                }

                static i32 present_mode = renderer.gpu->present_mode;
                if ( ImGui::Combo( "Present Mode", &present_mode, raptor::PresentMode::s_value_names, raptor::PresentMode::Count ) ) {
                    renderer.set_presentation_mode( ( raptor::PresentMode::Enum )present_mode );
                }

                if ( ImGui::CollapsingHeader( "Lights" ) ) {
                    static u32 light_to_debug = 0;
                    ImGui::SliderUint( "Active Lights", &render_scene.active_lights, 1, k_num_lights - 1 );
                    ImGui::SliderUint( "Light Index", &light_to_debug, 0, render_scene.active_lights - 1 );

                    Light& selected_light = render_scene.lights[ light_to_debug ];
                    ImGui::SliderFloat3( "Light position", &selected_light.world_position[0], -10.f, 10.f, "%2.3f" );
                    ImGui::SliderFloat( "Light radius", &selected_light.radius, 0.01f, 30.f, "%2.3f" );
                    ImGui::SliderFloat( "Light intensity", &selected_light.intensity, 0.01f, 30.f, "%2.3f" );

                    f32 light_color[ 3 ] = { selected_light.color.x, selected_light.color.y, selected_light.color.z };
                    ImGui::ColorEdit3( "Light color", light_color );
                    selected_light.color = { light_color[ 0 ], light_color[ 1 ], light_color[ 2 ] };

                    ImGui::Checkbox( "Light Edit Debug Draws", &frame_renderer.render_config.show_light_edit_debug_draws );

                    if ( frame_renderer.render_config.show_light_edit_debug_draws ) {
                        const Light& light = render_scene.lights[ light_to_debug ];
                        debug_draw_feature.point_light_wire( light.world_position, light.radius, Color::white() );
                    }
                }

                if ( ImGui::Button( "Reload Pipelines" ) ) {

                    frame_renderer.reload_psos();
                }

                frame_renderer.render_config.debug_draw.mesh_instances_count = render_scene.mesh_instances.size;
                frame_renderer.render_config.debug_draw.draw_imgui();
                frame_renderer.render_config.gpu_culling.draw_imgui();
                frame_renderer.render_config.meshlets.draw_imgui();
                frame_renderer.render_config.post.draw_imgui();
                frame_renderer.render_config.volumetric_fog.draw_imgui();
                frame_renderer.render_config.taa.draw_imgui();

                if ( frame_renderer.render_config.debug_draw.inspect_mesh_instance ) {
                    MeshInstance& mi = render_scene.mesh_instances[ frame_renderer.render_config.debug_draw.mesh_instance_index ];
                    Mesh& mesh = render_scene.meshes[ mi.mesh_index ];

                    glm::mat4 world = scene_graph.world_matrices[ mi.scene_graph_node_index ];
                    glm::vec4 world_min = world * glm::vec4( mesh.aabb[ 0 ], 1.f );
                    glm::vec4 world_max = world * glm::vec4( mesh.aabb[ 1 ], 1.f );
                    f32 scale = extract_scale( world ).x;

                    debug_draw_feature.aabb( world_min, world_max, Color::white() );

                    glm::vec4 sphere_center = glm::vec4{ mesh.bounding_sphere.x, mesh.bounding_sphere.y, mesh.bounding_sphere.z, 1.f };
                    debug_draw_feature.sphere_wire( world* sphere_center, mesh.bounding_sphere.w* scale, Color::blue() );

                    for ( u32 i = 0; i < mesh.meshlet_count; i++ ) {
                        const GpuMeshlet& meshlet = render_scene.meshlets[ mesh.meshlet_offset + i ];
                        glm::vec4 world_center = world * glm::vec4( meshlet.center, 1.f );

                        f32 world_radius = scale * meshlet.radius;

                        debug_draw_feature.sphere_wire( world_center, world_radius, Color::green() );
                    }
                }
            }
            ImGui::End();

            if ( ImGui::Begin( "GPU" ) ) {
                renderer.imgui_draw();

                ImGui::Separator();
                gpu_profiler.imgui_draw();

            }
            ImGui::End();

            if ( ImGui::Begin( "Scene" ) ) {
                scene_graph.debug_ui();
            }
            ImGui::End();

            if ( ImGui::Begin( "Frame Graph Debug" ) ) {

                frame_graph.add_ui();
                frame_graph.debug_ui();

                static i32 face_to = 0;
                ImGui::SliderInt( "Face", &face_to, 0, 5 );
                frame_renderer.render_config.cubemap_debug_face_index = (u32)face_to;
                ImGui::Checkbox( "Cubemap face enabled", &frame_renderer.render_config.cubemap_face_debug_enabled );

                image_view_debugger.debug_ui();
            }
            ImGui::End();

            MemoryService::instance()->imgui_draw();
        }

        {
            ZoneScopedN( "AnimationsUpdate" );
            static f32 animation_speed_multiplier = 1.f;
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
            ZoneScopedN( "UniformBufferUpdate" );

            if ( ( input.is_mouse_clicked( MOUSE_BUTTONS_LEFT ) || input.is_mouse_dragging( MOUSE_BUTTONS_LEFT ) ) && !ImGui::IsAnyItemHovered() ) {
                last_clicked_position = glm::vec2{ input.mouse_position.x, input.mouse_position.y };
            }

            frame_renderer.upload_gpu_data( game_camera, last_clicked_position, &temp_frame_allocator );

            // Upload per-mesh data
            if ( update_mesh_data )
            {
                update_mesh_data = false;

                Buffer* mesh_buffer = gpu.get_buffer( chapter5::scene_mesh_instances_ssbo );
                RASSERT( mesh_buffer );
                chapter5::MeshData* mesh_data = ( chapter5::MeshData* )mesh_buffer->mapped_data;
                if ( mesh_data ) {
                    for ( u32 i = 0; i < frame_renderer.main_view.opaque_items.size; ++i ) {
                        RenderItem& render_mesh = frame_renderer.main_view.opaque_items[ i ];
                        const u32 mesh_instance_index = render_mesh.mesh_instance->gpu_mesh_instance_index;
                        upload_material( render_scene, *render_mesh.mesh_instance, mesh_data[ mesh_instance_index ] );
                    }

                    for ( u32 i = 0; i < frame_renderer.main_view.transparent_items.size; ++i ) {
                        RenderItem& render_mesh = frame_renderer.main_view.transparent_items[ i ];
                        const u32 mesh_instance_index = render_mesh.mesh_instance->gpu_mesh_instance_index;
                        upload_material( render_scene, *render_mesh.mesh_instance, mesh_data[ mesh_instance_index ] );

                    }
                    gpu.flush_buffer( chapter5::scene_mesh_instances_ssbo, 0, sizeof( chapter5::MeshData ) * render_scene.mesh_instances.size );
                }
            }

            imgui->finalize_draw_data();
        }

        if ( !window.minimized ) {
            DrawTask draw_task;

            draw_task.init( renderer.gpu, &frame_graph, &renderer, imgui, &gpu_profiler, &render_scene, &frame_renderer );
            task_scheduler.AddTaskSetToPipe( &draw_task );

            task_scheduler.WaitforTask( &draw_task );
            frame_graph.update_persistent_resources_handles();

            // Avoid using the same command buffer
            CommandBuffer* image_upload_cb = renderer.add_image_finalize_commands( ( draw_task.thread_id + 1 ) % task_scheduler.GetNumTaskThreads() );

            gpu.update_bindless_resources();
            bool wait_for_sparse_semaphore = gpu.update_sparse_resources();

            // NEW: use new queue submit method
            StaticArray<CommandBuffer*, 8> cbs;
            StaticArray<VkSemaphoreSubmitInfoKHR, 4> waits;
            StaticArray<VkSemaphoreSubmitInfoKHR, 4> signals;

            // Explicit build of wait and signal infos
            // First compute the frame limiter value: the minimum value
            // the timeline semaphore must have to allow rendering of this frame
            const u64 frame_limiter_value = gpu.compute_frame_limiter_wait_value();
            const u64 next_frame_value = gpu.absolute_frame + 1;

            // Async submit //////////////////////////////////////////////
            u64 compute_value = ++gpu.last_compute_semaphore_value;
            cbs.clear();
            cbs.push( frame_graph.get_command_buffer_from_batch( CommandQueueType::Compute, 0 ) );

            // WAITS
            // Do not wait
            waits.clear();
            const u64 gfx_done_value = gpu.absolute_frame; // because gfx signals next_frame_value

            // Wait for graphics, first stage will be transfer used in vkCmdCopyImage
            waits.push( GpuDevice::build_semaphore_submit( gpu.vulkan_graphics_timeline_semaphore,
                                                           gfx_done_value,
                                                           VK_PIPELINE_STAGE_2_TRANSFER_BIT ) );

            if ( wait_for_sparse_semaphore ) {
                waits.push( GpuDevice::build_semaphore_submit( gpu.vulkan_bind_binary_semaphore,
                                                               0,
                                                               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT_KHR ) );
            }

            // SIGNALS
            // Signal compute, last texture will be written by a compute shader
            signals.clear();
            signals.push( GpuDevice::build_semaphore_submit( gpu.vulkan_compute_timeline_semaphore,
                                                             compute_value,
                                                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT ) );

            gpu.queue_submit( CommandQueueType::Compute, cbs.as_span(),
                              waits.as_span(),
                              signals.as_span() );

            // Gfx first submit: depth and gbuffer ///////////////////////
            cbs.clear();
            waits.clear();
            signals.clear();

            // Wait for the begin of the frame delimiters
            // First wait: image acquired semaphore, 0 because is a binary semaphore.
            waits.push( GpuDevice::build_semaphore_submit( gpu.get_current_image_acquired_semaphore(),
                                                           0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR ) );

            // Second wait: frame limiter timeline semaphore, waiting on the frame limiter value
            waits.push( GpuDevice::build_semaphore_submit( gpu.vulkan_graphics_timeline_semaphore,
                                                           frame_limiter_value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0 ) );

            cbs.push( frame_graph.get_command_buffer_from_batch( CommandQueueType::Graphics, 0 ) );

            // For now, this still does not work.
            gpu.queue_submit( CommandQueueType::Graphics, cbs.as_span(),
                              waits.as_span(),
                              signals.as_span() );

            // Graphics submit (all frame graph + post, can be divided better)
            cbs.clear();
            waits.clear();
            signals.clear();

            cbs.push( frame_graph.get_command_buffer_from_batch( CommandQueueType::Graphics, 1 ) );
            cbs.push( draw_task.gfx_cb );
            // Add image upload commands if any
            if ( image_upload_cb ) {
                cbs.push( image_upload_cb );
            }

            // WAITS
            // Wait: compute work, will be used by fragment program
            waits.push( GpuDevice::build_semaphore_submit( gpu.vulkan_compute_timeline_semaphore, compute_value,
                                                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT ) );

            // SIGNALS
            // First signal: render complete semaphore, 0 because is a binary semaphore.
            signals.push( GpuDevice::build_semaphore_submit( gpu.get_current_render_complete_semaphore(),
                                                             0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR ) );

            // Second signal: frame timeline semaphore, next frame value
            signals.push( GpuDevice::build_semaphore_submit( gpu.vulkan_graphics_timeline_semaphore,
                                                             next_frame_value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0 ) );

            gpu.queue_submit( CommandQueueType::Graphics, cbs.as_span(),
                              waits.as_span(),
                              signals.as_span() );

            gpu.present();
            gpu.resolve_timestamps();
            gpu.process_pending_resource_deletion();
        } else {
            ImGui::Render();
        }

        FrameMark;
    }

    run_pinned_task.execute = false;
    async_load_task.execute = false;

    task_scheduler.WaitforAllAndShutdown();

    vkDeviceWaitIdle( gpu.vulkan_device );

    image_view_debugger.shutdown();
    imgui->shutdown();
    gpu_profiler.shutdown();
    scene_graph.shutdown();

    gpu.destroy_descriptor_set( chapter5::scene_ds );
    gpu.destroy_buffer( chapter5::scene_mesh_instances_ssbo );

    frame_renderer.main_view.opaque_items.shutdown();
    frame_renderer.main_view.transparent_items.shutdown();

    frame_renderer.shutdown();

    frame_graph.shutdown();
    frame_graph_builder.shutdown();

    render_scene.shutdown( &renderer );

    rm.shutdown();
    renderer.shutdown();

    input.shutdown();
    window.unregister_os_messages_callback( input_os_messages_callback );
    window.shutdown();

    MemoryService::instance()->shutdown();

    return 0;
}
