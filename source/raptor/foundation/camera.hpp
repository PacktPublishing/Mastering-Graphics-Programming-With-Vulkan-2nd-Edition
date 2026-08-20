#pragma once

#include "foundation/platform.hpp"

#include "external/glm/mat4x4.hpp"

namespace raptor {

//
// Camera struct - can be both perspective and orthographic.
//
struct Camera {

    void                        init_perpective( f32 near_plane, f32 far_plane, f32 fov_y, f32 aspect_ratio );
    void                        init_orthographic( f32 near_plane, f32 far_plane, f32 viewport_width, f32 viewport_height, f32 zoom );

    void                        reset();

    void                        set_viewport_size( f32 width, f32 height );
    void                        set_zoom( f32 zoom );
    void                        set_aspect_ratio( f32 aspect_ratio );
    void                        set_fov_y( f32 fov_y );

    void                        update();
    void                        rotate( f32 delta_pitch, f32 delta_yaw );

    void                        calculate_projection_matrix();
    void                        calculate_view_projection();

    // Project/unproject
    glm::vec3                   unproject( const glm::vec3& screen_coordinates );

    // Unproject by inverting the y of the screen coordinate.
    glm::vec3                   unproject_inverted_y( const glm::vec3& screen_coordinates );

    void                        get_projection_ortho_2d( glm::mat4& out_matrix );

    static void                 yaw_pitch_from_direction( const glm::vec3& direction, f32 & yaw, f32& pitch );

    glm::mat4                   view;
    glm::mat4                   projection;
    glm::mat4                   view_projection;

    glm::vec3                   position;
    glm::vec3                   right;
    glm::vec3                   direction;
    glm::vec3                   up;

    f32                         yaw;
    f32                         pitch;

    f32                         near_plane;
    f32                         far_plane;

    f32                         field_of_view_y;
    f32                         aspect_ratio;

    f32                         zoom;
    f32                         viewport_width;
    f32                         viewport_height;

    bool                        perspective;
    bool                        update_projection;

}; // struct Camera

} // namespace raptor
