#include "graphics/render_passes/restirgi_pass.hpp"
#include "graphics/render_scene.hpp"
#include "graphics/render_blackboard.hpp"

#include "foundation/numerics.hpp"

namespace raptor {

static void cache_restirgi_common_resources( FrameGraph* frame_graph, ReSTIRGICommonResources* resources ) {

    FrameGraphResource* resource = frame_graph->get_resource( "gbuffer_normals" );
    resources->normals_texture = resource->resource_info.texture.image;
    resources->normals_image_view = resource->resource_info.texture.image_view;

    resource = frame_graph->get_resource( "depth" );
    resources->depth_texture = resource->resource_info.texture.image;
    resources->depth_image_view = resource->resource_info.texture.image_view;

    resource = frame_graph->get_resource( "linear_z_dd" );
    resources->linear_depth_texture = resource->resource_info.texture.image;
    resources->linear_depth_image_view = resource->resource_info.texture.image_view;

    resource = frame_graph->get_resource( "gbuffer_colour" );
    resources->albedo_texture = resource->resource_info.texture.image;
    resources->albedo_image_view = resource->resource_info.texture.image_view;

    resource = frame_graph->get_resource( "mesh_id" );
    resources->mesh_id_texture = resource->resource_info.texture.image;
    resources->mesh_id_image_view = resource->resource_info.texture.image_view;

    resource = frame_graph->get_resource( "gbuffer_metallic_roughness_occlusion" );
    resources->orm_texture = resource->resource_info.texture.image;
    resources->orm_image_view = resource->resource_info.texture.image_view;

    resource = frame_graph->get_resource( "motion_vectors" );
    resources->motion_vectors_texture = resource->resource_info.texture.image;
    resources->motion_vectors_image_view = resource->resource_info.texture.image_view;

    resource = frame_graph->get_resource( "normals_history" );
    resources->normal_history_texture = resource->resource_info.texture.image;
    resources->normal_history_image_view = resource->resource_info.texture.image_view;

    resource = frame_graph->get_resource( "linear_depth_history" );
    resources->linear_depth_history_texture = resource->resource_info.texture.image;
    resources->linear_depth_history_image_view = resource->resource_info.texture.image_view;

    resource = frame_graph->get_resource( "mesh_id_history" );
    resources->mesh_id_history_texture = resource->resource_info.texture.image;
    resources->mesh_id_history_image_view = resource->resource_info.texture.image_view;
}

// Sample Generation ///////////////////////////////////////////////////////////
struct ReSTIRGIConstants {
    u32                 albedo_texture_index;
    u32                 normal_texture_index;
    u32                 orm_texture_index;
    u32                 depth_texture_index;

    u32                 sbt_offset; // shader binding table offset
    u32                 sbt_stride; // shader binding table stride
    u32                 miss_index;
    u32                 output_texture_index;

    float               light[4];

    float               light_range;
    float               light_intensity;
    i32                 resolution[2];

    u32                 linear_depth_index;
    u32                 linear_depth_history_index;
    u32                 normal_history_index;
    u32                 motion_vectors_index;

    u32                 mesh_id_index;
    u32                 mesh_id_history_index;
    u32                 output_history_texture_index;
    u32                 output_indirect_texture_index;
};

void ReSTIRGIPass::declare_frame_graph_node( FrameGraphResourceContext& context ) {
    FrameGraphBuilder& builder = *context.frame_graph->builder;
    renderer = context.renderer;

    context.frame_graph->add_node_v2( {
        .inputs = {
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "gbuffer_pass_late", "depth" )
            },
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "gbuffer_pass_late", "gbuffer_colour" )
            },
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "gbuffer_pass_late", "gbuffer_normals" )
            },
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "gbuffer_pass_late", "gbuffer_metallic_roughness_occlusion" )
            },
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "motion_vector_pass", "visibility_motion_vectors" )
            }
        },
        .outputs = {
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Texture,
                .resource_info = {
                    .external = true
                },
                .name = "restirgi_output",
            } ),
        },
        .scheduling = { CommandQueueType::Graphics, 0 },
        .enabled = true,
        .compute = true,
        .name = "restirgi_pass" } );
}

