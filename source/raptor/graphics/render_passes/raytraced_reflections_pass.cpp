
#include "graphics/render_passes/raytraced_reflections_pass.hpp"
#include "graphics/render_passes/ddgi_pass.hpp"
#include "graphics/render_scene.hpp"
#include "graphics/render_blackboard.hpp"

#include "foundation/numerics.hpp"

namespace raptor {

// RaytracedReflectionsPass ///////////////////////////////////////////////////////////
void RaytracedReflectionsPass::declare_frame_graph_node( FrameGraphResourceContext& context ) {
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
                .handle = builder.get_output_handle( "gbuffer_pass_late", "gbuffer_normals" )
            },
            {
                .type = FrameGraphResourceType_Texture,
                .handle = builder.get_output_handle( "gbuffer_pass_late", "gbuffer_metallic_roughness_occlusion" )
            },
        },
        .outputs = {
            builder.create_output_handle( {
                .type = FrameGraphResourceType_Texture,
                .resource_info = {
                    .external = true
                },
                .name = "reflections",
            } ),
        },
        .scheduling = { CommandQueueType::Graphics, 0 },
        .enabled = true,
        .compute = true,
        .name = "reflections_pass" } );
}

void RaytracedReflectionsPass::pre_render( FrameGraphRenderContext& context ) {
    if ( !enabled )
        return;
}

i32 generate_brdf_counter = 10;

void RaytracedReflectionsPass::render( FrameGraphRenderContext& context ) {
    if ( !enabled )
        return;

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

    RenderScene* render_scene = context.render_view->scene;
    u32 current_frame_index = context.current_frame_index;
    CommandBuffer* gpu_commands = context.gpu_commands;

    if ( generate_brdf_counter > 0 ) {

        --generate_brdf_counter;

        gpu_commands->add_image_barrier( brdf_lut_image, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS ),
                                         { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                           VK_ACCESS_2_SHADER_WRITE_BIT,
                                           VK_IMAGE_LAYOUT_GENERAL } );
        gpu_commands->flush_barriers();

        gpu_commands->bind_pipeline( brdf_lut_generation_pipeline.pipeline );
        gpu_commands->bind_descriptor_set(
            { renderer->gpu->bindless_descriptor_set, brdf_lut_generation_descriptor_set },
            { render_blackboard.scene_cb_offset } );

        u32 push_constants[] = { brdf_lut_image_view.index(), 512 };
        gpu_commands->push_constants( brdf_lut_generation_pipeline.pipeline, 0, 8, &push_constants );

        gpu_commands->dispatch( 512 / 8, 512 / 8, 1 );

        gpu_commands->add_image_barrier( brdf_lut_image, range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS ),
                                         { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                           VK_ACCESS_2_SHADER_READ_BIT,
                                           VK_IMAGE_LAYOUT_GENERAL } );
        gpu_commands->flush_barriers();
    }

    // TODO(marco): clear
    //gpu_commands->issue_texture_barrier( reflections_image, RESOURCE_STATE_UNORDERED_ACCESS, 0, 1 );
    gpu_commands->bind_pipeline( reflections_pipeline.pipeline );
    gpu_commands->bind_descriptor_set(
        { renderer->gpu->bindless_descriptor_set, reflections_descriptor_set },
        { render_blackboard.scene_cb_offset, render_blackboard.lighting.lighting_constants_cb_offset,
          constants_offset/*, render_blackboard.ddgi_constants_offset */ } );

    f32 push_constants = 1.f / texture_scale;
    gpu_commands->push_constants( reflections_pipeline.pipeline, 0, 4, &push_constants );

    gpu_commands->trace_rays( reflections_pipeline.pipeline, ceilu32( render_blackboard.render_width * texture_scale ), ceilu32( render_blackboard.render_height * texture_scale ), 1 );
}

void RaytracedReflectionsPass::on_resize( FrameGraphResourceContext& context, u32 new_width, u32 new_height ) {
    if ( !enabled ) {
        return;
    }

    GpuDevice& gpu = *renderer->gpu;

    gpu.resize_image( reflections_image, u32( new_width * texture_scale ), u32( new_height * texture_scale ) );
    gpu.recreate_image_view( reflections_image_view );
    gpu.add_image_view_to_bindless( reflections_image_view );
}

