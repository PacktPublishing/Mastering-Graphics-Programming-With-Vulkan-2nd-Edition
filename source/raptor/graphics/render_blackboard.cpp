#include "graphics/render_blackboard.hpp"
#include "graphics/raptor_imgui.hpp"
#include "graphics/scene_graph.hpp"
#include "graphics/frame_renderer.hpp"
#include "graphics/gpu_device.hpp"

#include <vulkan/vk_enum_string_helper.h>

#include "external/imgui/imgui.h"

#include "graphics/render_scene.hpp"
#include "foundation/numerics.hpp"

namespace raptor {

void LightingRenderConfig::draw_lights_imgui( Span<Light> lights ) {

    if ( ImGui::CollapsingHeader( "Lights" ) ) {
        ImGui::PushID( "LightingRenderConfig::Lights" );

        const u32 light_count = ( u32 )lights.size;

        ImGui::Text( "Active lights: %u", light_count );
        ImGui::Checkbox( "Light Edit Debug Draws", &show_light_edit_debug_draws );

        if ( light_count > 0 ) {
            selected_light_index = raptor::min( selected_light_index, light_count - 1 );

            ImGui::SliderUint( "Light Index", &selected_light_index, 0, light_count - 1 );

            Light& light = lights.data[ selected_light_index ];

            ImGui::SliderFloat3( "Light position", &light.world_position[ 0 ], -10.f, 10.f, "%2.3f" );
            ImGui::SliderFloat( "Light radius", &light.radius, 0.01f, 30.f, "%2.3f" );
            ImGui::SliderFloat( "Light intensity", &light.intensity, 0.01f, 30.f, "%2.3f" );

            f32 color[ 3 ] = { light.color.x, light.color.y, light.color.z };

            ImGui::ColorEdit3( "Light color", color );
            light.color = { color[ 0 ], color[ 1 ], color[ 2 ] };

            ImGui::SeparatorText( "Shadow" );

            const u32 mip = ( u32 )light.shadow_mip_level;
            RASSERT( mip < 3 );

            ImGui::Text( "Desired resolution: %u", light.shadow_map_resolution );
            ImGui::Text( "Mip level: %u", mip );
            ImGui::Text( "Resolution: %u", 512u >> mip );
            ImGui::Text( "Projected radius: %.1f px", light.projected_radius );
            ImGui::Text( "Shadow / projected radius: %.2f", light.shadow_map_resolution / raptor::max( light.projected_radius, 1.0f ) );
        } else {
            ImGui::TextUnformatted( "No lights" );
        }

        ImGui::SeparatorText( "Test lights" );

        if ( ImGui::Button( "Sponza light test" ) ) {
            load_shadow_test_lights = true;
        }

        if ( ImGui::Button( "Sponza light test advanced" ) ) {
            load_shadow_test_lights_adv = true;
        }

        ImGui::Checkbox( "Animate lights", &animate_lights );

        ImGui::PopID();
    }
}

void LightingRenderConfig::draw_imgui() {

    if ( ImGui::CollapsingHeader( "Clustered Lighting" ) ) {
        ImGui::PushID( "LightingRenderConfig::Clustered" );

        ImGui::Checkbox( "Enable Camera Inside approximation", &enable_camera_inside );
        ImGui::Checkbox( "Use McGuire method for AABB sphere", &use_mcguire_method );
        ImGui::Checkbox( "Skip invisible lights", &skip_invisible_lights );
        ImGui::Checkbox( "force fullscreen light aabb", &force_fullscreen_light_aabb );
        ImGui::Checkbox( "debug show light tiles", &debug_show_light_tiles );
        ImGui::Checkbox( "debug show tiles", &debug_show_tiles );
        ImGui::Checkbox( "debug show bins", &debug_show_bins );
        ImGui::SliderUint( "Lighting debug modes", &lighting_debug_modes, 0, 10 );

        ImGui::PopID();
    }
}

void GpuCullingRenderConfig::draw_imgui() {

    if ( ImGui::CollapsingHeader( "Gpu Culling" ) ) {
        ImGui::PushID( "GpuCullingRenderConfig" );

        ImGui::Checkbox( "Use frustum cull for meshes", &enable_frustum_cull_meshes );
        ImGui::Checkbox( "Use frustum cull for meshlets", &enable_frustum_cull_meshlets );
        ImGui::Checkbox( "Use occlusion cull for meshes", &enable_occlusion_cull_meshes );
        ImGui::Checkbox( "Use occlusion cull for meshlets", &enable_occlusion_cull_meshlets );
        //ImGui::Checkbox( "Use meshes sphere cull for shadows", &shadow_meshes_sphere_cull );
        ImGui::Checkbox( "Use meshlets cone cull for shadows", &shadow_meshlets_cone_cull );
        ImGui::Checkbox( "Use meshlets sphere cull for shadows", &shadow_meshlets_sphere_cull );
        ImGui::Checkbox( "Use meshlets cubemap face cull for shadows", &shadow_meshlets_cubemap_face_cull );
        ImGui::Checkbox( "Freeze occlusion camera", &freeze_occlusion_camera );

        ImGui::PopID();
    }
}

u32 GpuCullingRenderConfig::pack_culling_options() const {
    u32 options = 0u;
    options |= enable_frustum_cull_meshes ? k_culling_frustum_meshes : 0u;
    options |= enable_frustum_cull_meshlets ? k_culling_frustum_meshlets : 0u;
    options |= enable_occlusion_cull_meshes ? k_culling_occlusion_meshes : 0u;
    options |= enable_occlusion_cull_meshlets ? k_culling_occlusion_meshlets : 0u;
    options |= freeze_occlusion_camera ? k_culling_freeze_occlusion_camera : 0u;
    options |= shadow_meshlets_cone_cull ? k_culling_shadow_meshlets_cone : 0u;
    options |= shadow_meshlets_sphere_cull ? k_culling_shadow_meshlets_sphere : 0u;
    options |= shadow_meshlets_cubemap_face_cull ? k_culling_shadow_meshlets_cubemap_face : 0u;
    return options;
}

void ShadowRenderConfig::draw_imgui( const PointlightShadowsRuntimeData& shadows ) {

    if ( ImGui::CollapsingHeader( "Shadows" ) ) {
        ImGui::PushID( "ShadowRenderConfig" );

        ImGui::Checkbox( "Disable Shadows", &disable_shadows );

        ImGui::Checkbox( "Force shadow mip", &force_shadow_mip );

        if ( force_shadow_mip ) {
            ImGui::SliderUint( "Shadow mip", &forced_shadow_mip, 0, 2 );
            ImGui::TextUnformatted( "0: 512    1: 256    2: 128" );
        }
        ImGui::SliderFloat( "Shadow Resolution Scale", &shadow_resolution_scale, 0.f, 10.f );

        ImGui::SliderFloat( "Depth Bias Constant", &depth_bias_constant, 0.f, 10.f );
        ImGui::SliderFloat( "Depth Bias Clamp", &depth_bias_clamp, 0.f, 1.f );
        ImGui::SliderFloat( "Depth Bias Slope", &depth_bias_slope, 0.f, 10.f );
        ImGui::Checkbox( "Use mip scale for slope", &use_slope_mip_scale );

        ImGui::SliderUint( "PCF Samples", &pcf_samples, 1, 16 );
        ImGui::SliderFloat( "PCF Radius", &pcf_radius, 0.0f, 4.0f, "%.2f" );

        ImGui::SeparatorText( "Memory" );

        const f32 to_mib = 1.0f / ( 1024.0f * 1024.0f );

        ImGui::Text( "Resident pages: %u / %u", shadows.resident_pages, shadows.max_mip0_pages );

        ImGui::Text( "Resident memory: %.2f MiB", shadows.resident_memory * to_mib );
        ImGui::Text( "All mip0 memory: %.2f MiB", shadows.max_mip0_memory * to_mib );
        ImGui::Text( "Allocated pool memory: %.2f MiB", shadows.allocated_memory * to_mib );

        const f32 savings = shadows.max_mip0_memory > 0 ? 1.0f - f32( shadows.resident_memory ) / f32( shadows.max_mip0_memory ) : 0.0f;

        ImGui::Text( "Residency saving: %.1f%%", savings * 100.0f );

        ImGui::Text( "Mip0 / Mip1 / Mip2: %u / %u / %u", shadows.mip_light_count[ 0 ], shadows.mip_light_count[ 1 ], shadows.mip_light_count[ 2 ] );

        ImGui::PopID();
    }
}

void MeshletsRenderConfig::draw_imgui() {

    if ( ImGui::CollapsingHeader( "Meshlets" ) ) {
        ImGui::PushID( "MeshletsRenderConfig" );

        ImGui::Text( "Mesh Shaders Extension Present: %s", gpu_mesh_shaders_extension_present ? "Yes" : "No" );
        static bool enable_meshlets = false;
        enable_meshlets = use_meshlets && gpu_mesh_shaders_extension_present;
        ImGui::Checkbox( "Use meshlets", &enable_meshlets );
        use_meshlets = enable_meshlets;
        ImGui::Checkbox( "Use meshlets emulation", &use_meshlets_emulation );

        ImGui::PopID();
    }
}

void PostProcessRenderConfig::draw_imgui() {
    if ( ImGui::CollapsingHeader( "Post-Process" ) ) {
        ImGui::PushID( "PostProcessRenderConfig" );

        static cstring tonemap_names[] = { "None", "ACES" };
        ImGui::Combo( "Tonemap", &tonemap_mode, tonemap_names, ArraySize( tonemap_names ) );
        ImGui::SliderFloat( "Exposure", &exposure, -4.0f, 4.0f );
        ImGui::SliderFloat( "Sharpening amount", &sharpening_amount, 0.0f, 4.0f );
        ImGui::SliderFloat( "Bloom Amount", &bloom_amount, 0.0f, 1.0f );
        ImGui::Checkbox( "Enable Magnifying Zoom", &enable_zoom );
        ImGui::Checkbox( "Block Magnifying Zoom Input", &block_zoom_input );
        ImGui::SliderUint( "Magnifying Zoom Scale", &zoom_scale, 2, 4 );

        ImGui::PopID();
    }
}

void DebugDrawRenderConfig::draw_imgui( RenderScene* scene, SceneGraph& scene_graph,
                                        DebugDrawRenderingFeature& debug_draw_feature ) {

    if ( ImGui::CollapsingHeader( "Debug Drawing" ) ) {
        ImGui::PushID( "DebugDrawRenderConfig" );

        ImGui::Checkbox( "Show Cpu Draws", &show_cpu_draws );
        ImGui::Checkbox( "Show Gpu Draws", &show_gpu_draws );
        ImGui::Checkbox( "Inspect Mesh Instance", &inspect_mesh_instance );
        if ( mesh_instances_count != u32_max && inspect_mesh_instance ) {
            ImGui::SliderUint( "Mesh Instance", &mesh_instance_index, 0, mesh_instances_count - 1 );
        }

        ImGui::PopID();
    }

    if ( scene ) {
        RenderScene& render_scene = *scene;
        mesh_instances_count = render_scene.mesh_instances.size;


        if ( inspect_mesh_instance ) {
            MeshInstance& mi = render_scene.mesh_instances[ mesh_instance_index ];
            Mesh& mesh = render_scene.meshes[ mi.mesh_index ];

            glm::mat4 world = scene_graph.world_matrices[ mi.scene_graph_node_index ];
            glm::vec4 world_min = world * glm::vec4( mesh.aabb[ 0 ], 1.f );
            glm::vec4 world_max = world * glm::vec4( mesh.aabb[ 1 ], 1.f );
            f32 scale = extract_scale( world ).x;

            debug_draw_feature.aabb( world_min, world_max, Color::white() );

            glm::vec4 sphere_center = glm::vec4{ mesh.bounding_sphere.x, mesh.bounding_sphere.y, mesh.bounding_sphere.z, 1.f };
            debug_draw_feature.sphere_wire( world * sphere_center, mesh.bounding_sphere.w * scale, Color::blue() );

            for ( u32 i = 0; i < mesh.meshlet_count; i++ ) {
                const GpuMeshlet& meshlet = render_scene.meshlets[ mesh.meshlet_offset + i ];
                glm::vec4 world_center = world * glm::vec4( meshlet.center, 1.f );

                f32 world_radius = scale * meshlet.radius;

                debug_draw_feature.sphere_wire( world_center, world_radius, Color::green() );
            }
        }
    }
}

// ImageViewerRenderConfig ///////////////////////////////////////////////

// "gbuffer_normals (42) 1280x800 R16G16_SFLOAT"
static void format_image_view_label( GpuDevice& gpu, ImageViewHandle view_handle, char* label, sizet label_size ) {

    const ImageView* view = gpu.get_image_view( view_handle );
    const Image* image = gpu.get_image( view->parent_image );

    cstring format_name = string_VkFormat( image->vk_format );
    if ( strncmp( format_name, "VK_FORMAT_", 10 ) == 0 ) {
        format_name += 10;
    }

    snprintf( label, label_size, "%s (%u) %ux%u %s", image->name, view_handle.index(),
              ( u32 )image->width, ( u32 )image->height, format_name );
}

// Views selectable by the viewer, in pool order.
static ImageViewHandle get_viewable_image_view( GpuDevice& gpu, const ImageViewerRenderingFeature& feature, u32 index ) {

    if ( gpu.image_views.active_elements.get_bit( index ) == 0 ) {
        return {};
    }

    const ImageViewHandle handle( ( u16 )index, gpu.image_views.generations[ index ] );
    return feature.is_debuggable( gpu, handle ) ? handle : ImageViewHandle{};
}

void ImageViewerRenderConfig::draw_imgui( GpuDevice& gpu, ImageViewerRenderingFeature& feature ) {

    ImGui::PushID( "ImageViewerRenderConfig" );

    const u32 first_view = gpu.dummy_image_view.index();
    const u32 view_count = gpu.image_views.size;

    // Image selection //////////////////////////////////////////////////
    char preview[ 256 ] = "None";
    if ( feature.is_debuggable( gpu, input ) ) {
        format_image_view_label( gpu, input, preview, sizeof( preview ) );
    }

    if ( ImGui::BeginCombo( "Image", preview ) ) {
        for ( u32 v = first_view; v < view_count; ++v ) {
            const ImageViewHandle handle = get_viewable_image_view( gpu, feature, v );
            if ( handle.is_invalid() ) {
                continue;
            }

            char label[ 256 ];
            format_image_view_label( gpu, handle, label, sizeof( label ) );

            const bool is_selected = ( handle == input );
            if ( ImGui::Selectable( label, is_selected ) ) {
                input = handle;
                mip = 0;
                zoom = 1.0f;
                pan = { 0.0f, 0.0f };
            }

            if ( is_selected ) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    if ( !feature.is_debuggable( gpu, input ) || feature.output_view.is_invalid() ) {
        ImGui::TextUnformatted( "Select an image." );
        ImGui::PopID();
        return;
    }

    const ImageView* view = gpu.get_image_view( input );
    const Image* image = gpu.get_image( view->parent_image );

    // Conversion controls ////////////////////////////////////////////////
    static cstring channel_names[] = { "RGB", "R", "G", "B", "A", "Luminance" };
    i32 channel = ( i32 )channel_mode;
    ImGui::Combo( "Channels", &channel, channel_names, ArraySize( channel_names ) );
    channel_mode = ( u32 )channel;

    ImGui::DragFloatRange2( "Range", &range_min, &range_max, 0.01f, 0.0f, 0.0f, "Min %.4f", "Max %.4f" );
    ImGui::SameLine();
    if ( ImGui::Button( "0..1" ) ) {
        range_min = 0.0f;
        range_max = 1.0f;
    }

    ImGui::SliderFloat( "Exposure (EV)", &exposure_ev, -16.0f, 16.0f, "%.1f" );

    const VkImageSubresourceRange& range = view->subresource_range;
    const u32 mip_count = range.levelCount == VK_REMAINING_MIP_LEVELS ? image->mip_level_count - range.baseMipLevel : range.levelCount;
    if ( mip_count > 1 ) {
        ImGui::SliderUint( "Mip", &mip, 0, mip_count - 1 );
    }
    mip = raptor::min( mip, mip_count - 1 );

    ImGui::Checkbox( "sRGB encode", &encode_srgb );
    ImGui::SameLine();
    ImGui::Checkbox( "NaN/Inf (magenta)", &show_nan_inf );
    ImGui::SameLine();
    ImGui::Checkbox( "Out of range (red/blue)", &show_out_of_range );

    ImGui::SliderFloat( "Zoom", &zoom, 0.1f, 256.0f, "%.2f", ImGuiSliderFlags_Logarithmic );
    ImGui::SameLine();
    if ( ImGui::Button( "Reset view" ) ) {
        zoom = 1.0f;
        pan = { 0.0f, 0.0f };
    }
    ImGui::SameLine();
    ImGui::Checkbox( "Fullscreen (Esc)", &fullscreen );

    if ( fullscreen && ImGui::IsKeyPressed( ImGuiKey_Escape ) ) {
        fullscreen = false;
    }

    // View: display pixel -> input texel
    const u32 base_mip = range.baseMipLevel + mip;
    const glm::vec2 input_size{ ( f32 )raptor::max( ( u32 )image->width >> base_mip, 1u ),
                                ( f32 )raptor::max( ( u32 )image->height >> base_mip, 1u ) };

    // The image fills the rest of the panel; the output image is as large as the swapchain.
    const ImVec2 available = ImGui::GetContentRegionAvail();
    // Output always full size: the same pixels feed the panel and the fullscreen passthrough.
    const glm::vec2 output_size{ ( f32 )feature.output_width, ( f32 )feature.output_height };

    const f32 fit = raptor::min( output_size.x / input_size.x, output_size.y / input_size.y );
    f32 texels_per_pixel = 1.0f / ( fit * zoom );
    glm::vec2 view_center = input_size * 0.5f + pan;

    // Panel: the output keeps its aspect and is downscaled by the sampler.
    const f32 panel_scale = raptor::max( raptor::min( available.x / output_size.x, available.y / output_size.y ), 0.01f );
    const glm::vec2 panel_size = output_size * panel_scale;

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton( "image", ImVec2( panel_size.x, panel_size.y ) );

    // Mouse in output pixels, from the centre.
    const ImGuiIO& io = ImGui::GetIO();
    const glm::vec2 mouse_from_center = ( glm::vec2( io.MousePos.x - origin.x, io.MousePos.y - origin.y ) - panel_size * 0.5f ) / panel_scale;

    if ( ImGui::IsItemHovered() && io.MouseWheel != 0.0f ) {
        const glm::vec2 texel_under_mouse = view_center + mouse_from_center * texels_per_pixel;
        zoom = raptor::clamp( zoom * powf( 1.2f, io.MouseWheel ), 0.1f, 256.0f );
        texels_per_pixel = 1.0f / ( fit * zoom );
        view_center = texel_under_mouse - mouse_from_center * texels_per_pixel;
    }

    if ( ImGui::IsItemActive() && ImGui::IsMouseDragging( ImGuiMouseButton_Left ) ) {
        view_center -= glm::vec2( io.MouseDelta.x, io.MouseDelta.y ) / panel_scale * texels_per_pixel;
    }

    pan = view_center - input_size * 0.5f;

    if ( ImGui::IsItemHovered() ) {
        const glm::vec2 texel = view_center + mouse_from_center * texels_per_pixel;
        ImGui::SetTooltip( "Texel %d, %d (mip %u)", ( i32 )floorf( texel.x ), ( i32 )floorf( texel.y ), mip );
    }

    ImGui::GetWindowDrawList()->AddImage( ( ImTextureID )&feature.output_view, origin,
                                          ImVec2( origin.x + panel_size.x, origin.y + panel_size.y ) );   // uv 0..1

    // Request the conversion for this frame
    GpuImageViewerConstants& constants = feature.constants;
    constants.input_index       = input.index();
    constants.output_index      = feature.output_view.index();
    constants.options           = ( TextureFormat::is_uint_format( image->vk_format ) ? k_image_viewer_input_uint : 0 ) |
                                  ( TextureFormat::is_sint_format( image->vk_format ) ? k_image_viewer_input_sint : 0 ) |
                                  ( encode_srgb ? k_image_viewer_encode_srgb : 0 ) |
                                  ( show_nan_inf ? k_image_viewer_show_nan_inf : 0 ) |
                                  ( show_out_of_range ? k_image_viewer_show_out_of_range : 0 );
    constants.channel_mode      = channel_mode;
    constants.range_min         = range_min;
    constants.range_rcp_size    = 1.0f / raptor::max( range_max - range_min, 1e-6f );
    constants.exposure_scale    = exp2f( exposure_ev );
    constants.mip               = mip;
    constants.display_size      = output_size;
    constants.input_size        = input_size;
    constants.view_center       = view_center;
    constants.texels_per_pixel  = texels_per_pixel;

    feature.input = input;
    feature.requested = true;

    ImGui::PopID();
}

// VolumetricFogRenderConfig /////////////////////////////////////////////
void VolumetricFogRenderConfig::draw_imgui() {
    if ( ImGui::CollapsingHeader( "Volumetric Fog" ) ) {
        ImGui::PushID( "VolumetricFogRenderConfig" );

        ImGui::SliderFloat( "Fog Constant Density", &density, 0.0f, 1.0f );
        ImGui::SliderFloat( "Fog Scattering Factor", &scattering_factor, 0.0f, 1.0f );
        ImGui::SliderFloat( "Height Fog Density", &height_fog_density, 0.0f, 10.0f );
        ImGui::SliderFloat( "Height Fog Falloff", &height_fog_falloff, 0.0f, 10.0f );
        ImGui::SliderUint( "Phase Function Type", &phase_function_type, 0, 3 );
        ImGui::SliderFloat( "Phase Anisotropy", &phase_anisotropy_01, 0.0f, 1.0f );
        ImGui::SliderFloat( "Fog Noise Scale", &noise_scale, 0.0f, 1.0f );
        ImGui::SliderFloat( "Lighting Noise Scale", &lighting_noise_scale, 0.0f, 1.0f );
        ImGui::Checkbox( "Light Scattering Jitter Animated (on/off)", &light_scattering_jitter_animated );
        ImGui::SliderFloat( "Light Scattering Jitter Scale", &light_scattering_jitter_scale, 0.0f, 1.0f );
        ImGui::SliderUint( "Fog Noise Type", &noise_type, 0, 2 );
        ImGui::SliderFloat( "Temporal Reprojection Percentage", &temporal_reprojection_percentage, 0.0f, 1.0f );
        ImGui::SliderFloat( "Temporal Reprojection Jittering Scale", &temporal_reprojection_jittering_scale, 0.0f, 1.0f );
        ImGui::Checkbox( "Use Temporal Reprojection", &use_temporal_reprojection );
        ImGui::Checkbox( "Use Spatial Filtering", &use_spatial_filtering );
        ImGui::SliderFloat( "Fog Application Scale", &application_dithering_scale, 0.0f, 0.1f );
        ImGui::Checkbox( "Fog Application Opacity AA", &application_apply_opacity_anti_aliasing );
        ImGui::Checkbox( "Fog Application Tricubic", &application_apply_tricubic_filtering );
        ImGui::SliderFloat( "Fog Volumetric Noise Position Scale", &noise_position_scale, 0.0f, 1.0f );
        ImGui::SliderFloat( "Fog Volumetric Noise Speed Scale", &noise_speed_scale, 0.0f, 1.0f );

        ImGui::SliderFloat3( "Box position", &box_position[ 0 ], -10.f, 10.f, "%2.3f" );
        ImGui::SliderFloat3( "Box size", &box_size[ 0 ], -4.f, 4.f, "%1.3f" );
        ImGui::SliderFloat( "Box density", &box_density, 0.0f, 10.0f );

        Color box_color_ = { box_color };
        f32 box_color_floats[ 3 ] = { box_color_.r(), box_color_.g(), box_color_.b() };
        if ( ImGui::ColorEdit3( "Box color", box_color_floats ) ) {

            box_color_.set( box_color_floats[ 0 ], box_color_floats[ 1 ], box_color_floats[ 2 ], 1.0f );

            box_color = box_color_.abgr;
        }
        ImGui::PopID();
    }
}

u32 VolumetricFogRenderConfig::pack_options() const {
    u32 options = 0u;
    options |= application_apply_opacity_anti_aliasing ? k_vfog_opacity_anti_aliasing : 0u;
    options |= application_apply_tricubic_filtering ? k_vfog_tricubic_filtering : 0u;

    return options;
}

void TAARenderConfig::draw_imgui() {

    if ( ImGui::CollapsingHeader( "Temporal Anti-Aliasing" ) ) {
        ImGui::PushID( "TAARenderConfig" );

        ImGui::Checkbox( "Enable", &enabled );
        ImGui::Checkbox( "Jittering Enable", &jittering_enabled );

        static i32 current_jitter_type = ( i32 )jitter_type;
        static cstring jitter_names[] = { "Halton", "Martin Robert R2", "Hammersley", "Interleaved Gradients" };
        ImGui::Combo( "Jitter Type", &current_jitter_type, jitter_names, ArraySize( jitter_names ) );
        jitter_type = ( JitterType::Enum )current_jitter_type;

        ImGui::SliderUint( "Jittering Period", &jitter_period, 1, 16 );
        ImGui::SliderFloat( "Jitter Scale", &jitter_scale, 0.0f, 4.0f );
        ImGui::SliderFloat( "Current Sample Sharpness", &current_sample_sharpness, 0.0f, 1.0f );

        static cstring taa_mode_names[] = { "OnlyReprojection", "Full" };
        ImGui::Combo( "Modes", &mode, taa_mode_names, ArraySize( taa_mode_names ) );

        static cstring taa_velocity_mode_names[] = { "None", "3x3 Neighborhood", "3x3 Dominant Velocity"};
        ImGui::Combo( "Velocity sampling modes", &velocity_sampling_mode, taa_velocity_mode_names, ArraySize( taa_velocity_mode_names ) );

        static cstring taa_history_sampling_names[] = { "None", "CatmullRom" };
        ImGui::Combo( "History sampling filter", &history_sampling_filter, taa_history_sampling_names, ArraySize( taa_history_sampling_names ) );

        static cstring taa_history_constraint_names[] = { "None", "Clamp", "Clip", "Variance Clip", "Variance Clip with Color Clamping" };
        ImGui::Combo( "History constraint mode", &history_constraint_mode, taa_history_constraint_names, ArraySize( taa_history_constraint_names ) );

        static cstring taa_current_color_filter_names[] = { "None", "Mitchell-Netravali", "Blackman-Harris", "Catmull-Rom" };
        ImGui::Combo( "Current color filter", &current_color_filter, taa_current_color_filter_names, ArraySize( taa_current_color_filter_names ) );

        ImGui::Checkbox( "Inverse Luminance Filtering", &use_inverse_luminance_filtering );
        ImGui::Checkbox( "Temporal Filtering", &use_temporal_filtering );
        ImGui::Checkbox( "Luminance Difference Filtering", &use_luminance_difference_filtering );
        ImGui::Checkbox( "Use YCoCg color space", &use_ycocg );
        ImGui::PopID();
    }
}

void RaytracedShadowsConfig::draw_imgui() {
    if ( ImGui::CollapsingHeader( "Raytraced Shadows" ) ) {
        ImGui::PushID( "RaytracedShadowsConfig" );

        static cstring light_type_names[] = { "Directional", "Point" };
        ImGui::Combo( "RT Light Type", &light_type, light_type_names, ArraySize( light_type_names ) );

        ImGui::SliderFloat( "RT Light intensity", &light_intensity, 0.01f, 10.f, "%2.2f" );
        ImGui::ColorEdit3( "RT Light Color", &light_color[ 0 ]);

        // If directional light, disable light position and light radius controls
        if ( light_type == 0 ) {
            ImGui::BeginDisabled();
        }
        ImGui::SliderFloat( "RT Light Radius", &light_radius, 0.01f, 10.f );
        ImGui::SliderFloat3( "RT Light Position", &light_position[ 0 ], -10.f, 10.f, "%2.2f" );
        ImGui::SliderFloat( "Softness (source radius, m)", &light_source_radius, 0.0f, 1.f );
        if ( light_type == 0 ) {
            ImGui::EndDisabled();
        }

        // If type is a pointlight, disable the light direction
        if ( light_type == 1 ) {
            ImGui::BeginDisabled();
        }
        ImGui::SliderFloat3( "RT Directional Direction", &light_direction[ 0 ], -1.f, 1.f, "%2.2f" );
        ImGui::SliderFloat( "Softness (angular diameter, deg)", &light_angular_radius, 0.0f, 1.f );
        if ( light_type == 1 ) {
            ImGui::EndDisabled();
        }

        ImGui::Checkbox( "Disable History", &disable_history );
        ImGui::Checkbox( "Disable Spatial", &disable_spatial );
        ImGui::SliderUint( "Max Samples", &max_samples, 0, 4 );
        ImGui::PopID();
    }
}

void RaytracedReflectionsConfig::draw_imgui() {
    if ( ImGui::CollapsingHeader( "Raytraced Reflections" ) ) {
        ImGui::PushID( "RaytracedReflectionsConfig" );

        ImGui::Checkbox( "Enable", &enabled );
        ImGui::SliderFloat( "Intensity", &intensity, 0.0f, 1.0f );
        //ImGui::SliderFloat( "Reflections Scale", &reflections_scale, 0.0f, 1.0f );
        ImGui::SliderFloat( "Temporal Depth Difference", &temporal_depth_difference, 0.0f, 100.0f );
        ImGui::SliderFloat( "Temporal Normal Difference", &temporal_normal_difference, 0.0f, 100.0f );
        ImGui::SliderFloat( "Wavelet Sigma Z", &wavelet_sigma_z, 1.0f, 10.0f );
        ImGui::SliderFloat( "Wavelet Sigma N", &wavelet_sigma_n, 1.0f, 256.0f );
        ImGui::SliderFloat( "Wavelet Sigma L", &wavelet_sigma_l, 1.0f, 10.0f );

        ImGui::PopID();
    }
}

void ReSTIRGIConfig::draw_imgui() {
    if ( ImGui::CollapsingHeader( "ReSTIR GI" ) ) {
        ImGui::PushID( "ReSTIRGIConfig" );

        ImGui::Checkbox( "Enable", &enabled );
        ImGui::SliderFloat( "Intensity", &gi_intensity, 0.0f, 1.0f );

        ImGui::PopID();
    }
}

void RenderConfig::draw_common_imgui() {

    ImGui::Checkbox( "Use Slang Shaders", &use_slang_shaders );
    ImGui::InputFloat( "Scene global scale", &global_scale, 0.001f );
    ImGui::SliderFloat( "Force Roughness", &forced_roughness, -1, 1 );
    ImGui::SliderFloat( "Force Metalness", &forced_metalness, -1, 1 );
    ImGui::SliderFloat( "Specular AA variance", &specular_aa_variance, 0.0f, 1.0f, "%.3f" );
    ImGui::SliderFloat( "Specular AA threshold", &specular_aa_threshold, 0.0f, 0.5f, "%.3f" );
    ImGui::Separator();

}

} // namespace raptor