#include "graphics/render_passes/depth_of_field_pass.hpp"
#include "graphics/render_scene.hpp"

#include "external/imgui/imgui.h"

namespace raptor {

//
// DoFPass ////////////////////////////////////////////////////////////////
void DoFPass::add_ui() {
    if ( !enabled )
        return;

    ImGui::InputFloat( "Focal Length", &focal_length );
    ImGui::InputFloat( "Plane in Focus", &plane_in_focus );
    ImGui::InputFloat( "Aperture", &aperture );
}

void DoFPass::pre_render( FrameGraphRenderContext& context ) {

    RenderScene* render_scene = context.render_view->scene;
    u32 current_frame_index = context.current_frame_index;
    CommandBuffer* gpu_commands = context.gpu_commands;

    FrameGraphResource* texture = ( FrameGraphResource* )context.frame_graph->get_resource( "lighting" );
    RASSERT( texture != nullptr );

    //gpu_commands->copy_image( texture->resource_info.texture.image, scene_mips->image, RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
}

void DoFPass::render( FrameGraphRenderContext& context ) {
    if ( !enabled )
        return;

    RASSERT( false );

    /*RenderScene* render_scene = context.render_view->scene;
    u32 current_frame_index = context.current_frame_index;
    CommandBuffer* gpu_commands = context.gpu_commands;

    PipelineHandle pipeline = renderer->get_pipeline( material, 0 );

    gpu_commands->bind_pipeline( pipeline );
    gpu_commands->bind_vertex_buffer( renderer->gpu->get_fullscreen_vertex_buffer(), 0, 0 );
    gpu_commands->bind_descriptor_set( { renderer->gpu->bindless_descriptor_set, descriptor_set },
                                        { constants_offset } );

    gpu_commands->draw( TopologyType::Triangle, 0, 3, 0, 1 );*/
}

//TODO:
static ImageCreation dof_scene_tc;

void DoFPass::on_resize( FrameGraphResourceContext& context, u32 new_width, u32 new_height ) {
    if ( !enabled ) return;

    u32 w = new_width;
    u32 h = new_height;

    u32 mips = 1;
    while ( w > 1 && h > 1 ) {
        w /= 2;
        h /= 2;
        mips++;
    }

    // Destroy scene mips
    {
        renderer->destroy_texture( scene_mips );

        // Reuse cached texture creation and create new scene mips.
        dof_scene_tc.set_mips( mips ).set_size( new_width, new_height, 1 );
        scene_mips = renderer->create_texture( dof_scene_tc );
    }
}

void DoFPass::create_gpu_resources( FrameGraphResourceContext& context ) {
    FrameGraph* frame_graph = context.frame_graph;
    RenderScene& scene = *context.render_scene;

    FrameGraphNode* node = frame_graph->get_node( "depth_of_field_pass" );
    if ( node == nullptr ) {
        enabled = false;

        return;
    }

    enabled = node->enabled;
    if ( !enabled )
        return;

    const u64 hashed_name = hash_calculate( "depth_of_field" );
    RASSERT( false );
    //GpuTechnique* main_technique = renderer->resource_cache.techniques.get( hashed_name );

    //MaterialCreation material_creation;
    //material_creation.set_name( "material_dof" ).set_technique( main_technique ).set_render_index( 0 );

    /*DescriptorSetLayoutHandle layout = renderer->gpu->get_descriptor_set_layout( main_technique->passes[ 0 ].pipeline, k_material_descriptor_set_index );
    descriptor_set = renderer->gpu->create_descriptor_set( {
        .dynamic_buffers = {0, sizeof( DoFData )},
        .layout = layout } );

    FrameGraphResource* color_texture = frame_graph->access_resource( node->inputs[ 0 ] );
    FrameGraphResource* depth_texture_reference = frame_graph->access_resource( node->inputs[ 1 ] );

    depth_texture = frame_graph->get_resource( depth_texture_reference->name );
    RASSERT( depth_texture != nullptr );

    FrameGraphResourceInfo& info = color_texture->resource_info;
    u32 w = info.texture.width;
    u32 h = info.texture.height;

    u32 mips = 1;
    while ( w > 1 && h > 1 ) {
        w /= 2;
        h /= 2;
        mips++;
    }

    dof_scene_tc.set_data( nullptr ).set_format_type( info.texture.format, TextureType::Texture2D ).set_mips( mips ).set_size( ( u16 )info.texture.width, ( u16 )info.texture.height, 1 ).set_name( "scene_mips" );
    {
        scene_mips = renderer->create_texture( dof_scene_tc );
    }

    znear = 0.1f;
    zfar = 1000.0f;
    focal_length = 5.0f;
    plane_in_focus = 1.0f;
    aperture = 8.0f;*/
}

void DoFPass::upload_gpu_data( FrameGraphResourceContext& context ) {
    if ( !enabled )
        return;

    u32 current_frame_index = renderer->gpu->current_frame;

    DoFData* dof_data = renderer->gpu->dynamic_buffer_allocate<DoFData>( &constants_offset );
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

void DoFPass::destroy_gpu_resources( FrameGraphResourceContext& context ) {
    if ( !enabled )
        return;

    renderer->destroy_texture( scene_mips );

    renderer->gpu->destroy_descriptor_set( descriptor_set );
}

} // namespace raptor