void ReSTIRGIPass::update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) {
    Renderer* renderer = context.renderer;

    if ( phase == PipelineUpdatePhase::Destroy ) {
        renderer->destroy_ray_tracing_pipeline_state( sample_generation_pipeline );
        renderer->destroy_compute_pipeline_state( spatial_sampling_pipeline );
        renderer->destroy_compute_pipeline_state( temporal_accumulation_pipeline );

        return;
    }

    // Ray tracing pipelines
    RayTracingPipelineTransaction rt_transaction( renderer );

    RayTracingPipelineState& sample_pipeline = rt_transaction.add( sample_generation_pipeline );

    renderer->create_raytracing_pipeline_state(
        {
        .stages = {
            {
                .source_file_path = "glsl/chapter14/restir_gi.glsl",
                .type = VK_SHADER_STAGE_RAYGEN_BIT_KHR,
            },
            {
                .source_file_path = "glsl/chapter14/restir_gi.glsl",
                .type = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
            },
            {
                .source_file_path = "glsl/chapter14/restir_gi.glsl",
                .type = VK_SHADER_STAGE_MISS_BIT_KHR,
            },
        },
        .name = "sample_generation" },
        {
            .name = "restirgi_sample_generation",
            .render_pass_name = "restirgi_pass",
        },
        "restir_gi",
        context.frame_graph, sample_pipeline );

    rt_transaction.commit_or_rollback();

    // Compute pipelines
    ComputePipelineTransaction compute_transaction( renderer );

    ComputePipelineState& spatial_sampling = compute_transaction.add( spatial_sampling_pipeline );

    renderer->create_compute_pipeline_state(
        {
        .stages = {
            {
                .source_file_path = "glsl/chapter14/restir_gi.glsl",
                .type = VK_SHADER_STAGE_COMPUTE_BIT,
            },
        },
        .name = "spatial_sampling" },
        {
            .name = "restirgi_spatial_sampling",
            .render_pass_name = "restirgi_pass",
        },
        "restir_gi",
        context.frame_graph, spatial_sampling );

    ComputePipelineState& temporal_accumulation = compute_transaction.add( temporal_accumulation_pipeline );

    renderer->create_compute_pipeline_state(
        {
        .stages = {
            {
                .source_file_path = "glsl/chapter14/restir_gi.glsl",
                .type = VK_SHADER_STAGE_COMPUTE_BIT,
            },
        },
        .name = "temporal_accumulation" },
        {
            .name = "restirgi_temporal_accumulation",
            .render_pass_name = "restirgi_pass",
        },
        "restir_gi",
        context.frame_graph, temporal_accumulation );

    compute_transaction.commit_or_rollback();
}

