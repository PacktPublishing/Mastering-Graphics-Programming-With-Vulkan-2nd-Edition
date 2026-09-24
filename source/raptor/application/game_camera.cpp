#include "game_camera.hpp"

#include "foundation/platform.hpp"
#include "foundation/numerics.hpp"

#include "external/glm/gtc/matrix_transform.hpp"
#include "external/imgui/imgui.h"

namespace raptor {

// GameCamera //////////////////////////////////////////////////////////////////
void GameCamera::init( bool enabled_, f32 rotation_speed_, f32 movement_speed_, f32 movement_delta_ ) {

    reset();
    enabled = enabled_;

    rotation_speed = rotation_speed_;
    movement_speed = movement_speed_;
    movement_delta = movement_delta_;
}

void GameCamera::reset() {

    target_yaw = 0.0f;
    target_pitch = 0.0f;

    target_movement = camera.position;

    mouse_dragging = false;
    ignore_dragging_frames = 3;
    mouse_sensitivity = 1.0f;
}

// Taken from this article:
// http://www.rorydriscoll.com/2016/03/07/frame-rate-independent-damping-using-lerp/
//
float lerp( float a, float b, float t, float dt ) {
    return glm::mix( a, b, 1.f - powf( 1 - t, dt ) );
}

glm::vec3 lerp3( const glm::vec3& from, const glm::vec3& to, f32 t, f32 dt ) {
    return glm::vec3{ lerp(from.x, to.x, t, dt), lerp( from.y, to.y, t, dt ), lerp( from.z, to.z, t, dt ) };
}

void GameCamera::update( InputService* input, u32 window_width, u32 window_height, f32 delta_time ) {

    if ( !enabled )
        return;

    camera.update();

    // Ignore first dragging frames for mouse movement waiting the cursor to be placed at the center of the screen.

    if ( input->is_mouse_dragging( MOUSE_BUTTONS_RIGHT ) && !ImGui::GetIO().WantCaptureMouse ) {

        if ( ignore_dragging_frames == 0 ) {
            target_yaw -= ( input->mouse_position.x - roundu32(window_width / 2.f) ) * mouse_sensitivity * delta_time;
            target_pitch -= ( input->mouse_position.y - roundu32(window_height / 2.f) ) * mouse_sensitivity * delta_time;
        } else {
            --ignore_dragging_frames;
        }
        mouse_dragging = true;

    } else {
        mouse_dragging = false;

        ignore_dragging_frames = 3;
    }

    glm::vec3 camera_movement{ 0, 0, 0 };
    float camera_movement_delta = movement_delta;

    if ( input->is_key_down( KEY_RSHIFT ) || input->is_key_down( KEY_LSHIFT ) ) {
        camera_movement_delta *= 10.0f;
    }

    if ( input->is_key_down( KEY_RALT ) || input->is_key_down( KEY_LALT ) ) {
        camera_movement_delta *= 100.0f;
    }

    if ( input->is_key_down( KEY_RCTRL ) || input->is_key_down( KEY_LCTRL ) ) {
        camera_movement_delta *= 0.1f;
    }

    if ( input->is_key_down( KEY_LEFT ) || input->is_key_down( KEY_A ) ) {
        camera_movement = camera_movement + camera.right * -camera_movement_delta;
    } else if ( input->is_key_down( KEY_RIGHT ) || input->is_key_down( KEY_D ) ) {
        camera_movement = camera_movement + camera.right * camera_movement_delta;
    }

    if ( input->is_key_down( KEY_PAGEDOWN ) || input->is_key_down( KEY_E ) ) {
        camera_movement = camera_movement + camera.up * -camera_movement_delta;
    } else if ( input->is_key_down( KEY_PAGEUP ) || input->is_key_down( KEY_Q ) ) {
        camera_movement = camera_movement + camera.up * camera_movement_delta;
    }

    if ( input->is_key_down( KEY_UP ) || input->is_key_down( KEY_W ) ) {
        camera_movement = camera_movement + camera.direction * camera_movement_delta;
    } else if ( input->is_key_down( KEY_DOWN ) || input->is_key_down( KEY_S ) ) {
        camera_movement = camera_movement + camera.direction * -camera_movement_delta;
    }

    // Do not navigate while editing ImGui fields.
    if ( ImGui::GetIO().WantCaptureKeyboard ) {
        camera_movement = glm::vec3( 0.0f );
    }

    target_movement = target_movement + camera_movement;


    {
        // Update camera rotation
        const f32 tween_speed = rotation_speed * delta_time;
        camera.rotate( ( target_pitch - camera.pitch ) * tween_speed,
                       ( target_yaw - camera.yaw ) * tween_speed );

        // Update camera position
        const f32 tween_position_speed = movement_speed * delta_time;
        camera.position = lerp3( camera.position, target_movement, 0.9f, tween_position_speed );
    }
}

static bool s_jittering_optimized = false;

void GameCamera::apply_jittering( f32 x, f32 y ) {
    // Reset camera projection
    camera.calculate_projection_matrix();

    if ( s_jittering_optimized ) {
        camera.projection[ 2 ][ 0 ] += x * camera.projection[ 2 ][ 3 ];
        camera.projection[ 2 ][ 1 ] += y * camera.projection[ 2 ][ 3 ];
    }
    else {
        glm::mat4 jittering_matrix = glm::translate( glm::mat4( 1.0f ), glm::vec3{ x, y, 0.0f } );
        camera.projection = jittering_matrix * camera.projection;
    }

    camera.calculate_view_projection();
}

bool GameCamera::draw_debug_ui() {

    if ( ImGui::CollapsingHeader( "Camera" ) ) {
        return false;
    }

    Camera& c = camera;
    bool changed = false;

    // Freeze navigation while keeping rendering and temporal accumulation active.
    bool lock_controls = !enabled;
    const bool controls_changed = ImGui::Checkbox( "Lock controls", &lock_controls );

    if ( controls_changed ) {
        enabled = !lock_controls;
    }

    ImGui::Separator();

    // Position ////////////////////////////////////////////////////////////////

    changed |= ImGui::DragFloat3(
        "Position", &c.position.x, 0.05f, 0.0f, 0.0f, "%.3f" );

    if ( ImGui::Button( "Position = 0" ) ) {
        c.position = glm::vec3( 0.0f );
        changed = true;
    }

    ImGui::SameLine();

    if ( ImGui::Button( "Reset pose" ) ) {
        c.position = glm::vec3( 0.0f );
        c.yaw = 0.0f;
        c.pitch = 0.0f;
        changed = true;
    }

    // Orientation /////////////////////////////////////////////////////////////

    float angles[ 2 ] = { glm::degrees( c.yaw ), glm::degrees( c.pitch ) };

    if ( ImGui::DragFloat2( "Yaw / Pitch (deg)", angles, 0.25f, 0.0f, 0.0f, "%.2f" ) ) {

        c.yaw = glm::radians( angles[ 0 ] );
        c.pitch = glm::radians( glm::clamp( angles[ 1 ], -90.0f, 90.0f ) );

        changed = true;
    }

    const auto set_angles = [ & ]( float yaw_degrees,
                                   float pitch_degrees ) {
       c.yaw = glm::radians( yaw_degrees );
       c.pitch = glm::radians( pitch_degrees );
       changed = true;
    };

    ImGui::TextUnformatted( "Look along world axis" );

    if ( ImGui::Button( "+X" ) ) set_angles( -90.0f, 0.0f );
    ImGui::SameLine();
    if ( ImGui::Button( "-X" ) ) set_angles( 90.0f, 0.0f );
    ImGui::SameLine();
    if ( ImGui::Button( "+Y" ) ) set_angles( 0.0f, 90.0f );
    ImGui::SameLine();
    if ( ImGui::Button( "-Y" ) ) set_angles( 0.0f, -90.0f );
    ImGui::SameLine();
    if ( ImGui::Button( "+Z" ) ) set_angles( 180.0f, 0.0f );
    ImGui::SameLine();
    if ( ImGui::Button( "-Z" ) ) set_angles( 0.0f, 0.0f );

    if ( ImGui::Button( "Level pitch" ) ) {
        c.pitch = 0.0f;
        changed = true;
    }

    ImGui::SameLine();

    if ( ImGui::Button( "Turn 180" ) ) {
        c.yaw += glm::radians( 180.0f );
        changed = true;
    }

    if ( ImGui::Button( "Look at origin" ) ) {
        const glm::vec3 to_origin = -c.position;
        const float distance_squared = glm::dot( to_origin, to_origin );

        // Looking at our own position has no defined direction.
        if ( distance_squared > 1e-12f ) {
            const glm::vec3 d =
                to_origin / std::sqrt( distance_squared );

            // Camera::update() uses:
            // forward = (-sin(yaw)*cos(pitch),
            //             sin(pitch),
            //            -cos(yaw)*cos(pitch)).
            //
            // Preserve yaw when looking exactly vertically.
            if ( d.x * d.x + d.z * d.z > 1e-12f ) {
                c.yaw = std::atan2( -d.x, -d.z );
            }

            c.pitch = std::asin(
                glm::clamp( d.y, -1.0f, 1.0f ) );

            changed = true;
        }
    }

    // Projection //////////////////////////////////////////////////////////////

    ImGui::Separator();

    if ( c.perspective ) {
        float fov = c.field_of_view_y;

        if ( ImGui::SliderFloat( "Vertical FOV", &fov, 10.0f, 120.0f, "%.1f deg" ) ) {
            c.set_fov_y( glm::clamp( fov, 10.0f, 120.0f ) );
            changed = true;
        }
    } else {
        float zoom = c.zoom;

        if ( ImGui::DragFloat( "Zoom", &zoom, 0.01f, 0.001f, 1000.0f ) ) {
            c.set_zoom( glm::clamp( zoom, 0.001f, 1000.0f ) );
            changed = true;
        }
    }

    // Navigation //////////////////////////////////////////////////////////////
    if ( ImGui::TreeNode( "Navigation" ) ) {

        // movement_delta controls translation increments in this controller.
        // movement_speed controls how quickly position follows its target.
        if ( ImGui::DragFloat( "Movement step", &movement_delta, 0.001f, 0.0001f, 100.0f, "%.4f" ) ) {
            movement_delta = glm::clamp( movement_delta, 0.0001f, 100.0f );
        }

        if ( ImGui::DragFloat( "Mouse sensitivity", &mouse_sensitivity, 0.01f, 0.01f, 10.0f, "%.2f" ) ) {
            mouse_sensitivity = glm::clamp( mouse_sensitivity, 0.01f, 10.0f );
        }

        ImGui::TextUnformatted( "Shift: x10 | Alt: x100 | Ctrl: x0.1" );

        ImGui::TreePop();
    }

    // Keep controller targets aligned with direct camera edits.
    // Also discard pending movement when locking/unlocking navigation.
    if ( changed || controls_changed ) {
        target_movement = c.position;
        target_yaw = c.yaw;
        target_pitch = c.pitch;

        mouse_dragging = false;
        ignore_dragging_frames = 3;

        // Rebuild an unjittered projection.
        // FrameRenderer::upload_gpu_data() applies this frame's jitter later.
        c.update_projection = true;
        c.update();
    }

    return changed;
}

} // namespace raptor
