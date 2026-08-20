#if !defined(__SHARED_BINDINGS_H__)
#define __SHARED_BINDINGS_H__

// TODO(marco): fix duplicated entries

#define k_src 0
#define k_camera_parameters 0
#define k_scene_constants 0
#define k_views 0
#define k_physics_data 0
#define k_meshlets 1
#define k_dst 1
// #define k_visible_mesh_instances 1
#define k_physics_mesh 1
#define k_lighting_constants 1
#define k_sphere_transforms 1
#define k_fragment_shading_rate_image 2
#define k_mesh_draws 2
#define k_position_data 2
#define k_ray_params 3
#define k_culled_mesh_instances 3
#define k_meshlet_data 3
#define k_normal_data 3
#define k_joint_matrices 3
#define k_index_data 4
#define k_vertex_positions 4
#define k_vertex_data 5
#define k_visible_mesh_instances 6
#define k_visible_mesh_count 7
#define k_visible_meshlet_index_buffer 8
#define k_meshlet_instances 9
#define k_mesh_instance_draws 10
#define k_post_constants 11
// #define k_visible_mesh_count 11
#define k_mesh_bounds 12
#define k_early_visible_mesh_count 13
#define k_indirect_per_meshlet_counts 17
#define k_visible_meshlet_instances 19
#define k_z_bins 20
#define k_lights 21
#define k_tiles 22
#define k_light_constants 23
#define k_light_indices 25
#define k_as 26
// #define k_lights 27
#define k_shadow_visibility_constants 30
// #define k_meshlet_instances 30
#define k_per_light_meshlet_indices 31
#define k_meshlet_draw_commands 32
#define k_shadow_camera_spheres 33
#define k_shadow_views 34
#define k_lights_aabb_array 35
// #define k_lights 35
#define k_shadow_resolutions 36
// #define k_lights 37
#define k_svgf_accumulation_constants 40
#define k_volumetric_fog_constants 40
#define k_reflections_constants 40
#define k_irradiance_image 41
#define k_visibility_image 42
#define k_probe_status_ssbo 43
#define k_debug_lines 50
#define k_taa_constants 50
#define k_debug_lines_count 51
#define k_motion_vectors 51
#define k_debug_line_commands 52
#define k_visibility_motion_vectors 52
#define k_normals_texture 53
#define k_ddgi_constants 55

#endif // __SHARED_BINDINGS_H__