void RaytracedReflectionsPass::create_gpu_resources( FrameGraphResourceContext& context ) {

    FrameGraph* frame_graph = context.frame_graph;
    RenderScene& scene = *context.render_scene;

    FrameGraphNode* node = frame_graph->get_node( "reflections_pass" );
    if ( node == nullptr ) {
        enabled = false;

        return;
    }

    enabled = node->enabled;
    if ( !enabled ) {
        return;
    }

    GpuDevice& gpu = *renderer->gpu;
    RenderBlackboard& render_blackboard = *context.render_blackboard;

    // Cache normals texture
    // TODO: texture can be null
    FrameGraphResource* resource = frame_graph->get_resource( "svgf_current_normals" );
    RASSERT( resource );
    normals_image_view = resource->resource_info.texture.image_view;

    resource = frame_graph->get_resource( "gbuffer_metallic_roughness_occlusion" );
    roughness_image_view = resource->resource_info.texture.image_view;

    // resource = frame_graph->get_resource( "indirect_lighting" );
    // indirect_image_view = resource->resource_info.texture.image_view;

    texture_scale = context.render_config->raytraced_reflections.reflections_scale;

    ImageCreation texture_creation{ };
    u32 adjusted_width = ceilu32( render_blackboard.render_width * texture_scale );
    u32 adjusted_height = ceilu32( render_blackboard.render_height * texture_scale );
    texture_creation.set_size( adjusted_width, adjusted_height, 1 ).set_format_type( VK_FORMAT_B10G11R11_UFLOAT_PACK32, TextureType::Texture2D ).set_mips( 1 ).set_layers( 1 ).set_flags( TextureFlags::Compute_mask ).set_name( "reflections_texture" );

    reflections_image = gpu.create_image( texture_creation );

    reflections_image_view = gpu.create_image_view( {
            .parent_image = reflections_image, .view_type = VK_IMAGE_VIEW_TYPE_2D,
            .sub_resource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }, .name = texture_creation.name } );
    gpu.add_image_view_to_bindless( reflections_image_view );

    resource = frame_graph->get_resource( "reflections" );
    resource->resource_info.set_external_texture_2d( adjusted_width, adjusted_height, VK_FORMAT_B10G11R11_UFLOAT_PACK32, 0, reflections_image, reflections_image_view );

    // Create BRDF Lut texture
    texture_creation.reset().set_size( 512, 512, 1 ).set_format_type( VK_FORMAT_R16G16_SFLOAT, TextureType::Texture2D )
        .set_name( "brdf_lut" ).set_flags( TextureFlags::Compute_mask );
    brdf_lut_image = gpu.create_image( texture_creation );

    brdf_lut_image_view = gpu.create_image_view( {
            .parent_image = brdf_lut_image, .view_type = VK_IMAGE_VIEW_TYPE_2D,
            .sub_resource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }, .name = texture_creation.name } );
    gpu.add_image_view_to_bindless( brdf_lut_image_view );

    render_blackboard.brdf_lut_image_view = brdf_lut_image_view;
}

void RaytracedReflectionsPass::upload_gpu_data( FrameGraphResourceContext& context ) {
    if ( !enabled )
        return;

    GpuDevice& gpu = *renderer->gpu;

    GpuReflectionsConstants* gpu_constants = gpu.dynamic_buffer_allocate<GpuReflectionsConstants>( &constants_offset );
    if ( gpu_constants ) {
        gpu_constants->sbt_offset = 0;
        gpu_constants->sbt_stride = renderer->gpu->ray_tracing_pipeline_properties.shaderGroupHandleAlignment;
        gpu_constants->miss_index = 0;
        gpu_constants->out_image_index = reflections_image_view.index();

        gpu_constants->gbuffer_texures[ 0 ] = roughness_image_view.index();
        gpu_constants->gbuffer_texures[ 1 ] = normals_image_view.index();
        // gpu_constants->gbuffer_texures[ 2 ] = indirect_image_view.index();
    }
}