void ReSTIRGIPass::render( FrameGraphRenderContext& context ) {
    if ( !enabled ) {
        return;
    }

    GpuDevice& gpu = *renderer->gpu;
    RenderBlackboard& render_blackboard = *context.render_blackboard;

    // Wait for valid TLAS before creating the descriptors
    if ( render_blackboard.tlas.is_invalid() ) {
        return;
    }

    if ( !descriptors_created ) {
        create_descriptors( context );
        descriptors_created = true;
    }

    bool ping = ( gpu.absolute_frame % 2 ) == 0;
    BufferHandle temporal_reservoir_buffer_read = ping ? temporal_reservoir_buffer[ 0 ] : temporal_reservoir_buffer[ 1 ];
    BufferHandle temporal_reservoir_buffer_write = ping ? temporal_reservoir_buffer[ 1 ] : temporal_reservoir_buffer[ 0 ];

    CommandBuffer* cb = context.gpu_commands;

    if ( reset_history ) {
        cb->add_buffer_barrier( spatial_reservoir_buffer,
            0,
            reservoir_buffer_size,
            {
                .stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .access = VK_ACCESS_2_TRANSFER_WRITE_BIT
            });
        cb->add_buffer_barrier( temporal_reservoir_buffer_read,
            0,
            reservoir_buffer_size,
            {
                .stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .access = VK_ACCESS_2_TRANSFER_WRITE_BIT
            });
        cb->add_buffer_barrier( temporal_reservoir_buffer_write,
            0,
            reservoir_buffer_size,
            {
                .stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .access = VK_ACCESS_2_TRANSFER_WRITE_BIT
            });
        cb->flush_barriers();

        cb->fill_buffer( spatial_reservoir_buffer, 0, reservoir_buffer_size, 0 );
        cb->fill_buffer( temporal_reservoir_buffer_read, 0, reservoir_buffer_size, 0 );
        cb->fill_buffer( temporal_reservoir_buffer_write, 0, reservoir_buffer_size, 0 );

        reset_history = false;
    }

    cb->add_buffer_barrier( temporal_reservoir_buffer_write,
        0,
        reservoir_buffer_size,
        {
            .stage = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            .access = VK_ACCESS_2_SHADER_WRITE_BIT
        });
    cb->add_buffer_barrier( temporal_reservoir_buffer_read,
        0,
        reservoir_buffer_size,
        {
            .stage = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            .access = VK_ACCESS_2_SHADER_READ_BIT
        });
    cb->add_buffer_barrier( spatial_reservoir_buffer,
        0,
        reservoir_buffer_size,
        {
            .stage = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            .access = VK_ACCESS_2_SHADER_WRITE_BIT
        });
    cb->flush_barriers();

    cb->bind_pipeline( sample_generation_pipeline.pipeline );

    DescriptorSetHandle descriptor_set = ping ? descriptor_set_ping : descriptor_set_pong;
    cb->bind_descriptor_set( { renderer->gpu->bindless_descriptor_set, descriptor_set },
                              { context.render_blackboard->scene_cb_offset, constants_offset, render_blackboard.lighting.lighting_constants_cb_offset } );

    cb->trace_rays( sample_generation_pipeline.pipeline, render_blackboard.render_width / 2, render_blackboard.render_height / 2, 1 );

    cb->add_buffer_barrier( temporal_reservoir_buffer_write,
        0,
        reservoir_buffer_size,
        {
            .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .access = VK_ACCESS_2_SHADER_READ_BIT
        });
    cb->add_buffer_barrier( spatial_reservoir_buffer,
        0,
        reservoir_buffer_size,
        {
            .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .access = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT
        });

    cb->add_image_barrier( output_indirect_texture, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS ),
                          { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_WRITE_BIT,
                          VK_IMAGE_LAYOUT_GENERAL } );
    cb->flush_barriers();

    cb->bind_pipeline( spatial_sampling_pipeline.pipeline );
    cb->bind_descriptor_set( { renderer->gpu->bindless_descriptor_set, descriptor_set },
                              { context.render_blackboard->scene_cb_offset, constants_offset, render_blackboard.lighting.lighting_constants_cb_offset } );
    cb->dispatch( ( render_blackboard.render_width / 2 + 7 ) / 8, ( render_blackboard.render_height / 2 + 7 ) / 8, 1 );

    // cb->bind_pipeline( temporal_accumulation_pipeline.pipeline );
    // cb->bind_descriptor_set( { renderer->gpu->bindless_descriptor_set, descriptor_set },
    //                           { context.render_blackboard->scene_cb_offset, constants_offset, render_blackboard.lighting.lighting_constants_cb_offset } );
    // cb->dispatch( ( render_blackboard.render_width / 2 + 7 ) / 8, ( render_blackboard.render_height / 2 + 7 ) / 8, 1 );

    cb->add_image_barrier( output_indirect_texture, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS ),
                          { VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                            VK_ACCESS_2_TRANSFER_READ_BIT,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL } );
    cb->add_image_barrier( output_history_texture, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS ),
                          { VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                            VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL } );

    // NOTE(marco): re-enabled for a biased version of the algorithm
    // cb->add_buffer_barrier( temporal_reservoir_buffer_write,
    //     0,
    //     reservoir_buffer_size,
    //     {
    //         .stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
    //         .access = VK_ACCESS_2_TRANSFER_WRITE_BIT
    //     });
    // cb->add_buffer_barrier( spatial_reservoir_buffer,
    //     0,
    //     reservoir_buffer_size,
    //     {
    //         .stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
    //         .access = VK_ACCESS_2_TRANSFER_READ_BIT
    //     });

    // cb->flush_barriers();

    // cb->copy_buffer( spatial_reservoir_buffer, 0, temporal_reservoir_buffer_write, 0, reservoir_buffer_size );

    cb->copy_image( output_indirect_texture, output_history_texture, {
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_READ_BIT,
        VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL
    } );
}

