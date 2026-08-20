
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
#include "graphics/frame_renderer.hpp"
#include "graphics/asynchronous_loader.hpp"
#include "graphics/scene_graph.hpp"

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
namespace chapter3 {

    FlatHashMap< u64, PipelineHandle >    pipeline_cache;

    raptor::DescriptorSetHandle           scene_ds;
    raptor::BufferHandle                  scene_mesh_instances_ssbo;  // Buffer containing all mesh instances data
    u32                                   scene_cb_offset;

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
        glm::mat4   m;
        glm::mat4   inverseM;

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
    struct DepthPrePass : public FrameGraphRenderPass {
        void declare_frame_graph_node( FrameGraphResourceContext& context ) override {
            FrameGraphBuilder& builder = *context.frame_graph->builder;

            context.frame_graph->add_node_v2( {
                .outputs = {
                    builder.create_output_handle( {
                        .type = FrameGraphResourceType_Attachment,
                        .resource_info{
                            .texture = {
                                .scale_width = 1.0f,
                                .scale_height = 1.0f,
                                .format = VK_FORMAT_D32_SFLOAT,
                                .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                .clear_values = { 1.0f, 0.0f, 0.0f, 0.0f }
                            }
                        },
                        .name = "depth",
                    } ),
                },
                .enabled = true,
                .name = "depth_prepass" } );
        }

        void update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) override {

            Renderer* renderer = context.renderer;

            if ( phase == PipelineUpdatePhase::Destroy ) {

                renderer->gpu->destroy_pipeline_layout( pipeline_layout );
                renderer->destroy_shader_state( depth_prepass_shader );

                return;
            }

            ShaderReflection depth_prepass_reflection;
            depth_prepass_shader = renderer->create_shader_state( {
                .stages = {
                    {
                        .source_file_path = "glsl/chapter3/mesh_depth.glsl",
                        .headers = {
                            "glsl/platform.h", "glsl/chapter3/scene_mesh_data.h"
                        },
                        .type = VK_SHADER_STAGE_VERTEX_BIT,
                    },
                },
                .name = "depth_prepass" },
                "depth_prepass", &depth_prepass_reflection );

            pipeline_layout = renderer->create_pipeline_layout( depth_prepass_reflection );

            PipelineCreation depth_prepass_creation = {
                .rasterization = {
                    .cull_mode = VK_CULL_MODE_BACK_BIT
                },
                .depth_stencil = {
                    .depth_comparison = VK_COMPARE_OP_LESS_OR_EQUAL,
                    .depth_enable = 1,
                    .depth_write_enable = 1,
                    .stencil_enable = 0,
                },
                .vertex_input = {
                    .bindings = {
                        {.binding = 0, .stride = 12, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX },
                    },
                    .attributes = {
                        {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,    .offset = 0 },
                    }
                },
                .shader = depth_prepass_shader,
                .layout = pipeline_layout,
                .name = "depth_prepass",
                .render_pass_name = "depth_prepass",
            };

            context.frame_graph->cache_render_pass_output( "depth_prepass", renderer->gpu, depth_prepass_creation.render_pass_output, false );

            PipelineHandle depth_prepass_pipeline = renderer->create_pipeline( depth_prepass_reflection, depth_prepass_creation );
            chapter3::pipeline_cache.insert( hash_calculate( "depth_prepass" ), depth_prepass_pipeline );
        }

        void render( FrameGraphRenderContext& context ) override {

            CommandBuffer* gpu_commands = context.gpu_commands;
            RenderView* render_view = context.render_view;
            RenderScene* render_scene = render_view->scene;
            Renderer* renderer = context.renderer;
            RenderBlackboard& render_blackboard = *context.render_blackboard;

            FlatHashMapIterator it = pipeline_cache.find( hash_calculate( "depth_prepass" ) );
            RASSERT( it.is_valid() );

            PipelineHandle pipeline = pipeline_cache.get( it );
            gpu_commands->bind_pipeline( pipeline );

            for ( RenderItem& render_mesh : render_view->opaque_items ) {
                MeshInstance* mesh_instance = render_mesh.mesh_instance;
                Mesh& mesh = render_scene->meshes[ mesh_instance->mesh_index ];

                gpu_commands->bind_vertex_buffer( mesh.position_buffer, 0, mesh.position_offset );

                gpu_commands->bind_index_buffer( mesh.index_buffer, mesh.index_offset_bytes, mesh.index_type );

                gpu_commands->bind_descriptor_set( { renderer->gpu->bindless_descriptor_set, chapter3::scene_ds }, { chapter3::scene_cb_offset } );

                gpu_commands->draw_indexed( raptor::TopologyType::Triangle, mesh.index_count, 1, 0, 0, mesh_instance->gpu_mesh_instance_index );
            }
        }

