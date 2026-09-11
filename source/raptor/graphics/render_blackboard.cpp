#include "graphics/render_blackboard.hpp"
#include "graphics/raptor_imgui.hpp"

#include "external/imgui/imgui.h"

#include "graphics/render_scene.hpp"
#include "foundation/numerics.hpp"

namespace raptor {

void LightingRenderConfig::draw_imgui( Span<Light> lights ) {

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

        ImGui::PopID();
    }

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

        if ( ImGui::Button( "Sponza light test" ) ) {
            load_shadow_test_lights = true;
        }

        if ( ImGui::Button( "Sponza light test advanced" ) ) {
            load_shadow_test_lights_adv = true;
        }

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

void DebugDrawRenderConfig::draw_imgui() {
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
        if ( light_type == 0 ) {
            ImGui::EndDisabled();
        }

        // If type is a pointlight, disable the light direction
        if ( light_type == 1 ) {
            ImGui::BeginDisabled();
        }
        ImGui::SliderFloat3( "RT Directional Direction", &light_direction[ 0 ], -1.f, 1.f, "%2.2f" );
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

} // namespace raptor