void ReSTIRGIPass::on_resize(FrameGraphResourceContext& context, u32 new_width, u32 new_height ) {
    if ( !enabled ) {
        return;
    }

    GpuDevice& gpu = *renderer->gpu;

    const u32 adjusted_width = ceilu32( new_width * texture_scale );
    const u32 adjusted_height = ceilu32( new_height * texture_scale );

    reservoir_buffer_size = adjusted_width * adjusted_height * sizeof( Reservoir );

    gpu.resize_buffer( spatial_reservoir_buffer, reservoir_buffer_size );
    gpu.resize_buffer( temporal_reservoir_buffer[0], reservoir_buffer_size );
    gpu.resize_buffer( temporal_reservoir_buffer[1], reservoir_buffer_size );
    gpu.resize_image( output_texture, adjusted_width, adjusted_height, 1 );
    gpu.resize_image( output_indirect_texture, adjusted_width, adjusted_height, 1 );
    gpu.resize_image( output_history_texture, adjusted_width, adjusted_height, 1 );

    gpu.recreate_image_view( output_image_view );
    gpu.recreate_image_view( output_indirect_image_view );
    gpu.recreate_image_view( output_history_image_view );

    gpu.add_image_view_to_bindless( output_image_view );
    gpu.add_image_view_to_bindless( output_indirect_image_view );
    gpu.add_image_view_to_bindless( output_history_image_view );

    reset_history = true;

    create_descriptors( context );
}

void ReSTIRGIPass::create_gpu_resources( FrameGraphResourceContext& context ) {
    FrameGraph* frame_graph = context.frame_graph;
    RenderScene& scene = *context.render_scene;
    RenderBlackboard& render_blackboard = *context.render_blackboard;

    FrameGraphNode* node = frame_graph->get_node( "restirgi_pass" );
    if ( node == nullptr ) {
        enabled = false;

        return;
    }

    enabled = node->enabled;
    if ( !enabled ) {
        return;
    }

    GpuDevice& gpu = *renderer->gpu;

    cache_restirgi_common_resources( frame_graph, &resources );

    ImageCreation texture_creation{ };
    const u32 adjusted_width = ceilu32( render_blackboard.render_width * texture_scale );
    const u32 adjusted_height = ceilu32( render_blackboard.render_height * texture_scale );
    texture_creation.set_size( adjusted_width, adjusted_height, 1 )
        .set_format_type( VK_FORMAT_R16G16B16A16_SFLOAT, TextureType::Texture2D )
        .set_mips( 1 )
        .set_layers( 1 )
        .set_flags( TextureFlags::Compute_mask )
        .set_name( "restirgi_output" );

    output_texture = gpu.create_image( texture_creation );

    output_image_view = gpu.create_image_view( {
        .parent_image = output_texture, .view_type = VK_IMAGE_VIEW_TYPE_2D,
        .sub_resource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }, .name = texture_creation.name } );
    gpu.add_image_view_to_bindless( output_image_view );

    texture_creation.name = "restir_output_history";
    output_history_texture = gpu.create_image( texture_creation );
    output_history_image_view = gpu.create_image_view( {
        .parent_image = output_history_texture, .view_type = VK_IMAGE_VIEW_TYPE_2D,
        .sub_resource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }, .name = texture_creation.name } );
    gpu.add_image_view_to_bindless( output_history_image_view );

    texture_creation.name = "restir_indirect_output";
    output_indirect_texture = gpu.create_image( texture_creation );
    output_indirect_image_view = gpu.create_image_view( {
        .parent_image = output_indirect_texture, .view_type = VK_IMAGE_VIEW_TYPE_2D,
        .sub_resource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }, .name = texture_creation.name } );
    gpu.add_image_view_to_bindless( output_indirect_image_view );

    FrameGraphResource* restir_output_texture = ( FrameGraphResource* )context.frame_graph->get_resource( "restirgi_output" );
    RASSERT( restir_output_texture );

    restir_output_texture->resource_info.set_external_texture_2d(
        adjusted_width, adjusted_height,
        VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
        output_indirect_texture, output_indirect_image_view );

    reservoir_buffer_size = adjusted_width * adjusted_height * sizeof( Reservoir );
    BufferCreation buffer_creation{
        .size = reservoir_buffer_size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .name = "restirgi_spatial_reservoir_buffer"
    };

    spatial_reservoir_buffer = gpu.create_buffer( buffer_creation );

    buffer_creation.name = "restirgi_temporal_reservoir_buffer_0";
    temporal_reservoir_buffer[ 0 ] = gpu.create_buffer( buffer_creation );

    buffer_creation.name = "restirgi_temporal_reservoir_buffer_1";
    temporal_reservoir_buffer[ 1 ] = gpu.create_buffer( buffer_creation );

    reset_history = true;
}