        ShaderStateHandle           depth_prepass_shader;
        PipelineLayoutHandle        pipeline_layout;
    }; // struct DepthPrePass

    //
    //
    struct GBufferPass : public FrameGraphRenderPass {
        void declare_frame_graph_node( FrameGraphResourceContext& context ) override {
            FrameGraphBuilder& builder = *context.frame_graph->builder;

            context.frame_graph->add_node_v2( {
                .inputs = {
                    {
                        .type = FrameGraphResourceType_Attachment,
                        .handle = builder.get_output_handle( "depth_prepass", "depth" )
                    }
                },
                .outputs = {
                    builder.create_output_handle( {
                        .type = FrameGraphResourceType_Attachment,
                        .resource_info{
                            .texture = {
                                .scale_width = 1.0f,
                                .scale_height = 1.0f,
                                .format = VK_FORMAT_B8G8R8A8_UNORM,
                                .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR
                            }
                        },
                        .name = "gbuffer_colour",
                    } ),
                    builder.create_output_handle( {
                        .type = FrameGraphResourceType_Attachment,
                        .resource_info{
                            .texture = {
                                .scale_width = 1.0f,
                                .scale_height = 1.0f,
                                .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR
                            }
                        },
                        .name = "gbuffer_normals",
                    } ),
                    builder.create_output_handle( {
                        .type = FrameGraphResourceType_Attachment,
                        .resource_info{
                            .texture = {
                                .scale_width = 1.0f,
                                .scale_height = 1.0f,
                                .format = VK_FORMAT_B8G8R8A8_UNORM,
                                .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR
                            }
                        },
                        .name = "gbuffer_metallic_roughness_occlusion",
                    } ),
                    builder.create_output_handle( {
                        .type = FrameGraphResourceType_Attachment,
                        .resource_info{
                            .texture = {
                                .scale_width = 1.0f,
                                .scale_height = 1.0f,
                                .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR
                            }
                        },
                        .name = "gbuffer_position",
                    } ),
                },
                .enabled = true,
                .name = "gbuffer_pass" } );
        }

        void update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) override {

            Renderer* renderer = context.renderer;

            if ( phase == PipelineUpdatePhase::Destroy ) {

                renderer->gpu->destroy_pipeline_layout( pipeline_layout );
                renderer->destroy_shader_state( gbuffer_shader );

                return;
            }

            // GBuffer pipeline no cull
            ShaderReflection gbuffer_reflection;
            gbuffer_shader = renderer->create_shader_state( {
                .stages = {
                    {
                        .source_file_path = "glsl/chapter3/mesh_gbuffer.glsl",
                        .headers = {
                            "glsl/platform.h",
                            "glsl/chapter3/common.h",
                            "glsl/chapter3/scene_mesh_data.h"
                        },
                        .type = VK_SHADER_STAGE_VERTEX_BIT,
                    },
                    {
                        .source_file_path = "glsl/chapter3/mesh_gbuffer.glsl",
                        .headers = {
                            "glsl/platform.h",
                            "glsl/chapter3/common.h",
                            "glsl/chapter3/scene_mesh_data.h"
                        },
                        .type = VK_SHADER_STAGE_FRAGMENT_BIT,
                    },
                },
                .name = "gbuffer" },
                "gbuffer", &gbuffer_reflection );

            pipeline_layout = renderer->create_pipeline_layout( gbuffer_reflection );

            PipelineCreation gbuffer_creation = {
                .rasterization = {
                    .cull_mode = VK_CULL_MODE_NONE
                },
                .depth_stencil = {
                    .depth_comparison = VK_COMPARE_OP_EQUAL,
                    .depth_enable = 1,
                    .depth_write_enable = 0,
                    .stencil_enable = 0,
                },
                .vertex_input = {
                    .bindings = {
                        {.binding = 0, .stride = 12, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX },
                        {.binding = 1, .stride = 16, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX },
                        {.binding = 2, .stride = 12, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX },
                        {.binding = 3, .stride = 8,  .inputRate = VK_VERTEX_INPUT_RATE_VERTEX }
                    },
                    .attributes = {
                        {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,    .offset = 0 },
                        {.location = 1, .binding = 1, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0 },
                        {.location = 2, .binding = 2, .format = VK_FORMAT_R32G32B32_SFLOAT,    .offset = 0 },
                        {.location = 3, .binding = 3, .format = VK_FORMAT_R32G32_SFLOAT,       .offset = 0 }
                    }
                },
                .shader = gbuffer_shader,
                .layout = pipeline_layout,
                .name = "gbuffer_no_cull",
                .render_pass_name = "gbuffer_pass",
            };

            context.frame_graph->cache_render_pass_output( "gbuffer_pass", renderer->gpu, gbuffer_creation.render_pass_output, false );

            PipelineHandle gbuffer_no_cull_pipeline = renderer->create_pipeline( gbuffer_reflection, gbuffer_creation );
            chapter3::pipeline_cache.insert( hash_calculate( "gbuffer_no_cull" ), gbuffer_no_cull_pipeline );

            // GBuffer pipeline cull
            gbuffer_creation.rasterization.cull_mode = VK_CULL_MODE_BACK_BIT;
            PipelineHandle gbuffer_cull_pipeline = renderer->create_pipeline( gbuffer_reflection, gbuffer_creation );
            chapter3::pipeline_cache.insert( hash_calculate( "gbuffer_cull" ), gbuffer_cull_pipeline );
        }

        void render( FrameGraphRenderContext& context ) override {

            CommandBuffer* gpu_commands = context.gpu_commands;
            RenderView* render_view = context.render_view;
            RenderScene* render_scene = render_view->scene;
            Renderer* renderer = context.renderer;
            RenderBlackboard& render_blackboard = *context.render_blackboard;

            FlatHashMapIterator it = pipeline_cache.find( hash_calculate( "gbuffer_no_cull" ) );
            RASSERT( it.is_valid() );

            PipelineHandle pipeline_no_cull = pipeline_cache.get( it );

            it = pipeline_cache.find( hash_calculate( "gbuffer_cull" ) );
            RASSERT( it.is_valid() );
            PipelineHandle pipeline_cull = pipeline_cache.get( it );

            for ( RenderItem& render_mesh : render_view->opaque_items ) {
                MeshInstance* mesh_instance = render_mesh.mesh_instance;
                Mesh& mesh = render_scene->meshes[ mesh_instance->mesh_index ];

                if ( mesh.is_double_sided() ) {
                    gpu_commands->bind_pipeline( pipeline_no_cull );
                } else {
                    gpu_commands->bind_pipeline( pipeline_cull );
                }

                gpu_commands->bind_vertex_buffer( mesh.position_buffer, 0, mesh.position_offset );
                if ( mesh.tangent_buffer.is_valid() ) {
                    gpu_commands->bind_vertex_buffer( mesh.tangent_buffer, 1, mesh.tangent_offset );
                }
                gpu_commands->bind_vertex_buffer( mesh.normal_buffer, 2, mesh.normal_offset );

                if ( mesh.texcoord_buffer.is_valid() ) {
                    gpu_commands->bind_vertex_buffer( mesh.texcoord_buffer, 3, mesh.texcoord_offset );
                }

                gpu_commands->bind_index_buffer( mesh.index_buffer, mesh.index_offset_bytes, mesh.index_type );

                gpu_commands->bind_descriptor_set( { renderer->gpu->bindless_descriptor_set, chapter3::scene_ds }, { chapter3::scene_cb_offset } );

                gpu_commands->draw_indexed( raptor::TopologyType::Triangle, mesh.index_count, 1, 0, 0, mesh_instance->gpu_mesh_instance_index );
            }
        }

        PipelineLayoutHandle        pipeline_layout;
        ShaderStateHandle           gbuffer_shader;
    }; // struct GBufferPass

    //
    //
    struct LighPass : public FrameGraphRenderPass {
        void declare_frame_graph_node( FrameGraphResourceContext& context ) override {
            FrameGraphBuilder& builder = *context.frame_graph->builder;

            context.frame_graph->add_node_v2( {
                .inputs = {
                    {
                        .type = FrameGraphResourceType_Texture,
                        .handle = builder.get_output_handle( "gbuffer_pass", "gbuffer_colour" )
                    },
                    {
                        .type = FrameGraphResourceType_Texture,
                        .handle = builder.get_output_handle( "gbuffer_pass", "gbuffer_normals" )
                    },
                    {
                        .type = FrameGraphResourceType_Texture,
                        .handle = builder.get_output_handle( "gbuffer_pass", "gbuffer_metallic_roughness_occlusion" )
                    },
                    {
                        .type = FrameGraphResourceType_Texture,
                        .handle = builder.get_output_handle( "gbuffer_pass", "gbuffer_position" )
                    },
                },
                .outputs = {
                    builder.create_output_handle( {
                        .type = FrameGraphResourceType_Attachment,
                        .resource_info{
                            .texture = {
                                .scale_width = 1.0f,
                                .scale_height = 1.0f,
                                .format = VK_FORMAT_B8G8R8A8_UNORM,
                                .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR
                            }
                        },
                        .name = "lighting",
                    } ),
                },
                .enabled = true,
                .name = "lighting_pass" } );
        }

        void update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) override {

            Renderer* renderer = context.renderer;

            if ( phase == PipelineUpdatePhase::Destroy ) {

                renderer->gpu->destroy_pipeline_layout( pipeline_layout );
                renderer->destroy_shader_state( pbr_lighting_shader );

                return;
            }

            // PBR lighting pipeline
            ShaderReflection pbr_lighting_reflection;
            pbr_lighting_shader = renderer->create_shader_state( {
                .stages = {
                    {
                        .source_file_path = "glsl/chapter3/post_pbr_lighting.glsl",
                        .headers = {
                            "glsl/platform.h",
                            "glsl/chapter3/common.h",
                        },
                        .type = VK_SHADER_STAGE_VERTEX_BIT,
                    },
                    {
                        .source_file_path = "glsl/chapter3/post_pbr_lighting.glsl",
                        .headers = {
                            "glsl/platform.h",
                            "glsl/chapter3/common.h",
                        },
                        .type = VK_SHADER_STAGE_FRAGMENT_BIT,
                    },
                },
                .name = "pbr_lighting" },
                "pbr_lighting", &pbr_lighting_reflection );

            pipeline_layout = renderer->create_pipeline_layout( pbr_lighting_reflection );
            PipelineCreation pbr_lighting_creation = {
                .depth_stencil = {
                    .depth_comparison = VK_COMPARE_OP_NEVER,
                    .depth_enable = 0,
                    .depth_write_enable = 0,
                    .stencil_enable = 0,
                },
                .shader = pbr_lighting_shader,
                .layout = pipeline_layout,
                .name = "pbr_lighting",
                .render_pass_name = "lighting_pass",
            };

            context.frame_graph->cache_render_pass_output( "lighting_pass", renderer->gpu, pbr_lighting_creation.render_pass_output, false );

            PipelineHandle pbr_lighting_pipeline = renderer->create_pipeline( pbr_lighting_reflection, pbr_lighting_creation );
            chapter3::pipeline_cache.insert( hash_calculate( "pbr_lighting" ), pbr_lighting_pipeline );
        }

        void render( FrameGraphRenderContext& context ) override {

            CommandBuffer* gpu_commands = context.gpu_commands;
            RenderScene* render_scene = context.render_view->scene;
            Renderer* renderer = context.renderer;
            RenderBlackboard& render_blackboard = *context.render_blackboard;

            FlatHashMapIterator it = pipeline_cache.find( hash_calculate( "pbr_lighting" ) );
            RASSERT( it.is_valid() );

            PipelineHandle pipeline = pipeline_cache.get( it );

            gpu_commands->bind_pipeline( pipeline );
            gpu_commands->bind_descriptor_set( { renderer->gpu->bindless_descriptor_set, descriptor_set }, { chapter3::scene_cb_offset, light_cb_offset } );

            gpu_commands->draw( TopologyType::Triangle, 0, 3, 0, 1 );
        }

        void create_gpu_resources( FrameGraphResourceContext& context ) override {

            FlatHashMapIterator it = pipeline_cache.find( hash_calculate( "pbr_lighting" ) );
            RASSERT( it.is_valid() );

            Renderer* renderer = context.renderer;
            GpuDevice& gpu = *renderer->gpu;

            PipelineHandle pipeline = pipeline_cache.get( it );
            DescriptorSetLayoutHandle layout_handle = gpu.get_descriptor_set_layout( pipeline, k_material_descriptor_set_index );
            ShaderReflectionInfo* reflection_info = renderer->get_shader_reflection( pipeline );

            DescriptorSetCreation ds_creation{ };
            ds_creation.layout = layout_handle;
            ds_creation.dynamic_buffers = {
                { .binding = renderer->get_binding_index( reflection_info, "LocalConstants"), .size = sizeof( GpuSceneData ) },
                { .binding = renderer->get_binding_index( reflection_info, "GbufferTextures"), .size = sizeof( glm::vec4 ) },
            };

            descriptor_set = gpu.create_descriptor_set( ds_creation );
        }

        void upload_gpu_data( FrameGraphResourceContext& context ) override {
            FrameGraph* frame_graph = context.frame_graph;
            FrameGraphNode* node = frame_graph  ->get_node( "lighting_pass" );
            if ( node == nullptr ) {
                RASSERT( false );
                return;
            }

            Renderer* renderer = context.renderer;

            FrameGraphResource* color_texture = frame_graph->access_output_resource( node->inputs[ 0 ] );
            FrameGraphResource* normal_texture = frame_graph->access_output_resource( node->inputs[ 1 ] );
            FrameGraphResource* roughness_texture = frame_graph->access_output_resource( node->inputs[ 2 ] );
            FrameGraphResource* position_texture = frame_graph->access_output_resource( node->inputs[ 3 ] );

            u32* uniform_data = ( u32* )renderer->gpu->dynamic_buffer_allocate( sizeof( glm::vec4 ), 16, &light_cb_offset );
            if ( uniform_data ) {
                uniform_data[ 0 ] = color_texture->resource_info.texture.image_view.index();
                uniform_data[ 1 ] = normal_texture->resource_info.texture.image_view.index();
                uniform_data[ 2 ] = roughness_texture->resource_info.texture.image_view.index();
                uniform_data[ 3 ] = position_texture->resource_info.texture.image_view.index();
            }
        }
        // TODO(marco): handle on resize

        void destroy_gpu_resources( FrameGraphResourceContext& context ) override {
            Renderer* renderer = context.renderer;
            renderer->gpu->destroy_descriptor_set( descriptor_set );
        }

        u32                     light_cb_offset;
        ShaderStateHandle       pbr_lighting_shader;
        PipelineLayoutHandle    pipeline_layout;
        DescriptorSetHandle     descriptor_set;
    }; // struct LighPass

    //
    //
    struct TransparentPass : public FrameGraphRenderPass {
        void declare_frame_graph_node( FrameGraphResourceContext& context ) override {
            FrameGraphBuilder& builder = *context.frame_graph->builder;

            context.frame_graph->add_node_v2( {
                .inputs = {
                    {
                        .type = FrameGraphResourceType_Attachment,
                        .handle = builder.get_output_handle( "lighting_pass", "lighting" )
                    },
                    {
                        .type = FrameGraphResourceType_Attachment,
                        .handle = builder.get_output_handle( "depth_prepass", "depth" )
                    },
                },
                .outputs = {
                    builder.create_output_reference(
                        builder.get_output_handle( "lighting_pass", "lighting" ),
                        FrameGraphResourceType_Reference )
                },
                .enabled = true,
                .name = "transparent_pass" } );
        }

        void update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) override {

            Renderer* renderer = context.renderer;

            if ( phase == PipelineUpdatePhase::Destroy ) {

                renderer->gpu->destroy_pipeline_layout( pipeline_layout );
                renderer->destroy_shader_state( transparent_shader );

                return;
            }

            // Transparent pipeline no cull
            ShaderReflection transparent_reflection;
            transparent_shader = renderer->create_shader_state( {
                .stages = {
                    {
                        .source_file_path = "glsl/chapter3/mesh_transparent.glsl",
                        .headers = {
                            "glsl/platform.h",
                            "glsl/chapter3/common.h",
                            "glsl/chapter3/scene_mesh_data.h"
                        },
                        .type = VK_SHADER_STAGE_VERTEX_BIT,
                    },
                    {
                        .source_file_path = "glsl/chapter3/mesh_transparent.glsl",
                        .headers = {
                            "glsl/platform.h",
                            "glsl/chapter3/common.h",
                            "glsl/chapter3/scene_mesh_data.h"
                        },
                        .type = VK_SHADER_STAGE_FRAGMENT_BIT,
                    },
                },
                .name = "transparent" },
                "transparent", &transparent_reflection );

            pipeline_layout = renderer->create_pipeline_layout( transparent_reflection );
            PipelineCreation transparent_creation = {
                .rasterization = {
                    .cull_mode = VK_CULL_MODE_NONE
                },
                .depth_stencil = {
                    .depth_comparison = VK_COMPARE_OP_LESS_OR_EQUAL,
                    .depth_enable = 1,
                    .depth_write_enable = 0,
                    .stencil_enable = 0,
                },
                .blend_state = {
                    .blend_states = {
                        {
                            .source_color = VK_BLEND_FACTOR_SRC_ALPHA,
                            .destination_color = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
                        }
                    }
                },
                .vertex_input = {
                    .bindings = {
                        {.binding = 0, .stride = 12, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX },
                        {.binding = 1, .stride = 16, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX },
                        {.binding = 2, .stride = 12, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX },
                        {.binding = 3, .stride = 8,  .inputRate = VK_VERTEX_INPUT_RATE_VERTEX }
                    },
                    .attributes = {
                        {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,    .offset = 0 },
                        {.location = 1, .binding = 1, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0 },
                        {.location = 2, .binding = 2, .format = VK_FORMAT_R32G32B32_SFLOAT,    .offset = 0 },
                        {.location = 3, .binding = 3, .format = VK_FORMAT_R32G32_SFLOAT,       .offset = 0 }
                    }
                },
                .shader = transparent_shader,
                .layout = pipeline_layout,
                .name = "transparent_no_cull",
                .render_pass_name = "transparent_pass",
            };

            context.frame_graph->cache_render_pass_output( "transparent_pass", renderer->gpu, transparent_creation.render_pass_output, false );

            PipelineHandle transparent_no_cull_pipeline = renderer->create_pipeline( transparent_reflection, transparent_creation );
            chapter3::pipeline_cache.insert( hash_calculate( "transparent_no_cull" ), transparent_no_cull_pipeline );

            // Transparent pipeline cull
            transparent_creation.rasterization.cull_mode = VK_CULL_MODE_BACK_BIT;
            PipelineHandle transparent_cull_pipeline = renderer->create_pipeline( transparent_reflection, transparent_creation );
            chapter3::pipeline_cache.insert( hash_calculate( "transparent_cull" ), transparent_cull_pipeline );
        }

        void render( FrameGraphRenderContext& context ) override {

            CommandBuffer* gpu_commands = context.gpu_commands;
            RenderView* render_view = context.render_view;
            RenderScene* render_scene = render_view->scene;
            Renderer* renderer = context.renderer;
            RenderBlackboard& render_blackboard = *context.render_blackboard;

            FlatHashMapIterator it = pipeline_cache.find( hash_calculate( "transparent_no_cull" ) );
            RASSERT( it.is_valid() );

            PipelineHandle pipeline_no_cull = pipeline_cache.get( it );

            it = pipeline_cache.find( hash_calculate( "transparent_cull" ) );
            RASSERT( it.is_valid() );
            PipelineHandle pipeline_cull = pipeline_cache.get( it );

            for ( RenderItem& render_mesh : render_view->transparent_items ) {
                MeshInstance* mesh_instance = render_mesh.mesh_instance;
                Mesh& mesh = render_scene->meshes[ mesh_instance->mesh_index ];

                if ( mesh.is_double_sided() ) {
                    gpu_commands->bind_pipeline( pipeline_no_cull );
                } else {
                    gpu_commands->bind_pipeline( pipeline_cull );
                }

                gpu_commands->bind_vertex_buffer( mesh.position_buffer, 0, mesh.position_offset );
                if ( mesh.tangent_buffer.is_valid() ) {
                    gpu_commands->bind_vertex_buffer( mesh.tangent_buffer, 1, mesh.tangent_offset );
                }
                gpu_commands->bind_vertex_buffer( mesh.normal_buffer, 2, mesh.normal_offset );

                if ( mesh.texcoord_buffer.is_valid() ) {
                    gpu_commands->bind_vertex_buffer( mesh.texcoord_buffer, 3, mesh.texcoord_offset );
                }

                gpu_commands->bind_index_buffer( mesh.index_buffer, mesh.index_offset_bytes, mesh.index_type );

                gpu_commands->bind_descriptor_set( { renderer->gpu->bindless_descriptor_set, chapter3::scene_ds }, { chapter3::scene_cb_offset } );

                gpu_commands->draw_indexed( raptor::TopologyType::Triangle, mesh.index_count, 1, 0, 0, mesh_instance->gpu_mesh_instance_index );
            }
        }

        PipelineLayoutHandle        pipeline_layout;
        ShaderStateHandle           transparent_shader;
    }; // struct TransparentPass

    //
    //
    struct DoFPass : public FrameGraphRenderPass {

        struct alignas( 16 ) DoFData {
            u32                 textures[ 4 ]; // diffuse, depth
            float               znear;
            float               zfar;
            float               focal_length;
            float               plane_in_focus;
            float               aperture;
            float               padding[ 3 ];
        }; // struct DoFData

        void declare_frame_graph_node( FrameGraphResourceContext& context ) override {
            FrameGraphBuilder& builder = *context.frame_graph->builder;

            context.frame_graph->add_node_v2( {
                .inputs = {
                    {
                        .type = FrameGraphResourceType_Texture,
                        .handle = builder.get_output_handle( "transparent_pass", "lighting" )
                    },
                    {
                        .type = FrameGraphResourceType_Texture,
                        .handle = builder.get_output_handle( "depth_prepass", "depth" )
                    },
                },
                .outputs = {
                    builder.create_output_handle( {
                        .type = FrameGraphResourceType_Attachment,
                        .resource_info{
                            .texture = {
                                .scale_width = 1.0f,
                                .scale_height = 1.0f,
                                .format = VK_FORMAT_B8G8R8A8_UNORM,
                                .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR
                            }
                        },
                        .name = "final",
                    } ),
                },
                .enabled = true,
                .name = "depth_of_field_pass" } );
        }

        void add_ui() override {
            ImGui::InputFloat( "Focal Length", &focal_length);
            ImGui::InputFloat( "Plane in Focus", &plane_in_focus);
            ImGui::InputFloat( "Aperture", &aperture);
        }

        void update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) override {

            Renderer* renderer = context.renderer;

            if ( phase == PipelineUpdatePhase::Destroy ) {

                renderer->gpu->destroy_pipeline_layout( pipeline_layout );
                renderer->destroy_shader_state( dof_shader );

                return;
            }

            // DOF pipeline
            ShaderReflection dof_reflection;
            dof_shader = renderer->create_shader_state( {
                .stages = {
                    {
                        .source_file_path = "glsl/chapter3/post_dof.glsl",
                        .headers = {
                            "glsl/platform.h"
                        },
                        .type = VK_SHADER_STAGE_VERTEX_BIT,
                    },
                    {
                        .source_file_path = "glsl/chapter3/post_dof.glsl",
                        .headers = {
                            "glsl/platform.h"
                        },
                        .type = VK_SHADER_STAGE_FRAGMENT_BIT,
                    },
                },
                .name = "dof" },
                "dof", &dof_reflection );

            pipeline_layout = renderer->create_pipeline_layout( dof_reflection );
            PipelineCreation dof_creation = {
                .shader = dof_shader,
                .layout = pipeline_layout,
                .name = "dof",
                .render_pass_name = "depth_of_field_pass",
            };

            context.frame_graph->cache_render_pass_output( "depth_of_field_pass", renderer->gpu, dof_creation.render_pass_output, false );

            PipelineHandle dof_pipeline = renderer->create_pipeline( dof_reflection, dof_creation );
            chapter3::pipeline_cache.insert( hash_calculate( "dof" ), dof_pipeline );
        }

        void pre_render( FrameGraphRenderContext& context ) override {

            CommandBuffer* gpu_commands = context.gpu_commands;
            RenderScene* render_scene = context.render_view->scene;
            Renderer* renderer = context.renderer;

            FrameGraphResource* texture = ( FrameGraphResource* )context.frame_graph->get_resource( "lighting" );
            RASSERT ( texture != nullptr );

            gpu_commands->copy_image(
                texture->resource_info.texture.image,
                scene_mips->image,
                ImageSyncState{
                    .stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    .access = VK_ACCESS_2_SHADER_READ_BIT,
                    .layout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
                }
            );

           // gpu_commands->copy_image( texture->resource_info.texture.image, scene_mips->image, RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
        }

        void render( FrameGraphRenderContext& context ) override {

            CommandBuffer* gpu_commands = context.gpu_commands;
            RenderScene* render_scene = context.render_view->scene;
            Renderer* renderer = context.renderer;

            FlatHashMapIterator it = pipeline_cache.find( hash_calculate( "dof" ) );
            RASSERT( it.is_valid() );

            GpuDevice& gpu = *renderer->gpu;

            PipelineHandle pipeline = pipeline_cache.get( it );

            gpu_commands->bind_pipeline( pipeline );
            gpu_commands->bind_descriptor_set( { renderer->gpu->bindless_descriptor_set, descriptor_set }, { dof_cb_offset } );

            gpu_commands->draw( TopologyType::Triangle, 0, 3, 0, 1 );

        }

        void on_resize( FrameGraphResourceContext& context, u32 new_width, u32 new_height ) override {

            Renderer* renderer = context.renderer;
            FrameGraph* frame_graph = context.frame_graph;
            GpuDevice& gpu = *renderer->gpu;

            FrameGraphNode* node = frame_graph->get_node( "depth_of_field_pass" );
            if ( node == nullptr ) {
                RASSERT( false );
                return;
            }

            FrameGraphResource* diffuse_texture = frame_graph->access_output_resource( node->inputs[ 0 ] );
            FrameGraphResourceInfo& info = diffuse_texture->resource_info;

            new_width = u32( info.texture.scale_height != 0.0f ? info.texture.scale_height * new_width : new_width );
            new_height = u32( info.texture.scale_height != 0.0f ? info.texture.scale_height * new_height : new_height );

            u32 w = new_width;
            u32 h = new_height;

            u32 mips = 1;
            while ( w > 1 && h > 1 ) {
                w /= 2;
                h /= 2;
                mips++;
            }

            // Destroy scene mips
            renderer->destroy_texture( scene_mips );

            // Reuse cached texture creation and create new scene mips.
            ImageCreation dof_scene_tc {
                .initial_data = nullptr,
                .width = ( u16 )new_width,
                .height = ( u16 )new_height,
                .mip_level_count = ( u8 )mips,
                .format = info.texture.format,
                .name = "scene_mips",
            };
            scene_mips = renderer->create_texture( dof_scene_tc );
            gpu.link_image_sampler( scene_mips->image, mips_sampler );

            FrameGraphResource* depth_texture = frame_graph->access_output_resource( node->inputs[ 1 ] );
            gpu.link_image_sampler( depth_texture->resource_info.texture.image, depth_sampler );
        }

        void create_gpu_resources( FrameGraphResourceContext& context ) override {

            FlatHashMapIterator it = pipeline_cache.find( hash_calculate( "dof" ) );
            RASSERT( it.is_valid() );

            Renderer* renderer = context.renderer;
            GpuDevice& gpu = *renderer->gpu;

            PipelineHandle pipeline = pipeline_cache.get( it );
            DescriptorSetLayoutHandle layout_handle = gpu.get_descriptor_set_layout( pipeline, k_material_descriptor_set_index );
            ShaderReflectionInfo* reflection_info = renderer->get_shader_reflection( pipeline );

            DescriptorSetCreation ds_creation{ };
            ds_creation.layout = layout_handle;
            ds_creation.dynamic_buffers = {
                { .binding = renderer->get_binding_index( reflection_info, "CameraParameters"), .size = sizeof( DoFData ) },
            };

            descriptor_set = gpu.create_descriptor_set( ds_creation );

            FrameGraph* frame_graph = context.frame_graph;
            FrameGraphNode* node = frame_graph->get_node( "depth_of_field_pass" );
            if ( node == nullptr ) {
                RASSERT( false );
                return;
            }

            FrameGraphResource* diffuse_texture = frame_graph->access_output_resource( node->inputs[ 0 ] );
            FrameGraphResourceInfo& info = diffuse_texture->resource_info;

            u32 w = info.texture.width;
            u32 h = info.texture.height;

            u32 mips = 1;
            while ( w > 1 && h > 1) {
                w /= 2;
                h /= 2;
                mips++;
            }

            ImageCreation dof_scene_tc {
                .initial_data = nullptr,
                .width = ( u16 )info.texture.width,
                .height = ( u16 )info.texture.height,
                .mip_level_count = ( u8 )mips,
                .format = info.texture.format,
                .name = "scene_mips",
            };
            scene_mips = renderer->create_texture( dof_scene_tc );

            SamplerCreation sampler_creation {
                .min_filter = VK_FILTER_LINEAR,
                .mag_filter = VK_FILTER_LINEAR,
                .mip_filter = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                .address_mode_u = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .address_mode_v = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .address_mode_w = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .name = "dof_mips_sampler"
            };
            mips_sampler = gpu.create_sampler( sampler_creation );
            gpu.link_image_sampler( scene_mips->image, mips_sampler );

            FrameGraphResource* depth_texture = frame_graph->access_output_resource( node->inputs[ 1 ] );
            FrameGraphResourceInfo& depth_info = depth_texture->resource_info;

            sampler_creation.min_filter = VK_FILTER_NEAREST;
            sampler_creation.mag_filter = VK_FILTER_NEAREST;
            sampler_creation.name = "dof_depth_sampler";
            depth_sampler = gpu.create_sampler( sampler_creation );
            gpu.link_image_sampler( depth_texture->resource_info.texture.image, depth_sampler );

            znear = 0.1f;
            zfar = 1000.0f;
            focal_length = 5.0f;
            plane_in_focus = 1.0f;
            aperture = 8.0f;
        }

        void upload_gpu_data( FrameGraphResourceContext& context ) override {
            FrameGraph* frame_graph = context.frame_graph;
            FrameGraphNode* node = frame_graph->get_node( "depth_of_field_pass" );
            if ( node == nullptr ) {
                RASSERT( false );
                return;
            }

            Renderer* renderer = context.renderer;

            FrameGraphResource* diffuse_texture = frame_graph->access_output_resource( node->inputs[ 0 ] );
            FrameGraphResource* depth_texture = frame_graph->access_output_resource( node->inputs[ 1 ] );

            DoFData* dof_data = ( DoFData* )renderer->gpu->dynamic_buffer_allocate( sizeof( DoFData ), 16, &dof_cb_offset );
            if ( dof_data ) {
                dof_data->textures[ 0 ] = scene_mips->image_view.index();
                dof_data->textures[ 1 ] = depth_texture->resource_info.texture.image_view.index();
                dof_data->znear = znear;
                dof_data->zfar = zfar;
                dof_data->focal_length = focal_length;
                dof_data->plane_in_focus = plane_in_focus;
                dof_data->aperture = aperture;
            }
        }

        void destroy_gpu_resources( FrameGraphResourceContext& context ) override {
            Renderer* renderer = context.renderer;

            renderer->gpu->destroy_descriptor_set( descriptor_set );
            renderer->gpu->destroy_sampler( depth_sampler );
            renderer->gpu->destroy_sampler( mips_sampler );
            renderer->destroy_texture( scene_mips );
        }

        u32                     dof_cb_offset;
        DescriptorSetHandle     descriptor_set;
        TextureResource*        scene_mips;
        PipelineLayoutHandle    pipeline_layout;
        ShaderStateHandle       dof_shader;

        SamplerHandle           depth_sampler;
        SamplerHandle           mips_sampler;

        float                   znear;
        float                   zfar;
        float                   focal_length;
        float                   plane_in_focus;
        float                   aperture;
    }; // struct DoFPass
}; // chapter3
}; // raptor


