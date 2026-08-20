#include "foundation/camera.hpp"

#include "external/glm/mat4x4.hpp"
#include "external/glm/gtc/quaternion.hpp"
#include "external/glm/ext/matrix_transform.hpp"

namespace raptor {


// Camera ///////////////////////////////////////////////////////////////////////

void Camera::init_perpective( f32 near_plane_, f32 far_plane_, f32 fov_y, f32 aspect_ratio_ ) {
    perspective = true;

    near_plane = near_plane_;
    far_plane = far_plane_;
    field_of_view_y = fov_y;
    aspect_ratio = aspect_ratio_;

    reset();
}

void Camera::init_orthographic( f32 near_plane_, f32 far_plane_, f32 viewport_width_, f32 viewport_height_, f32 zoom_ ) {
    perspective = false;

    near_plane = near_plane_;
    far_plane = far_plane_;

    viewport_width = viewport_width_;
    viewport_height = viewport_height_;
    zoom = zoom_;

    reset();
}

void Camera::reset() {
    position = { };
    yaw = 0;
    pitch = 0;
    view = glm::mat4( 1.0f );
    projection = glm::mat4( 1.0f );

    update_projection = true;
}

void Camera::set_viewport_size( f32 width_, f32 height_ ) {
    viewport_width = width_;
    viewport_height = height_;

    update_projection = true;
}

void Camera::set_zoom( f32 zoom_ ) {
    zoom = zoom_;

    update_projection = true;
}

void Camera::set_aspect_ratio( f32 aspect_ratio_ ) {
    aspect_ratio = aspect_ratio_;

    update_projection = true;
}

void Camera::set_fov_y( f32 fov_y_ ) {
    field_of_view_y = fov_y_;

    update_projection = true;
}

void Camera::update() {

    // Left for reference.
    // Calculate rotation from yaw and pitch
    /*direction.x = sinf( ( yaw ) ) * cosf( ( pitch ) );
    direction.y = sinf( ( pitch ) );
    direction.z = cosf( ( yaw ) ) * cosf( ( pitch ) );
    direction = glms_vec3_normalize( direction );

    glm::vec3 center = glms_vec3_sub( position, direction );
    glm::vec3 cup{ 0,1,0 };

    right = glms_cross( cup, direction );
    up = glms_cross( direction, right );

    // Calculate view matrix
    view = glms_lookat( position, center, up );
    */

    // Right-handed coordinate system
    // Quaternion based rotation.
    // https://stackoverflow.com/questions/49609654/quaternion-based-first-person-view-camera
    const glm::quat pitch_rotation = glm::angleAxis( pitch, glm::vec3( 1, 0, 0 ) );
    const glm::quat yaw_rotation = glm::angleAxis( yaw, glm::vec3( 0, 1, 0 ) );
    const glm::quat rotation = glm::normalize( yaw_rotation * pitch_rotation );

    const glm::quat rotation_inv = glm::conjugate( rotation );
    const glm::mat4 rotation_mat = glm::mat4_cast( rotation_inv );

    const glm::mat4 translation = glm::translate( glm::mat4( 1.0f ), -position );
    view = rotation_mat * translation;

    // Update the vectors used for movement
    right = { view[0][0], view[1][0], view[2][0] };
    up = { view[0][1], view[1][1], view[2][1] };
    direction = glm::vec3( view[0][2], view[1][2], view[2][2] ) * -1.0f;

    if ( update_projection ) {
        update_projection = false;

        calculate_projection_matrix();
    }

    // Calculate final view projection matrix
    calculate_view_projection();
}

void Camera::rotate( f32 delta_pitch, f32 delta_yaw ) {

    pitch += delta_pitch;
    yaw += delta_yaw;
}

void Camera::calculate_projection_matrix() {
    if ( perspective ) {
        projection = glm::perspective( glm::radians( field_of_view_y ), aspect_ratio, near_plane, far_plane );
    } else {
        projection = glm::ortho( zoom * -viewport_width / 2.f, zoom * viewport_width / 2.f, zoom * -viewport_height / 2.f, zoom * viewport_height / 2.f, near_plane, far_plane );
    }
}

void Camera::calculate_view_projection() {
    view_projection = projection * view;
}

glm::vec3 Camera::unproject( const glm::vec3& screen_coordinates ) {
    return glm::unProject( screen_coordinates, glm::mat4( 1.0f ), view_projection, glm::vec4{ 0, 0, viewport_width, viewport_height } );
}

glm::vec3 Camera::unproject_inverted_y( const glm::vec3& screen_coordinates ) {
    const glm::vec3 screen_coordinates_y_inv{ screen_coordinates.x, viewport_height - screen_coordinates.y, screen_coordinates.z };
    return unproject( screen_coordinates_y_inv );
}

void Camera::get_projection_ortho_2d( glm::mat4& out_matrix ) {
    out_matrix = glm::ortho( 0.0f, viewport_width * zoom, 0.0f, viewport_height * zoom, -1.0f, 1.0f );
}

void Camera::yaw_pitch_from_direction( const glm::vec3& direction, f32& yaw, f32& pitch ) {

    yaw = glm::degrees( atan2f( direction.z, direction.x ) );
    pitch = glm::degrees( asinf( direction.y ) );
}

} // namespace raptor