void RaytracedReflectionsPass::destroy_gpu_resources( FrameGraphResourceContext& context ) {
    if ( !enabled )
        return;

    GpuDevice& gpu = *renderer->gpu;

    gpu.destroy_image( brdf_lut_image );
    gpu.destroy_image( reflections_image );

    gpu.destroy_image_view( brdf_lut_image_view );
    gpu.destroy_image_view( reflections_image_view );

    gpu.destroy_descriptor_set( reflections_descriptor_set );
    gpu.destroy_descriptor_set( brdf_lut_generation_descriptor_set );
}

void RaytracedReflectionsPass::update_psos( FrameGraphResourceContext& context, PipelineUpdatePhase phase ) {
    Renderer* renderer = context.renderer;

    if ( phase == PipelineUpdatePhase::Destroy ) {
        renderer->destroy_ray_tracing_pipeline_state( reflections_pipeline );
        renderer->destroy_compute_pipeline_state( brdf_lut_generation_pipeline );

        return;
    }

    // Ray tracing pipelines
    RayTracingPipelineTransaction rt_transaction( renderer );

    RayTracingPipelineState& sample_pipeline = rt_transaction.add( reflections_pipeline );

    renderer->create_raytracing_pipeline_state(
        {
        .stages = {
            {
                .source_file_path = "glsl/reflections.glsl",
                .type = VK_SHADER_STAGE_RAYGEN_BIT_KHR,
            },
            {
                .source_file_path = "glsl/reflections.glsl",
                .type = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
            },
            {
                .source_file_path = "glsl/reflections.glsl",
                .type = VK_SHADER_STAGE_MISS_BIT_KHR,
            },
        },
        .name = "reflections_rt" },
        {
            .name = "rt_reflections",
            .render_pass_name = "reflections_pass",
        },
        "reflections",
        context.frame_graph, sample_pipeline );

    rt_transaction.commit_or_rollback();

    ComputePipelineTransaction compute_transaction( renderer );

    ComputePipelineState& brdf_lut_generation = compute_transaction.add( brdf_lut_generation_pipeline );

    renderer->create_compute_pipeline_state(
        {
        .stages = {
            {
                .source_file_path = "glsl/reflections.glsl",
                .type = VK_SHADER_STAGE_COMPUTE_BIT,
            },
        },
        .name = "brdf_lut_generation" },
        {
            .name = "brdf_lut_generation",
            .render_pass_name = "reflections_pass",
        },
        "reflections",
        context.frame_graph, brdf_lut_generation );

    compute_transaction.commit_or_rollback();
}

void RaytracedReflectionsPass::update_dependent_resources( FrameGraphResourceContext& context ) {

    if ( !enabled )
        return;
}

void RaytracedReflectionsPass::create_descriptors( FrameGraphRenderContext& context ) {

    GpuDevice* gpu = renderer->gpu;
    RenderBlackboard& render_blackboard = *context.render_blackboard;

    ShaderReflectionInfo* reflection_info = renderer->get_shader_reflection( reflections_pipeline.pipeline );

    DescriptorSetBinder descriptors;
    descriptors.dynamic_buffers.push( { 40, sizeof( GpuReflectionsConstants ) } );
    // descriptors.dynamic_buffers.push( { 55, sizeof( GpuDDGIConstants ) } );
    descriptors.name = "rt_reflections_ds";

    reflections_descriptor_set = renderer->create_descriptor_set( descriptors, reflection_info, reflections_pipeline.pipeline, 0, render_blackboard );

    // BRDF LUT generation
    reflection_info = renderer->get_shader_reflection( brdf_lut_generation_pipeline.pipeline );

    descriptors.reset();
    descriptors.name = "brdf_lut_generation_ds";

    brdf_lut_generation_descriptor_set = renderer->create_descriptor_set( descriptors, reflection_info, brdf_lut_generation_pipeline.pipeline, 0, render_blackboard );;
}

} // namespace raptor