void ReSTIRGIPass::upload_gpu_data( FrameGraphResourceContext& context ) {
    if ( !enabled ) {
        return;
    }

    GpuDevice& gpu = *renderer->gpu;
    RenderScene& scene = *context.render_scene;
    RenderBlackboard& render_blackboard = *context.render_blackboard;

    ReSTIRGIConstants* gpu_constants = gpu.dynamic_buffer_allocate<ReSTIRGIConstants>( &constants_offset );
    if ( gpu_constants ) {

        gpu_constants->albedo_texture_index = resources.albedo_image_view.index();
        gpu_constants->normal_texture_index = resources.normals_image_view.index();
        gpu_constants->orm_texture_index = resources.orm_image_view.index();
        gpu_constants->depth_texture_index = resources.depth_image_view.index();

        gpu_constants->sbt_offset = 0; // shader binding table offset
        gpu_constants->sbt_stride = renderer->gpu->ray_tracing_pipeline_properties.shaderGroupHandleAlignment; // shader binding table stride
        gpu_constants->miss_index = 0;
        gpu_constants->output_texture_index = output_image_view.index();

        gpu_constants->linear_depth_index = resources.linear_depth_image_view.index();
        gpu_constants->linear_depth_history_index = resources.linear_depth_history_image_view.index();
        gpu_constants->normal_history_index = resources.normal_history_image_view.index();
        gpu_constants->motion_vectors_index = resources.motion_vectors_image_view.index();

        gpu_constants->mesh_id_index = resources.mesh_id_image_view.index();
        gpu_constants->mesh_id_history_index = resources.mesh_id_history_image_view.index();
        gpu_constants->output_history_texture_index = output_history_image_view.index();
        gpu_constants->output_indirect_texture_index = output_indirect_image_view.index();

        // TODO(marco): read from scene
        glm::vec3 light_position = glm::vec3{ 0.0f, 4.0f, 0.0f };
        float light_radius = 20.0f;
        float light_intensity = 80.0f;

        gpu_constants->light[0] = light_position.x;
        gpu_constants->light[1] = light_position.y;
        gpu_constants->light[2] = light_position.z;
        gpu_constants->light[3] = 0.0f;

        gpu_constants->light_range = light_radius;
        gpu_constants->light_intensity = light_intensity;

        gpu_constants->resolution[0] = render_blackboard.render_width;
        gpu_constants->resolution[1] = render_blackboard.render_height;
    }
}