//
//
int main( int argc, char** argv ) {

    if ( argc < 2 ) {
        printf( "Usage: chapter3 [path to glTF model]\n");
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
    WindowConfiguration wconf{ 1280, 800, "Chapter 3: Frame Graph", &MemoryService::instance()->system_allocator};
    raptor::Window window;
    window.init( &wconf );

    InputService input;
    input.init( allocator );

    // Callback register: input needs to react to OS messages.
    window.register_os_messages_callback( input_os_messages_callback, &input );

    // graphics
    GpuDeviceCreation dc;
    dc.enable_bindless = true;
    dc.force_disable_mesh_shaders = true;
    dc.set_window( window.width, window.height, window.platform_handle ).set_allocator( &MemoryService::instance()->system_allocator )
      .set_num_threads( task_scheduler.GetNumTaskThreads() );
    dc.resource_pool_creation.buffers = 1024;

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

    chapter3::pipeline_cache.init( allocator, 16 );

    PostProcessRenderingFeature post_feature;
    frame_renderer.post = &post_feature;

    MeshesRenderingFeature meshes;
    frame_renderer.meshes = &meshes;

    // Load frame graph and parse gpu techniques
    {
        // Special node to determine final output
        FrameGraphNodeCreation present_node_info {
            .name = "present",
        };
        frame_graph.add_node( present_node_info );

        frame_renderer.add_render_pass( "depth_prepass", rnewa( chapter3::DepthPrePass, allocator, 64) );
        frame_renderer.add_render_pass( "gbuffer_pass", rnewa( chapter3::GBufferPass, allocator, 64 ) );
        frame_renderer.add_render_pass( "lighting_pass", rnewa( chapter3::LighPass, allocator, 64 ) );
        frame_renderer.add_render_pass( "transparent_pass", rnewa( chapter3::TransparentPass, allocator, 64 ) );
        frame_renderer.add_render_pass( "depth_of_field_pass", rnewa( chapter3::DoFPass, allocator, 64 ) );

        FrameGraphResourceContext resource_context{ &renderer, &frame_graph, &frame_renderer.render_blackboard, &frame_renderer.render_config, &render_scene };

        // declare frame graph nodes and register passes
        for ( FrameGraphRenderPass* pass : frame_renderer.render_passes ) {
            pass->declare_frame_graph_node( resource_context );
            frame_graph.builder->register_render_pass( pass->name, pass );
        }

        frame_graph.compile();

        // Compile all PSOs
        for ( FrameGraphRenderPass* pass : frame_renderer.render_passes ) {
            pass->update_psos( resource_context, PipelineUpdatePhase::Create );
        }

        // Compile Post-Process feature PSOs
        post_feature.update_psos( &renderer, &frame_graph, PipelineUpdatePhase::Create );
    }

    Directory cwd{ };
    directory_current(&cwd);

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

        render_scene.add_and_load_model( file_name, file_base_path, MemoryService::instance()->get_thread_allocator() );
    }

    // Create scene resources
    {
        // Create scene mesh instances buffer with all material data
        chapter3::scene_mesh_instances_ssbo = gpu.create_buffer( {
            .size = sizeof( chapter3::MeshData ) * render_scene.mesh_instances.size,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
            .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .name = "mesh_instances_sb" } );

        FlatHashMapIterator it = chapter3::pipeline_cache.find( hash_calculate( "gbuffer_no_cull" ) );
        RASSERT( it.is_valid() );

        PipelineHandle gbuffer_pipeline = chapter3::pipeline_cache.get( it );
        DescriptorSetLayoutHandle gbuffer_layout_handle = gpu.get_descriptor_set_layout( gbuffer_pipeline, k_material_descriptor_set_index );
        ShaderReflectionInfo* gbuffer_reflection_info = renderer.get_shader_reflection( gbuffer_pipeline );


        ShaderReflectionInfo* reflection_info = gbuffer_reflection_info;

        DescriptorSetCreation ds_creation{ };
        ds_creation.layout = gbuffer_layout_handle;
        ds_creation.ssbos = {
            {.buffer = chapter3::scene_mesh_instances_ssbo,
             .binding = renderer.get_binding_index( reflection_info, "MeshData" ) },
        };
        ds_creation.dynamic_buffers = {
            {.binding = renderer.get_binding_index( reflection_info, "LocalConstants" ), .size = sizeof( GpuFrameData ) },
        };

        chapter3::scene_ds = gpu.create_descriptor_set( ds_creation );
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

    scene_graph.update_matrices();

    ArenaAllocator* scratch_allocator = MemoryService::instance()->get_thread_allocator();
    frame_renderer.create_resources( scratch_allocator );

    // TODO(marco):
    // - Compile frame graph each frame
    // - Implement simple debug pass to show intermediate result and demonstrate live graph pruning
    // - (Implement UI node view)

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
            frame_graph.on_resize( &renderer, &frame_renderer.render_blackboard,
                                   &frame_renderer.render_config, window.width, window.height );

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
                ImGui::InputFloat( "Scene global scale", &frame_renderer.render_config.global_scale, 0.001f );
                ImGui::SliderFloat3( "Light position", &light_position[0], -30.0f, 30.0f );
                ImGui::InputFloat( "Light radius", &light_radius );
                ImGui::InputFloat( "Light intensity", &light_intensity );
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

                frame_graph.add_ui();

                frame_graph.debug_ui();

                frame_renderer.render_config.post.draw_imgui();
            }
            ImGui::End();

            if ( ImGui::Begin( "GPU" ) ) {
                renderer.imgui_draw();

                ImGui::Separator();
                gpu_profiler.imgui_draw();

            }
            ImGui::End();

            MemoryService::instance()->imgui_draw();
        }

        {
            ZoneScopedN( "SceneGraphUpdate" );
            scene_graph.update_matrices();
        }

        {
            ZoneScopedN( "UniformBufferUpdate" );

            // Update scene constant buffer
            chapter3::GpuSceneData* uniform_data = gpu.dynamic_buffer_allocate<chapter3::GpuSceneData>( &chapter3::scene_cb_offset );
            if ( uniform_data ) {
                uniform_data->view_projection = game_camera.camera.view_projection;
                uniform_data->eye = glm::vec4{ game_camera.camera.position.x, game_camera.camera.position.y, game_camera.camera.position.z, 1.0f };
                uniform_data->light_position = glm::vec4{ light_position.x, light_position.y, light_position.z, 1.0f };
                uniform_data->light_range = light_radius;
                uniform_data->light_intensity = light_intensity;

            }

            if ( ( input.is_mouse_clicked( MOUSE_BUTTONS_LEFT ) || input.is_mouse_dragging( MOUSE_BUTTONS_LEFT ) ) && !ImGui::IsAnyItemHovered() ) {
                last_clicked_position = glm::vec2{ input.mouse_position.x, input.mouse_position.y };
            }

            FrameGraphResourceContext resource_context{ &renderer, &frame_graph, &frame_renderer.render_blackboard, &frame_renderer.render_config, &render_scene };

            for ( FrameGraphRenderPass* pass : frame_renderer.render_passes ) {
                pass->upload_gpu_data( resource_context );
            }

            // Upload per-mesh data
            if ( update_mesh_data )
            {
                update_mesh_data = false;

                Buffer* mesh_buffer = gpu.get_buffer( chapter3::scene_mesh_instances_ssbo );
                RASSERT( mesh_buffer );
                chapter3::MeshData* mesh_data = ( chapter3::MeshData* )mesh_buffer->mapped_data;
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
                    gpu.flush_buffer( chapter3::scene_mesh_instances_ssbo, 0, sizeof( chapter3::MeshData ) * render_scene.mesh_instances.size );
                }
            }

            frame_renderer.upload_gpu_data( game_camera, last_clicked_position, MemoryService::instance()->get_thread_allocator() );

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

            // Collect all command buffers for submission
            StaticArray<CommandBuffer*, 6> cbs;

            cbs.push( frame_graph.get_command_buffer_from_batch( CommandQueueType::Graphics, 0 ) );
            cbs.push( draw_task.gfx_cb );

            // Add image upload commands if any
            if ( image_upload_cb ) {
                cbs.push( image_upload_cb );
            }

            GpuSubmitSync present_sync = gpu.build_present_sync( VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT );

            gpu.queue_submit( CommandQueueType::Graphics, cbs.as_span(),
                              present_sync.waits.as_span(),
                              present_sync.signals.as_span() );

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

    imgui->shutdown();

    gpu_profiler.shutdown();

    scene_graph.shutdown();

    for ( FlatHashMapIterator it = chapter3::pipeline_cache.iterator_begin(); it.is_valid();
          chapter3::pipeline_cache.iterator_advance( it ) ) {
        PipelineHandle pipeline = chapter3::pipeline_cache.get( it );
        gpu.destroy_pipeline( pipeline );
    }
    chapter3::pipeline_cache.shutdown();

    gpu.destroy_descriptor_set( chapter3::scene_ds );
    gpu.destroy_buffer( chapter3::scene_mesh_instances_ssbo );

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