void ReSTIRGIPass::destroy_gpu_resources( FrameGraphResourceContext& context ) {
    if ( !enabled ) {
        return;
    }

    GpuDevice& gpu = *renderer->gpu;

    gpu.destroy_buffer( spatial_reservoir_buffer );
    gpu.destroy_buffer( temporal_reservoir_buffer[ 0 ] );
    gpu.destroy_buffer( temporal_reservoir_buffer[ 1 ] );
    gpu.destroy_image( resources.normal_history_texture );
    gpu.destroy_image_view( resources.normal_history_image_view );
    gpu.destroy_image( resources.linear_depth_history_texture );
    gpu.destroy_image_view( resources.linear_depth_history_image_view );
    gpu.destroy_image( resources.mesh_id_history_texture );
    gpu.destroy_image_view( resources.mesh_id_history_image_view );
    gpu.destroy_image_view( output_image_view );
    gpu.destroy_image( output_texture );
    gpu.destroy_image_view( output_history_image_view );
    gpu.destroy_image( output_history_texture );
    gpu.destroy_image_view( output_indirect_image_view );
    gpu.destroy_image( output_indirect_texture );
    gpu.destroy_descriptor_set( descriptor_set_ping );
    gpu.destroy_descriptor_set( descriptor_set_pong );
}

void ReSTIRGIPass::update_dependent_resources( FrameGraphResourceContext& context ) {

}

void ReSTIRGIPass::create_descriptors( FrameGraphRenderContext& context ) {

    GpuDevice* gpu = renderer->gpu;
    RenderBlackboard& render_blackboard = *context.render_blackboard;

    ShaderReflectionInfo* reflection_info = renderer->get_shader_reflection( sample_generation_pipeline.pipeline );

    DescriptorSetBinder descriptors;
    descriptors.dynamic_buffers.push( { 1, sizeof( ReSTIRGIConstants ) } );
    descriptors.bind_ssbo( spatial_reservoir_buffer, 30 );
    descriptors.bind_ssbo( temporal_reservoir_buffer[ 0 ], 31 );
    descriptors.bind_ssbo( temporal_reservoir_buffer[ 1 ], 32 );
    descriptors.name = "restirgi_ds_ping";
    descriptor_set_ping = renderer->create_descriptor_set( descriptors, reflection_info, sample_generation_pipeline.pipeline, 0, render_blackboard );

    descriptors.reset();
    descriptors.dynamic_buffers.push( { 1, sizeof( ReSTIRGIConstants ) } );
    descriptors.bind_ssbo( spatial_reservoir_buffer, 30 );
    descriptors.bind_ssbo( temporal_reservoir_buffer[ 1 ], 31 );
    descriptors.bind_ssbo( temporal_reservoir_buffer[ 0 ], 32 );
    descriptors.name = "restirgi_ds_pong";
    descriptor_set_pong = renderer->create_descriptor_set( descriptors, reflection_info, sample_generation_pipeline.pipeline, 0, render_blackboard );
}

void ReSTIRGIPass::create_descriptors( FrameGraphResourceContext& context ) {

    GpuDevice* gpu = renderer->gpu;
    RenderBlackboard& render_blackboard = *context.render_blackboard;

    ShaderReflectionInfo* reflection_info = renderer->get_shader_reflection( sample_generation_pipeline.pipeline );

    DescriptorSetBinder descriptors;
    descriptors.dynamic_buffers.push( { 1, sizeof( ReSTIRGIConstants ) } );
    descriptors.bind_ssbo( spatial_reservoir_buffer, 30 );
    descriptors.bind_ssbo( temporal_reservoir_buffer[ 0 ], 31 );
    descriptors.bind_ssbo( temporal_reservoir_buffer[ 1 ], 32 );
    descriptors.name = "restirgi_ds_ping";
    descriptor_set_ping = renderer->create_descriptor_set( descriptors, reflection_info, sample_generation_pipeline.pipeline, 0, render_blackboard );

    descriptors.reset();
    descriptors.dynamic_buffers.push( { 1, sizeof( ReSTIRGIConstants ) } );
    descriptors.bind_ssbo( spatial_reservoir_buffer, 30 );
    descriptors.bind_ssbo( temporal_reservoir_buffer[ 1 ], 31 );
    descriptors.bind_ssbo( temporal_reservoir_buffer[ 0 ], 32 );
    descriptors.name = "restirgi_ds_pong";
    descriptor_set_pong = renderer->create_descriptor_set( descriptors, reflection_info, sample_generation_pipeline.pipeline, 0, render_blackboard );
}

} // namespace raptor
