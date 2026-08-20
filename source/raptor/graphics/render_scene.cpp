#include "graphics/render_scene.hpp"
#include "graphics/renderer.hpp"
#include "graphics/scene_graph.hpp"
#include "graphics/obj_model.hpp"
#include "graphics/gltf_model.hpp"
#include "graphics/fbx_model.hpp"
#include "graphics/frame_renderer.hpp"

#include "foundation/numerics.hpp"
#include "foundation/file.hpp"

#include "external/glm/matrix.hpp"
#include "external/glm/mat4x4.hpp"
#include "external/glm/vec3.hpp"
#include "external/glm/gtc/quaternion.hpp"
#include "external/meshoptimizer/meshoptimizer.h"
#include "external/imgui/imgui.h"

namespace raptor {


//
// PhysicsVertex ///////////////////////////////////////////////////////
void PhysicsVertex::add_joint( u32 vertex_index ) {
    for ( u32 j = 0; j < joint_count; ++j ) {
        if ( joints[ j ].vertex_index == vertex_index ) {
            return;
        }
    }

    RASSERT( joint_count < k_max_joint_count );
    joints[ joint_count++ ].vertex_index = vertex_index;
}

// Quantize based on AABB
u32 pack_mesh_position_101010( const glm::vec3& position, const glm::vec3& aabb_min, const glm::vec3& aabb_max ) {
    const glm::vec3 extent = glm::max( aabb_max - aabb_min, glm::vec3( 1.0e-10f ) );
    const glm::vec3 normalized = glm::clamp( ( position - aabb_min ) / extent, glm::vec3( 0.0f ), glm::vec3( 1.0f ) );

    const u32 x = u32( normalized.x * 1023.0f + 0.5f );
    const u32 y = u32( normalized.y * 1023.0f + 0.5f );
    const u32 z = u32( normalized.z * 1023.0f + 0.5f );

    return x | ( y << 10u ) | ( z << 20u );
}

//
// RenderModel //////////////////////////////////////////////////////////
void RenderModel::build_meshlets( Mesh& mesh, Allocator* temp_allocator ) {
    if ( !renderer->gpu->mesh_shaders_extension_present ) return;

    // Build meshlets
    MeshBuffers mesh_buffers = render_scene->mesh_buffers;

    const sizet max_vertices = 64;
    const sizet max_triangles = 124;
    const f32 cone_weight = 0.0f;

    const sizet max_meshlets = meshopt_buildMeshletsBound( mesh.index_count, max_vertices, max_triangles );

    Array<meshopt_Meshlet> local_meshlets;
    local_meshlets.init( temp_allocator, ( u32 )max_meshlets, ( u32 )max_meshlets );

    Array<u32> meshlet_vertex_indices;
    u32 vertex_indices_count = u32( max_meshlets * max_vertices );
    meshlet_vertex_indices.init( temp_allocator, vertex_indices_count, vertex_indices_count );

    Array<u8> meshlet_triangles;
    u32 triangles_count = u32( max_meshlets * max_triangles * 3 );
    meshlet_triangles.init( temp_allocator, triangles_count, triangles_count );

    u32 index_offset = mesh.index_offset_bytes / sizeof( u32 );
    u32* indices = mesh_buffers.indices.data + index_offset;
    u32 position_offset = mesh.position_offset / sizeof( glm::vec3 );

    sizet meshlet_count = 0;
    if ( mesh.index_type == VK_INDEX_TYPE_UINT16 ) {
        meshlet_count = meshopt_buildMeshlets( local_meshlets.data, meshlet_vertex_indices.data, meshlet_triangles.data,
                                               ( u16* )indices, mesh.index_count,
                                               ( f32* )( mesh_buffers.vertices.data + position_offset ), mesh.position_count, sizeof( glm::vec3 ),
                                               max_vertices, max_triangles, cone_weight );
    } else {
        meshlet_count = meshopt_buildMeshlets( local_meshlets.data, meshlet_vertex_indices.data, meshlet_triangles.data,
                                               indices, mesh.index_count,
                                               ( f32* )( mesh_buffers.vertices.data + position_offset ), mesh.position_count, sizeof( glm::vec3 ),
                                               max_vertices, max_triangles, cone_weight );
    }

    // Write vertex data for meshlets. This is a _global_ buffer that contains data for all meshlets
    // When animating meshlets, a compute shader will read the original vertex data and update this vertex
    // buffer. A separate pass will then update the sphere and cone based on the new vertex positions and
    // normals.
    mesh.meshlet_vertex_offset = render_scene->meshlets_vertex_positions.size;
    for ( u32 v = 0; v < mesh.position_count; ++v ) {
        GpuMeshletVertexPosition meshlet_vertex_pos{ };

        glm::vec3& position = mesh_buffers.vertices[ v + position_offset ];
        f32 x = position.x;
        f32 y = position.y;
        f32 z = position.z;

        if ( x < mesh_aabb[ 0 ].x )
        {
            mesh_aabb[ 0 ].x = x;
        }
        if ( y < mesh_aabb[ 0 ].y )
        {
            mesh_aabb[ 0 ].y = y;
        }
        if ( z < mesh_aabb[ 0 ].z )
        {
            mesh_aabb[ 0 ].z = z;
        }

        if ( x > mesh_aabb[ 1 ].x )
        {
            mesh_aabb[ 1 ].x = x;
        }
        if ( y > mesh_aabb[ 1 ].y )
        {
            mesh_aabb[ 1 ].y = y;
        }
        if ( z > mesh_aabb[ 1 ].z )
        {
            mesh_aabb[ 1 ].z = z;
        }

        meshlet_vertex_pos.position[ 0 ] = x;
        meshlet_vertex_pos.position[ 1 ] = y;
        meshlet_vertex_pos.position[ 2 ] = z;

        render_scene->meshlets_vertex_positions.push( meshlet_vertex_pos );

        GpuMeshletVertexData meshlet_vertex_data{ };

        if ( mesh.pbr_material.flags & DrawFlags_HasNormals ) {
            u32 normal_offset = mesh.normal_offset / sizeof( glm::vec3 );
            glm::vec3& normal = mesh_buffers.normals[ v + normal_offset ];

            meshlet_vertex_data.normal[ 0 ] = u8( ( normal.x + 1.0f ) * 127.0f );
            meshlet_vertex_data.normal[ 1 ] = u8( ( normal.y + 1.0f ) * 127.0f );
            meshlet_vertex_data.normal[ 2 ] = u8( ( normal.z + 1.0f ) * 127.0f );
        }

        if ( mesh.pbr_material.flags & DrawFlags_HasTangents ) {
            u32 tangent_offset = mesh.tangent_offset / sizeof( glm::vec4 );
            glm::vec4& tangent = mesh_buffers.tangents[ v + tangent_offset ];

            meshlet_vertex_data.tangent[ 0 ] = u8( ( tangent.x + 1.0f ) * 127.0f );
            meshlet_vertex_data.tangent[ 1 ] = u8( ( tangent.y + 1.0f ) * 127.0f );
            meshlet_vertex_data.tangent[ 2 ] = u8( ( tangent.z + 1.0f ) * 127.0f );
            meshlet_vertex_data.tangent[ 3 ] = u8( ( tangent.w + 1.0f ) * 127.0f );
        }

        if ( mesh.pbr_material.flags & DrawFlags_HasTexCoords ) {
            u32 texcoord_offset = mesh.texcoord_offset / sizeof( glm::vec2 );
            glm::vec2& tex_coords = mesh_buffers.tex_coords[ v + texcoord_offset ];

            meshlet_vertex_data.uv_coords[ 0 ] = meshopt_quantizeHalf( tex_coords.x );
            meshlet_vertex_data.uv_coords[ 1 ] = meshopt_quantizeHalf( tex_coords.y );
        }

        render_scene->meshlets_vertex_data.push( meshlet_vertex_data );
    }

    mesh.aabb[ 0 ] = mesh_aabb[ 0 ];
    mesh.aabb[ 1 ] = mesh_aabb[ 1 ];

    const glm::vec3 quantization_aabb_min = mesh.aabb[ 0 ];
    const glm::vec3 quantization_aabb_max = mesh.aabb[ 1 ];

    // Cache meshlet offset. This is the start of the meshlets for this mesh
    mesh.meshlet_offset = render_scene->meshlets.size;
    mesh.meshlet_count = ( u32 )meshlet_count;
    mesh.meshlet_index_count = 0;

    // Append meshlet connectivity data
    // First store the vertex indices, then the grouped triangle indices
    for ( u32 m = 0; m < meshlet_count; ++m ) {
        meshopt_Meshlet& local_meshlet = local_meshlets[ m ];

        meshopt_Bounds meshlet_bounds = meshopt_computeMeshletBounds( meshlet_vertex_indices.data + local_meshlet.vertex_offset,
                                                                      meshlet_triangles.data + local_meshlet.triangle_offset, local_meshlet.triangle_count,
                                                                      ( f32* )( mesh_buffers.vertices.data + position_offset ), mesh.position_count, sizeof( glm::vec3 ) );

        GpuMeshlet meshlet{};
        meshlet.connectivity_data_offset = render_scene->meshlets_connectivity_data.size;
        meshlet.vertex_count = local_meshlet.vertex_count;
        meshlet.triangle_count = local_meshlet.triangle_count;

        meshlet.center = glm::vec3{ meshlet_bounds.center[ 0 ], meshlet_bounds.center[ 1 ], meshlet_bounds.center[ 2 ] };
        meshlet.radius = meshlet_bounds.radius;

        meshlet.cone_axis[ 0 ] = meshlet_bounds.cone_axis_s8[ 0 ];
        meshlet.cone_axis[ 1 ] = meshlet_bounds.cone_axis_s8[ 1 ];
        meshlet.cone_axis[ 2 ] = meshlet_bounds.cone_axis_s8[ 2 ];

        meshlet.cone_cutoff = meshlet_bounds.cone_cutoff_s8;
        meshlet.mesh_index = render_scene->meshes.size;

        // Resize data array
        const u32 index_group_count = ( local_meshlet.triangle_count * 3 + 3 ) / 4;
        render_scene->meshlets_connectivity_data.set_capacity( render_scene->meshlets_connectivity_data.size + local_meshlet.vertex_count + index_group_count );
        render_scene->meshlets_position_only_data.set_capacity( render_scene->meshlets_connectivity_data.size + local_meshlet.vertex_count + index_group_count );

        for ( u32 i = 0; i < meshlet.vertex_count; ++i ) {
            const u32 local_vertex_index = meshlet_vertex_indices[ local_meshlet.vertex_offset + i ];
            const u32 global_vertex_index = mesh.meshlet_vertex_offset + local_vertex_index;
            const glm::vec3 position = mesh_buffers.vertices[ position_offset + local_vertex_index ];

            render_scene->meshlets_connectivity_data.push( global_vertex_index );
            render_scene->meshlets_position_only_data.push( pack_mesh_position_101010( position, mesh.aabb[ 0 ], mesh.aabb[ 1 ] ) );
        }

        // Store indices as uint32
        // NOTE(marco): we write 4 indices at at time, it will come in handy in the mesh shader
        const u32* index_groups = reinterpret_cast< const u32* >( meshlet_triangles.data + local_meshlet.triangle_offset );
        for ( u32 i = 0; i < index_group_count; ++i ) {
            const u32 index_group = index_groups[ i ];
            render_scene->meshlets_connectivity_data.push( index_group );
            render_scene->meshlets_position_only_data.push( index_group );
        }

        mesh.meshlet_index_count += meshlet.triangle_count * 3;

        render_scene->meshlets.push( meshlet );

        render_scene->meshlets_index_count += index_group_count;
    }

    while ( render_scene->meshlets.size % 32 ) {
        render_scene->meshlets.push( GpuMeshlet() );
    }
}

// RenderScene ////////////////////////////////////////////////////////////
u32 RenderScene::write_vertices_to_staging_buffer( glm::vec3* vertices, u32 vertex_count ) {
    u32 buffer_offset = mesh_buffers.vertices.size;
    if ( mesh_buffers.vertices.size == 0 ) {
        mesh_buffers.vertices.init( resident_allocator, vertex_count );
    } else if ( mesh_buffers.vertices.size + vertex_count > mesh_buffers.vertices.capacity ) {
        mesh_buffers.vertices.grow( mesh_buffers.vertices.size + vertex_count );
    }
    mesh_buffers.vertices.size += vertex_count;

    memcpy( mesh_buffers.vertices.data + buffer_offset, vertices, vertex_count * sizeof( glm::vec3 ) );
    return buffer_offset * sizeof( glm::vec3 );
}

u32 RenderScene::write_indices_to_staging_buffer( u32* indices, u32 index_count ) {
    u32 buffer_offset = mesh_buffers.indices.size;
    if ( mesh_buffers.indices.size == 0 ) {
        mesh_buffers.indices.init( resident_allocator, index_count );
    } else if ( mesh_buffers.indices.size + index_count > mesh_buffers.indices.capacity ) {
        mesh_buffers.indices.grow( mesh_buffers.indices.size + index_count );
    }
    mesh_buffers.indices.size += index_count;

    memcpy( mesh_buffers.indices.data + buffer_offset, indices, index_count * sizeof( u32 ) );
    return buffer_offset * sizeof( u32 );
}

u32 RenderScene::write_normals_to_staging_buffer( glm::vec3* normals, u32 normal_count ) {
    u32 buffer_offset = mesh_buffers.normals.size;
    if ( mesh_buffers.normals.size == 0 ) {
        mesh_buffers.normals.init( resident_allocator, normal_count );
    } else if ( mesh_buffers.normals.size + normal_count > mesh_buffers.normals.capacity ) {
        mesh_buffers.normals.grow( mesh_buffers.normals.size + normal_count );
    }
    mesh_buffers.normals.size += normal_count;

    memcpy( mesh_buffers.normals.data + buffer_offset, normals, normal_count * sizeof( glm::vec3 ) );
    return buffer_offset * sizeof( glm::vec3 );
}

u32 RenderScene::write_tangents_to_staging_buffer( glm::vec3* tangents, u32 tangent_count ) {
    u32 buffer_offset = mesh_buffers.tangents.size;
    if ( mesh_buffers.tangents.size == 0 ) {
        mesh_buffers.tangents.init( resident_allocator, tangent_count );
    } else if ( mesh_buffers.tangents.size + tangent_count > mesh_buffers.tangents.capacity ) {
        mesh_buffers.tangents.grow( mesh_buffers.tangents.size + tangent_count );
    }
    mesh_buffers.tangents.size += tangent_count;

    glm::vec4* tangents_4 = mesh_buffers.tangents.data + buffer_offset;
    for ( u32 i = 0; i < tangent_count; ++i ) {
        tangents_4[ i ] = glm::vec4( tangents[ i ], 1.0f );
    }
    return buffer_offset * sizeof( glm::vec4 );
}

u32 RenderScene::write_tangents_to_staging_buffer( glm::vec4* tangents, u32 tangent_count ) {
    u32 buffer_offset = mesh_buffers.tangents.size;
    if ( mesh_buffers.tangents.size == 0 ) {
        mesh_buffers.tangents.init( resident_allocator, tangent_count );
    } else if ( mesh_buffers.tangents.size + tangent_count > mesh_buffers.tangents.capacity ) {
        mesh_buffers.tangents.grow( mesh_buffers.tangents.size + tangent_count );
    }
    mesh_buffers.tangents.size += tangent_count;

    memcpy( mesh_buffers.tangents.data + buffer_offset, tangents, tangent_count * sizeof( glm::vec4 ) );
    return buffer_offset * sizeof( glm::vec4 );
}

u32 RenderScene::write_texcoords_to_staging_buffer( glm::vec2* texcoords, u32 texcoord_count ) {
    u32 buffer_offset = mesh_buffers.tex_coords.size;
    if ( mesh_buffers.tex_coords.size == 0 ) {
        mesh_buffers.tex_coords.init( resident_allocator, texcoord_count );
    } else if ( mesh_buffers.tex_coords.size + texcoord_count > mesh_buffers.tex_coords.capacity ) {
        mesh_buffers.tex_coords.grow( mesh_buffers.tex_coords.size + texcoord_count );
    }
    mesh_buffers.tex_coords.size += texcoord_count;

    memcpy( mesh_buffers.tex_coords.data + buffer_offset, texcoords, texcoord_count * sizeof( glm::vec2 ) );
    return buffer_offset * sizeof( glm::vec2 );
}

u32 RenderScene::write_texcoords_to_staging_buffer( glm::vec3* texcoords, u32 texcoord_count, bool flip_y ) {
    u32 buffer_offset = mesh_buffers.tex_coords.size;
    if ( mesh_buffers.tex_coords.size == 0 ) {
        mesh_buffers.tex_coords.init( resident_allocator, texcoord_count );
    } else if ( mesh_buffers.tex_coords.size + texcoord_count > mesh_buffers.tex_coords.capacity ) {
        mesh_buffers.tex_coords.grow( mesh_buffers.tex_coords.size + texcoord_count );
    }
    mesh_buffers.tex_coords.size += texcoord_count;

    glm::vec2* texcoords_2 = mesh_buffers.tex_coords.data + buffer_offset;
    for ( u32 i = 0; i < texcoord_count; ++i ) {
        if ( flip_y ) {
            texcoords_2[ i ] = glm::vec2( texcoords[ i ].x, 1.0f - texcoords[ i ].y );
        } else {
            texcoords_2[ i ] = glm::vec2( texcoords[ i ].x, texcoords[ i ].y );
        }
    }
    return buffer_offset * sizeof( glm::vec2 );
}

u32 RenderScene::write_joints_to_staging_buffer( glm::u16vec4* joints, u32 joint_count ) {
    u32 buffer_offset = mesh_buffers.joints.size;
    if ( mesh_buffers.joints.size == 0 ) {
        mesh_buffers.joints.init( resident_allocator, joint_count );
    } else if ( mesh_buffers.joints.size + joint_count > mesh_buffers.joints.capacity ) {
        mesh_buffers.joints.grow( mesh_buffers.joints.size + joint_count );
    }
    mesh_buffers.joints.size += joint_count;

    memcpy( mesh_buffers.joints.data + buffer_offset, joints, joint_count * sizeof( glm::u16vec4 ) );
    return buffer_offset * sizeof( glm::u16vec4 );
}

u32 RenderScene::write_weights_to_staging_buffer( glm::vec4* weights, u32 weight_count ) {
    u32 buffer_offset = mesh_buffers.weights.size;
    if ( mesh_buffers.weights.size == 0 ) {
        mesh_buffers.weights.init( resident_allocator, weight_count );
    } else if ( mesh_buffers.weights.size + weight_count > mesh_buffers.weights.capacity ) {
        mesh_buffers.weights.grow( mesh_buffers.weights.size + weight_count );
    }
    mesh_buffers.weights.size += weight_count;

    memcpy( mesh_buffers.weights.data + buffer_offset, weights, weight_count * sizeof( glm::vec4 ) );
    return buffer_offset * sizeof( glm::vec4 );
}

void RenderScene::destroy_mesh_buffers() {
    mesh_buffers.vertices.shutdown();
    mesh_buffers.normals.shutdown();
    mesh_buffers.tangents.shutdown();
    mesh_buffers.tex_coords.shutdown();
    mesh_buffers.joints.shutdown();
    mesh_buffers.weights.shutdown();
    mesh_buffers.indices.shutdown();
}

CommandBuffer* RenderScene::update_physics( f32 delta_time, f32 air_density, f32 spring_stiffness, f32 spring_damping, glm::vec3 wind_direction, bool reset_simulation ) {
    // Based on http://graphics.stanford.edu/courses/cs468-02-winter/Papers/Rigidcloth.pdf
    RASSERT( false );
    return nullptr;
#if 0
    // NOTE(marco): left for reference
    const u32 sim_steps = 10;
    const f32 dt_multiplier = 1.0f / sim_steps;
    delta_time *= dt_multiplier;

    const glm::vec3 g{ 0.0f, -9.8f, 0.0f };

    for ( u32 m = 0; m < meshes.size; ++m ) {
        Mesh& mesh = meshes[ m ];

        PhysicsMesh* physics_mesh = mesh.physics_mesh;

        if ( physics_mesh != nullptr ) {
            const glm::vec3 fixed_vertex_1{ 0.0f,  1.0f, -1.0f };
            const glm::vec3 fixed_vertex_2{ 0.0f,  1.0f,  1.0f };
            const glm::vec3 fixed_vertex_3{ 0.0f, -1.0f,  1.0f };
            const glm::vec3 fixed_vertex_4{ 0.0f, -1.0f, -1.0f };

            if ( reset_simulation ) {
                for ( u32 v = 0; v < physics_mesh->vertices.size; ++v ) {
                    PhysicsVertex& vertex = physics_mesh->vertices[ v ];
                    vertex.position = vertex.start_position;
                    vertex.previous_position = vertex.start_position;
                    vertex.velocity = glm::vec3{ };
                    vertex.force = glm::vec3{ };
                }
            }

            for ( u32 s = 0; s < sim_steps; ++s ) {
                // First calculate the force to apply to each vertex
                for ( u32 v = 0; v < physics_mesh->vertices.size; ++v ) {
                    PhysicsVertex& vertex = physics_mesh->vertices[ v ];

                    if ( glms_vec3_eqv( vertex.start_position, fixed_vertex_1 ) || glms_vec3_eqv( vertex.start_position, fixed_vertex_2 ) ||
                        glms_vec3_eqv( vertex.start_position, fixed_vertex_3 ) || glms_vec3_eqv( vertex.start_position, fixed_vertex_4 )) {
                        continue;
                    }

                    f32 m = vertex.mass;

                    glm::vec3 spring_force{ };

                    for ( u32 j = 0; j < vertex.joint_count; ++j ) {
                        PhysicsVertex& other_vertex = physics_mesh->vertices[ vertex.joints[ j ].vertex_index ];

                        f32 spring_rest_length =  glms_vec3_distance( vertex.start_position, other_vertex.start_position );

                        glm::vec3 pull_direction = glms_vec3_sub( vertex.position, other_vertex.position );
                        glm::vec3 relative_pull_direction = glms_vec3_sub( pull_direction, glms_vec3_scale( glms_vec3_normalize( pull_direction ), spring_rest_length ) );
                        pull_direction = glms_vec3_scale( relative_pull_direction, spring_stiffness );
                        spring_force = glms_vec3_add( spring_force, pull_direction );
                    }

                    glm::vec3 viscous_damping = glms_vec3_scale( vertex.velocity, -spring_damping );

                    glm::vec3 viscous_velocity = glms_vec3_sub( wind_direction, vertex.velocity );
                    viscous_velocity = glms_vec3_scale( vertex.normal, glms_vec3_dot( vertex.normal, viscous_velocity ) );
                    viscous_velocity = glms_vec3_scale( viscous_velocity, air_density );

                    vertex.force = glms_vec3_scale( g, m );
                    vertex.force = glms_vec3_sub( vertex.force, spring_force );
                    vertex.force = glms_vec3_add( vertex.force, viscous_damping );
                    vertex.force = glms_vec3_add( vertex.force, viscous_velocity );
                }

                // Then update their position
                for ( u32 v = 0; v < physics_mesh->vertices.size; ++v ) {
                    PhysicsVertex& vertex = physics_mesh->vertices[ v ];

                    glm::vec3 previous_position = vertex.previous_position;
                    glm::vec3 current_position = vertex.position;

                    // Verlet integration
                    vertex.position = glms_vec3_scale( current_position, 2.0f );
                    vertex.position = glms_vec3_sub( vertex.position, previous_position );
                    vertex.position = glms_vec3_add( vertex.position, glms_vec3_scale( vertex.force, delta_time * delta_time ) );

                    vertex.previous_position = current_position;

                    vertex.velocity = glms_vec3_sub( vertex.position, current_position );
                }
            }

            Buffer* position_buffer = renderer->gpu->access_buffer( mesh.position_buffer );
            glm::vec3* positions = ( glm::vec3* )( position_buffer->mapped_data + mesh.position_offset );

            Buffer* normal_buffer = renderer->gpu->access_buffer( mesh.normal_buffer );
            glm::vec3* normals = ( glm::vec3* )( normal_buffer->mapped_data + mesh.normal_offset );

            Buffer* tangent_buffer = renderer->gpu->access_buffer( mesh.tangent_buffer );
            glm::vec3* tangents = ( glm::vec3* )( tangent_buffer->mapped_data + mesh.tangent_offset );

            Buffer* index_buffer = renderer->gpu->access_buffer( mesh.index_buffer );
            u32* indices = ( u32* )( index_buffer->mapped_data + mesh.index_offset );

            for ( u32 v = 0; v < physics_mesh->vertices.size; ++v ) {
                positions[ v ] = physics_mesh->vertices[ v ].position;
            }

            for ( u32 i = 0; i < mesh.primitive_count; i += 3 ) {
                u32 i0 = indices[ i + 0 ];
                u32 i1 = indices[ i + 1 ];
                u32 i2 = indices[ i + 2 ];

                glm::vec3 p0 = physics_mesh->vertices[ i0 ].position;
                glm::vec3 p1 = physics_mesh->vertices[ i1 ].position;
                glm::vec3 p2 = physics_mesh->vertices[ i2 ].position;

                // TODO(marco): better normal compuation, also update tangents
                glm::vec3 edge1 = glms_vec3_sub( p1, p0 );
                glm::vec3 edge2 = glms_vec3_sub( p2, p0 );

                glm::vec3 n = glms_cross( edge1, edge2 );

                physics_mesh->vertices[ i0 ].normal = glms_normalize( glms_vec3_add( normals[ i0 ], n ) );
                physics_mesh->vertices[ i1 ].normal = glms_normalize( glms_vec3_add( normals[ i1 ], n ) );
                physics_mesh->vertices[ i2 ].normal = glms_normalize( glms_vec3_add( normals[ i2 ], n ) );

                normals[ i0 ] = physics_mesh->vertices[ i0 ].normal;
                normals[ i1 ] = physics_mesh->vertices[ i1 ].normal;
                normals[ i2 ] = physics_mesh->vertices[ i2 ].normal;
            }
        }
    }
//#else
    if ( renderer->render_data.physics_cb.is_invalid() )
        return nullptr;

    GpuDevice& gpu = *renderer->gpu;

    MapBufferParameters physics_cb_map = { renderer->render_data.physics_cb, 0, 0 };
    PhysicsSceneData* gpu_physics_data = ( PhysicsSceneData* )gpu.map_buffer( physics_cb_map );
    if ( gpu_physics_data ) {
        gpu_physics_data->wind_direction = wind_direction;
        gpu_physics_data->reset_simulation = reset_simulation ? 1 : 0;
        gpu_physics_data->air_density = air_density;
        gpu_physics_data->spring_stiffness = spring_stiffness;
        gpu_physics_data->spring_damping = spring_damping;

        gpu.unmap_buffer( physics_cb_map );
    }

    CommandBuffer* cb = nullptr;

    for ( u32 m = 0; m < meshes.size; ++m ) {
        Mesh& mesh = meshes[ m ];

        PhysicsMesh* physics_mesh = mesh.physics_mesh;

        if ( physics_mesh != nullptr ) {
            if ( !gpu.buffer_ready( mesh.position_buffer ) ||
                 !gpu.buffer_ready( mesh.normal_buffer ) ||
                 !gpu.buffer_ready( mesh.tangent_buffer ) ||
                 !gpu.buffer_ready( mesh.index_buffer ) ||
                 !gpu.buffer_ready( physics_mesh->gpu_buffer ) ||
                 !gpu.buffer_ready( physics_mesh->draw_indirect_buffer ) ) {
                continue;
            }

            if ( cb == nullptr ) {
                cb = gpu.get_command_buffer( 0, gpu.current_frame, true );

                cb->push_marker( "Frame" );
                cb->push_marker( "async" );

                const u64 cloth_hashed_name = hash_calculate( "cloth" );
                GpuTechnique* cloth_technique = renderer->resource_cache.techniques.get( cloth_hashed_name );

                cb->bind_pipeline( cloth_technique->passes[ 0 ].pipeline );
            }

            cb->bind_descriptor_set( &physics_mesh->descriptor_set, 1, nullptr, 0 );

            // TODO(marco): submit all meshes at once
            cb->dispatch( 1, 1, 1 );
        }
    }

    if ( cb != nullptr ) {
        cb->pop_marker();
        cb->pop_marker();

        // If marker are present, then queries are as well.
        /*if ( cb->thread_frame_pool->time_queries->allocated_time_query ) {
            vkCmdEndQuery( cb->vk_command_buffer, cb->thread_frame_pool->vulkan_pipeline_stats_query_pool, 0 );
        }*/

        cb->end();
    }

    return cb;
#endif
}

void RenderScene::update_animations( f32 delta_time ) {

    if ( animations.size == 0 ) {
        return;
    }

    // TODO: update the first animation as test
    Animation& animation = animations[ 0 ];

    current_animation_time += delta_time;
    if ( current_animation_time > animation.time_end ) {
        current_animation_time -= animation.time_end;
    }

    // TODO: fix skeleton/scene graph relationship
    for ( u32 i = 0; i < animation.animated_transforms.size; ++i ) {

        Transform& transform = animation.animated_transforms[ i ];
        transform.reset();
    }

    // Accumulate transformations
    // For each animation channel
    for ( u32 ac = 0; ac < animation.channels.size; ++ac ) {
        AnimationChannel& channel = animation.channels[ ac ];
        AnimationSampler& sampler = animation.samplers[ channel.sampler ];

        if ( sampler.interpolation_type != AnimationSampler::Linear ) {
            rprint( "Interpolation %s still not supported.\n", sampler.interpolation_type );
            continue;
        }

        // Scroll through all key frames
        for ( u32 ki = 0; ki < sampler.key_frames.size - 1; ++ki ) {
            const f32 keyframe = sampler.key_frames[ ki ];
            const f32 next_keyframe = sampler.key_frames[ ki + 1 ];
            if ( current_animation_time >= keyframe && current_animation_time <= next_keyframe ) {

                const f32 interpolation = ( current_animation_time - keyframe ) / ( next_keyframe - keyframe );

                Transform& transform = animation.animated_transforms[ channel.target_node ];
                switch ( channel.target_type ) {
                    case AnimationChannel::TargetType::Translation:
                    {
                        const glm::vec3 current_data{ sampler.data[ ki ].x, sampler.data[ ki ].y, sampler.data[ ki ].z };
                        const glm::vec3 next_data{ sampler.data[ ki + 1 ].x, sampler.data[ ki + 1 ].y, sampler.data[ ki + 1 ].z };
                        transform.translation = glm::mix( current_data, next_data, interpolation );

                        break;
                    }
                    case AnimationChannel::TargetType::Rotation:
                    {
                        const glm::vec4 current_data = sampler.data[ ki ];
                        const glm::quat current_rotation = glm::quat( current_data.w, current_data.x, current_data.y, current_data.z );

                        const glm::vec4 next_data = sampler.data[ ki + 1 ];
                        const glm::quat next_rotation = glm::quat( next_data.w, next_data.x, next_data.y, next_data.z );

                        transform.rotation = glm::normalize( glm::slerp( current_rotation, next_rotation, interpolation ) );

                        break;
                    }
                    case AnimationChannel::TargetType::Scale:
                    {
                        const glm::vec3 current_data{ sampler.data[ ki ].x, sampler.data[ ki ].y, sampler.data[ ki ].z };
                        const glm::vec3 next_data{ sampler.data[ ki + 1 ].x, sampler.data[ ki + 1 ].y, sampler.data[ ki + 1 ].z };
                        transform.scale = glm::mix( current_data, next_data, interpolation );

                        break;
                    }
                    default:
                        break;
                }

                break;
            }
        }
    }
}

// TODO: remove, improve
glm::mat4 RenderScene::get_local_matrix( SceneGraph* scene_graph, u32 node_index ) {
    glm::mat4 local_matrix = scene_graph->local_matrices[ node_index ];
    if ( animations.size == 0 ) {
        return local_matrix;
    }

    Animation& animation = animations[ 0 ];
    if ( node_index >= animation.animated_transforms.size ) {
        return local_matrix;
    }

    glm::mat4 a = animation.animated_transforms[ node_index ].calculate_matrix();
    // NOTE(marco): according to the GLTF spec (3.7.3.2)
    // Only the joint transforms are applied to the skinned mesh; the transform of the skinned mesh node MUST be ignored

    // TODO(marco): if the animation is not playing, we should return the local matrix
    return a;
}

glm::mat4 RenderScene::get_node_transform( SceneGraph* scene_graph, u32 node_index ) {
    glm::mat4 node_transform = get_local_matrix( scene_graph, node_index );

    i32 parent = scene_graph->nodes_hierarchy[ node_index ].parent;
    while ( parent >= 0 ) {
        node_transform = get_local_matrix( scene_graph, parent ) * node_transform;

        parent = scene_graph->nodes_hierarchy[ parent ].parent;
    }

    return node_transform;
}

void RenderScene::update_joints( FrameRenderer* frame_renderer ) {

    // Joint buffer is persistently mapped
    for ( u32 i = 0; i < skins.size; ++i ) {
        Skin& skin = skins[ i ];

        BufferHandle joint_buffer_handle = skin.joint_transforms[ renderer->gpu->current_frame ];
        Buffer* joint_buffer = renderer->gpu->get_buffer( joint_buffer_handle );
        glm::mat4* joint_transforms = ( glm::mat4* )joint_buffer->mapped_data;

        for ( u32 ji = 0; ji < skin.joints.size; ++ji ) {
            const u32 joint = skin.joints[ ji ];
            joint_transforms[ ji ] = get_node_transform( scene_graph, joint ) * skin.inverse_bind_matrices[ ji ];
        }

        renderer->gpu->flush_buffer( joint_buffer_handle, 0, skin.joints.size * sizeof( glm::mat4 ) );
    }

    DebugDrawRenderingFeature* debug_draw_feature = frame_renderer->debug_draw;
    if ( debug_draw_feature == nullptr ) {
        return;
    }

    for ( u32 i = 0; i < scene_graph->nodes_hierarchy.size; i++ ) {
        SceneGraphNodeDebugData& debug_data = scene_graph->nodes_debug_data[ i ];
        if ( !debug_data.is_bone ) continue;

        // Get this bone's world position
        glm::mat4 bone_world_matrix = get_node_transform( scene_graph, i );
        glm::vec3 bone_position = glm::vec3( bone_world_matrix[3] );

        // Get parent bone's world position and draw line
        i32 parent_index = scene_graph->nodes_hierarchy[ i ].parent;
        if ( parent_index >= 0 && scene_graph->nodes_debug_data[ parent_index ].is_bone ) {
            glm::mat4 parent_world_matrix = get_node_transform( scene_graph, parent_index );
            glm::vec3 parent_position = glm::vec3( parent_world_matrix[3] );

            debug_draw_feature->line( parent_position, bone_position, Color::yellow() );
        }
    }
}

void RenderScene::init( SceneGraph* scene_graph_, Allocator* resident_allocator_, Renderer* renderer_ )
{
    resident_allocator = resident_allocator_;
    renderer = renderer_;
    scene_graph = scene_graph_;

    names_buffer.init( rkilo( 64 ), resident_allocator );

    meshlets_index_count = 0;

    models.init( resident_allocator, 8 );

    images.init( resident_allocator, 32 );
    samplers.init( resident_allocator, 8 );

    animations.init( resident_allocator, 8 );
    skins.init( resident_allocator, 8 );

    mesh_instances.init( resident_allocator, 32 );
    meshes.init( resident_allocator, 16 );
    meshlets.init( resident_allocator, 16 );
    meshlets_connectivity_data.init( resident_allocator, 16 );
    meshlets_position_only_data.init( resident_allocator, 16 );
    meshlets_vertex_positions.init( resident_allocator, 16 );
    meshlets_vertex_data.init( resident_allocator, 16 );
}

RenderModel* RenderScene::add_and_load_model( char* filename, cstring path, ArenaAllocator* temp_allocator )
{
    char* file_extension = file_extension_from_path( filename );

    RenderModel* model = nullptr;

    if ( strcmp( file_extension, "gltf" ) == 0 ) {
        model = new glTFModel;
    } else if ( strcmp( file_extension, "obj" ) == 0 ) {
        model = new ObjModel;
    } else if ( strcmp( file_extension, "fbx" ) == 0 ) {
        model = new FbxModel;
    } else {
        rprint( "Unsupported model format: %s\n", file_extension );
        return nullptr;
    }

    model->init( this, scene_graph, resident_allocator, renderer );

    model->load_model( filename, path, temp_allocator );

    models.push( model );

    return model;
}

void RenderScene::prepare_draws( Renderer* renderer, ArenaAllocator* scratch_allocator, SceneGraph* scene_graph ) {

    lights.init( resident_allocator, k_num_lights );

    // Add a first light in a fixed position and then random lights.
    const u32 lights_per_side = raptor::ceilu32( sqrtf( active_lights * 1.f ) );
    {
        const f32 x = 0;
        const f32 y = .5f;
        const f32 z = -1.2f;

        float r = 1;
        float g = 1;
        float b = 1;

        {
            Light new_light{ };
            new_light.world_position = glm::vec3{ x, y, z };
            new_light.radius = 12.888f;

            new_light.color = glm::vec3{ r, g, b };
            new_light.intensity = 3.0f;

            glm::vec3 aabb_min = new_light.world_position - glm::vec3{ new_light.radius };
            glm::vec3 aabb_max = new_light.world_position + glm::vec3{ new_light.radius };

            new_light.aabb_min = glm::vec4{ aabb_min.x, aabb_min.y, aabb_min.z, 1.0f };
            new_light.aabb_max = glm::vec4{ aabb_max.x, aabb_max.y, aabb_max.z, 1.0f };

            lights.push( new_light );
        }

        for ( u32 i = 1; i < k_num_lights; ++i ) {

            const f32 x = ( i % lights_per_side ) - lights_per_side * .7f;
            const f32 y = 0.1f;
            const f32 z = ( i / lights_per_side ) - lights_per_side * .7f;

            /*float x = get_random_value( mesh_aabb[ 0 ].x * scale, mesh_aabb[ 1 ].x * scale );
            float y = get_random_value( mesh_aabb[ 0 ].y * scale, mesh_aabb[ 1 ].y * scale );
            float z = get_random_value( mesh_aabb[ 0 ].z * scale, mesh_aabb[ 1 ].z * scale );*/

            f32 r = get_random_value( 0.1f, 1.0f );
            f32 g = get_random_value( 0.1f, 1.0f );
            f32 b = get_random_value( 0.1f, 1.0f );

            Light new_light{ };
            new_light.world_position = glm::vec3{ x, y, z };
            new_light.radius = 0.6f;

            glm::vec3 aabb_min = new_light.world_position - glm::vec3{ new_light.radius };
            glm::vec3 aabb_max = new_light.world_position + glm::vec3{ new_light.radius };

            new_light.aabb_min = glm::vec4{ aabb_min.x, aabb_min.y, aabb_min.z, 1.0f };
            new_light.aabb_max = glm::vec4{ aabb_max.x, aabb_max.y, aabb_max.z, 1.0f };

            new_light.color = glm::vec3{ r, g, b };
            new_light.intensity = 3.0f;

            lights.push( new_light );
        }
    }

    // Create per mesh descriptor sets, using the mesh draw and lighting ssbos
    //const u64 hashed_name = hash_calculate( "main" );
    //GpuTechnique* main_technique = renderer->resource_cache.techniques.get( hashed_name );

    // Create material
    /*MaterialCreation material_creation;
    material_creation.set_name( "material_no_cull_opaque" ).set_technique( main_technique ).set_render_index( 0 );

    const u64 cloth_hashed_name = hash_calculate( "cloth" );
    GpuTechnique* cloth_technique = renderer->resource_cache.techniques.get( cloth_hashed_name );

    const u64 debug_hashed_name = hash_calculate( "debug" );
    GpuTechnique* debug_technique = renderer->resource_cache.techniques.get( debug_hashed_name );*/

    // TODO: refactor this
    //Material* pbr_material = renderer->create_material( material_creation );

    //for ( u32 m = 0; m < meshes.size; ++m ) {
    //    Mesh& mesh = meshes[ m ];

    //    mesh.pbr_material.material = pbr_material;

    //    // Create material buffer
    //    BufferCreation buffer_creation;
    //    //buffer_creation.reset().set( VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, ResourceUsageType::Dynamic, sizeof( GpuMaterialData ) ).set_name( "mesh_data" );
    //    mesh.pbr_material.material_buffer = renderer->gpu->create_buffer( {
    //        .type_flags = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, .usage = ResourceUsageType::Dynamic,
    //        .size = ( u32 )( sizeof( GpuMaterialData ) ),
    //        .name = "mesh_data" } );

    //    u32 pass_index = 0;
    //    u32 depth_pass_index = 0;

    //    if ( mesh.has_skinning() ) {
    //        pass_index = main_technique->name_hash_to_index.get( hash_calculate( "transparent_skinning_no_cull" ) );
    //        depth_pass_index = main_technique->name_hash_to_index.get( hash_calculate( "depth_pre_skinning" ) );
    //    } else {
    //        pass_index = main_technique->name_hash_to_index.get( hash_calculate( "transparent_no_cull" ) );
    //        depth_pass_index = main_technique->name_hash_to_index.get( hash_calculate( "depth_pre" ) );
    //    }

    //    DescriptorSetBinder descriptors;

    //    GpuTechniquePass& main_mesh_pass = main_technique->passes[ pass_index ];
    //    DescriptorSetLayoutHandle main_layout = renderer->gpu->get_descriptor_set_layout( main_mesh_pass.pipeline, k_material_descriptor_set_index );

    //    if ( mesh.has_skinning() ) {
    //        descriptors.ssbos.push( { skins[ mesh.skin_index ].joint_transforms, 3 } );
    //    }
    //    // Create main descriptor set
    //    mesh.pbr_material.descriptor_set_transparent = create_descriptor_set( descriptors, main_mesh_pass, main_layout, 0 );

    //    // Create depth descriptor set
    //    GpuTechniquePass& depth_mesh_pass = main_technique->passes[ depth_pass_index ];
    //    DescriptorSetLayoutHandle depth_layout = renderer->gpu->get_descriptor_set_layout( depth_mesh_pass.pipeline, k_material_descriptor_set_index );

    //    descriptors.reset();

    //    mesh.pbr_material.descriptor_set_main = create_descriptor_set( descriptors, depth_mesh_pass, depth_layout, 0 );

    //    if ( mesh.physics_mesh != nullptr ) {
    //        DescriptorSetLayoutHandle physics_layout = renderer->gpu->get_descriptor_set_layout( cloth_technique->passes[ 0 ].pipeline, k_material_descriptor_set_index );

    //        descriptors.reset();
    //        descriptors.buffers.push({ renderer->render_blackboard.physics_cb, 0 });
    //        descriptors.ssbos.push({ mesh.physics_mesh->gpu_buffer, 1 });
    //        descriptors.ssbos.push({ mesh.position_buffer, 2 });
    //        descriptors.ssbos.push({ mesh.normal_buffer, 3 });
    //        descriptors.ssbos.push({ mesh.index_buffer, 4 });

    //        mesh.physics_mesh->descriptor_set = renderer->gpu->create_descriptor_set({
    //            .buffers = { descriptors.buffers.data, descriptors.buffers.size },
    //            .ssbos = { descriptors.ssbos.data, descriptors.ssbos.size },
    //            .layout = physics_layout });

    //        DescriptorSetLayoutHandle debug_mesh_layout = renderer->gpu->get_descriptor_set_layout( debug_technique->passes[ 0 ].pipeline, k_material_descriptor_set_index );

    //        mesh.physics_mesh->debug_mesh_descriptor_set = renderer->gpu->create_descriptor_set( {
    //            .buffers = {{mesh.physics_mesh->gpu_buffer, 1}},
    //            .dynamic_buffers = {{0, sizeof( GpuFrameData ) }},
    //            .layout = debug_mesh_layout } );
    //    }
    //}
}

void RenderScene::shutdown( Renderer* renderer )
{
     GpuDevice& gpu = *renderer->gpu;

    // Unload animations
    for ( u32 ai = 0; ai < animations.size; ++ai ) {
        Animation& animation = animations[ ai ];
        animation.channels.shutdown();
        animation.animated_transforms.shutdown();

        for ( u32 si = 0; si < animation.samplers.size; ++si ) {
            AnimationSampler& sampler = animation.samplers[ si ];
            sampler.key_frames.shutdown();
            rfree( sampler.data, resident_allocator );
        }
        animation.samplers.shutdown();
    }
    animations.shutdown();

    // Unload skins
    for ( u32 si = 0; si < skins.size; ++si ) {
        Skin& skin = skins[ si ];
        skin.joints.shutdown();
        skin.inverse_bind_matrices.shutdown();

        for ( u32 f = 0; f < k_max_frames; ++f ) {
            renderer->gpu->destroy_buffer( skin.joint_transforms[ f ] );
        }
    }
    skins.shutdown();

    // Unload meshlets
    meshlets.shutdown();
    meshlets_vertex_data.shutdown();
    meshlets_vertex_positions.shutdown();
    meshlets_connectivity_data.shutdown();
    meshlets_position_only_data.shutdown();

    // Unload meshes
    for ( u32 mesh_index = 0; mesh_index < meshes.size; ++mesh_index ) {
        Mesh& mesh = meshes[ mesh_index ];

        gpu.destroy_buffer( mesh.pbr_material.material_buffer );
        gpu.destroy_descriptor_set( mesh.pbr_material.descriptor_set_transparent );
        gpu.destroy_descriptor_set( mesh.pbr_material.descriptor_set_main );

        PhysicsMesh* physics_mesh = mesh.physics_mesh;

        if ( physics_mesh != nullptr ) {
            gpu.destroy_descriptor_set( physics_mesh->descriptor_set );
            gpu.destroy_descriptor_set( physics_mesh->debug_mesh_descriptor_set );

            physics_mesh->vertices.shutdown();

            resident_allocator->deallocate( physics_mesh );
        }
    }

    destroy_mesh_buffers();

    for ( u32 i = 0; i < images.size; ++i) {
        renderer->destroy_texture( &images[ i ] );
    }

    for ( u32 i = 0; i < samplers.size; ++i ) {
        renderer->destroy_sampler( &samplers[ i ] );
    }

    lights.shutdown();

    meshes.shutdown();
    mesh_instances.shutdown();

    names_buffer.shutdown();

    for ( u32 i = 0; i < models.size; ++i ) {
        models[ i ]->shutdown( renderer );
        delete models[ i ];
    }
    models.shutdown();

    // Free scene buffers
    samplers.shutdown();
    images.shutdown();
}

void RenderScene::on_resize( GpuDevice& gpu, FrameGraph* frame_graph, u32 new_width, u32 new_height ) {

    //const u64 hashed_name = hash_calculate( "main" );
    //GpuTechnique* main_technique = renderer->resource_cache.techniques.get( hashed_name );

    //for ( u32 m = 0; m < meshes.size; ++m ) {
    //    Mesh& mesh = meshes[ m ];

    //    renderer->gpu->destroy_descriptor_set( mesh.pbr_material.descriptor_set_main );
    //    renderer->gpu->destroy_descriptor_set( mesh.pbr_material.descriptor_set_transparent );

    //    u32 pass_index = 0;
    //    u32 depth_pass_index = 0;

    //    if ( mesh.has_skinning() ) {
    //        pass_index = main_technique->name_hash_to_index.get( hash_calculate( "transparent_skinning_no_cull" ) );
    //        depth_pass_index = main_technique->name_hash_to_index.get( hash_calculate( "depth_pre_skinning" ) );
    //    } else {
    //        pass_index = main_technique->name_hash_to_index.get( hash_calculate( "transparent_no_cull" ) );
    //        depth_pass_index = main_technique->name_hash_to_index.get( hash_calculate( "depth_pre" ) );
    //    }

    //    DescriptorSetBinder descriptors;

    //    GpuTechniquePass& main_mesh_pass = main_technique->passes[ pass_index ];
    //    DescriptorSetLayoutHandle main_layout = renderer->gpu->get_descriptor_set_layout( main_mesh_pass.pipeline, k_material_descriptor_set_index );

    //    // if ( mesh.has_skinning() ) {
    //    //     descriptors.ssbos.push( { skins[ mesh.skin_index ].joint_transforms, 3 } );
    //    // }
    //    // Create main descriptor set
    //    mesh.pbr_material.descriptor_set_transparent = create_descriptor_set( descriptors, main_mesh_pass, main_layout, 0 );

    //    // Create depth descriptor set
    //    GpuTechniquePass& depth_mesh_pass = main_technique->passes[ depth_pass_index ];
    //    DescriptorSetLayoutHandle depth_layout = renderer->gpu->get_descriptor_set_layout( depth_mesh_pass.pipeline, k_material_descriptor_set_index );

    //    descriptors.reset();

    //    mesh.pbr_material.descriptor_set_main = create_descriptor_set( descriptors, depth_mesh_pass, depth_layout, 0 );
    //}
}

void RenderScene::draw_mesh_instance( CommandBuffer* gpu_commands, MeshInstance& mesh_instance, bool transparent ) {

    RASSERT( false );
    //Mesh& mesh = meshes[ mesh_instance.mesh_index ];
    //BufferHandle buffers[]{ mesh.position_buffer, mesh.tangent_buffer, mesh.normal_buffer, mesh.texcoord_buffer, mesh.joints_buffer, mesh.weights_buffer };
    //u32 offsets[]{ mesh.position_offset, mesh.tangent_offset, mesh.normal_offset, mesh.texcoord_offset, mesh.joints_offset, mesh.weights_offset };
    //gpu_commands->bind_vertex_buffers( buffers, 0, mesh.skin_index != i32_max ? 6 : 4, offsets );

    //gpu_commands->bind_index_buffer( mesh.index_buffer, mesh.index_offset, mesh.index_type );

    //if ( recreate_per_thread_descriptors ) {
    //    RASSERT( false );
    //    //DescriptorSetCreation ds_creation{};
    //    //ds_creation.buffer( scene_cb, 0 ).buffer( mesh_instances_sb, 10 ).buffer( meshes_sb, 2 );
    //    //DescriptorSetHandle descriptor_set = renderer->create_descriptor_set( gfx_cb, mesh.pbr_material.material, ds_creation );
    //    //gfx_cb->bind_local_descriptor_set( &descriptor_set, 1, nullptr, 0 );
    //} else {
    //    if ( transparent ) {
    //        gpu_commands->bind_descriptor_set(
    //            { renderer->gpu->bindless_descriptor_set, mesh.pbr_material.descriptor_set_transparent },
    //            { renderer->render_blackboard.scene_cb_offset, renderer->render_blackboard.lighting_constants_cb_offset } );
    //    }
    //    else {
    //        gpu_commands->bind_descriptor_set(
    //            { renderer->gpu->bindless_descriptor_set, mesh.pbr_material.descriptor_set_main },
    //            { renderer->render_blackboard.scene_cb_offset } );
    //    }
    //}

    // Gpu mesh index used to retrieve mesh data
    //gpu_commands->draw_indexed( TopologyType::Triangle, mesh.primitive_count, 1, 0, 0, mesh_instance.gpu_mesh_instance_index );
}

// Transform /////////////////////////////////////////////////////////////

void Transform::reset() {
    translation = { 0.f, 0.f, 0.f };
    scale = { 1.f, 1.f, 1.f };
    rotation = glm::quat( 1.0f, 0.f, 0.f, 0.f );
}

glm::mat4 Transform::calculate_matrix() const {

    const glm::mat4 translation_matrix = glm::translate( glm::mat4(1.0f), translation );
    const glm::mat4 scale_matrix = glm::scale( glm::mat4(1.0f), scale );
    const glm::mat4 local_matrix = translation_matrix * glm::mat4_cast( rotation ) * scale_matrix;
    return local_matrix;
}

// AnimationViewer ///////////////////////////////////////////////////////
void AnimationViewer::init( Allocator* allocator ) {
    channel_collapsed_state.init( allocator, 128 );
}

void AnimationViewer::shutdown() {
    channel_collapsed_state.shutdown();
}

void AnimationViewer::draw_imgui( Span<Animation> animations, SceneGraph& scene_graph, f32 animation_global_time ) {

    char channel_label[ 256 ];
    // Iterate through all animations
    for ( u32 anim_idx = 0; anim_idx < animations.size; ++anim_idx ) {
        Animation& animation = animations[ anim_idx ];

        if ( ImGui::TreeNode( ( void* )( intptr_t )anim_idx, "Animation %u (%.2fs - %.2fs)", anim_idx, animation.time_start, animation.time_end ) ) {
            ImGui::Text( "Duration: %.2fs", animation.time_end - animation.time_start );
            ImGui::Text( "Channels: %u", animation.channels.size );
            ImGui::Separator();

            // Iterate through each channel
            for ( u32 channel_idx = 0; channel_idx < animation.channels.size; ++channel_idx ) {
                AnimationChannel& channel = animation.channels[ channel_idx ];

                // Skip invalid channels
                if ( channel.sampler < 0 || ( u32 )channel.sampler >= animation.samplers.size ) {
                    continue;
                }

                AnimationSampler& sampler = animation.samplers[ channel.sampler ];

                // Get node name
                cstring node_name = "Unknown Node";
                if ( channel.target_node >= 0 && ( u32 )channel.target_node < scene_graph.nodes_debug_data.size ) {
                    node_name = scene_graph.nodes_debug_data[ channel.target_node ].name;
                }

                // Get target type name
                cstring target_type_name = "Unknown";
                switch ( channel.target_type ) {
                    case AnimationChannel::Translation: target_type_name = "T"; break;
                    case AnimationChannel::Rotation: target_type_name = "R"; break;
                    case AnimationChannel::Scale: target_type_name = "S"; break;
                }

                // Create unique ID for this channel's collapse state
                RASSERT( anim_idx < 256 );
                RASSERT( channel_idx < ( 1 << 24 ) );
                u32 channel_id = ( anim_idx << 24 ) | channel_idx;
                FlatHashMapIterator collapsed_it = channel_collapsed_state.find( channel_id );
                bool is_collapsed = true;
                if ( collapsed_it.is_valid() ) {
                    is_collapsed = channel_collapsed_state.get( collapsed_it );
                } else {
                    channel_collapsed_state.insert( channel_id, is_collapsed );
                }

                // Create lane with header on left and data on right
                ImGui::PushID( channel_idx );

                // Use columns for layout: header on left, graph on right
                ImGui::Columns( 2, nullptr, false );
                ImGui::SetColumnWidth( 0, 300.0f ); // Fixed width for header column

                // Left column: Collapsible header
                sprintf( channel_label, "%s %s - %s", is_collapsed ? "+" : "-", node_name, target_type_name );

                // Make the entire left area clickable to toggle collapse
                f32 lane_height = is_collapsed ? 20.0f : 200.0f;
                if ( ImGui::Selectable( channel_label, false, ImGuiSelectableFlags_AllowItemOverlap, ImVec2( 0, lane_height ) ) ) {
                    is_collapsed = !is_collapsed;
                    FlatHashMapIterator update_it = channel_collapsed_state.find( channel_id );
                    if ( update_it.is_valid() ) {
                        channel_collapsed_state.get( update_it ) = is_collapsed;
                    }
                }

                // Show additional info when expanded
                if ( !is_collapsed ) {
                    ImGui::Indent();
                    ImGui::Text( "Keyframes: %u", sampler.key_frames.size );
                    ImGui::Text( "Interpolation: %s", sampler.interpolation_type == AnimationSampler::Linear ? "Linear" :
                                 sampler.interpolation_type == AnimationSampler::Step ? "Step" : "CubicSpline" );
                    ImGui::Unindent();
                }

                // Right column: Show the graph only when not collapsed
                ImGui::NextColumn();

                // Get the draw area for the play head indicator
                ImVec2 lane_cursor_start_pos = ImGui::GetCursorScreenPos();
                f32 lane_content_width = ImGui::GetContentRegionAvail().x;

                // Calculate play head position based on animation time
                // Account for the min/max labels on both sides (50px each)
                constexpr f32 label_width = 50.0f;
                f32 plot_area_width = lane_content_width - ( label_width * 2.0f );
                f32 time_range = animation.time_end - animation.time_start;
                f32 normalized_time = ( animation_global_time - animation.time_start ) / time_range;
                f32 playhead_x_offset = label_width + ( normalized_time * plot_area_width );

                if ( !is_collapsed ) {
                    // Draw component plots
                    if ( sampler.key_frames.size > 0 && sampler.data ) {
                        // Determine component count based on target type
                        u32 component_count = 3; // Default for Translation and Scale
                        if ( channel.target_type == AnimationChannel::Rotation ) {
                            component_count = 4; // Quaternion has 4 components
                        }

                        static cstring component_names[] = { "X", "Y", "Z", "W" };
                        static ImVec4 component_colors[] = {
                            ImVec4( 1.0f, 0.3f, 0.3f, 1.0f ),  // Red for X
                            ImVec4( 0.3f, 1.0f, 0.3f, 1.0f ),  // Green for Y
                            ImVec4( 0.3f, 0.3f, 1.0f, 1.0f ),  // Blue for Z
                            ImVec4( 1.0f, 1.0f, 0.3f, 1.0f )   // Yellow for W
                        };

                        // Display each component as a line plot with its own min/max labels
                        for ( u32 comp = 0; comp < component_count; ++comp ) {
                            // Calculate value range for this component specifically
                            f32 comp_min_val = FLT_MAX;
                            f32 comp_max_val = -FLT_MAX;
                            for ( u32 i = 0; i < sampler.key_frames.size; ++i ) {
                                f32 val = sampler.data[ i ][ comp ];
                                comp_min_val = raptor::min( val, comp_min_val );
                                comp_max_val = raptor::max( val, comp_max_val );
                            }

                            // Add some padding to the range for display
                            f32 range = comp_max_val - comp_min_val;
                            if ( range < 0.001f ) range = 0.001f;
                            f32 display_min = comp_min_val - range * 0.1f;
                            f32 display_max = comp_max_val + range * 0.1f;

                            f32 plot_height = lane_height / component_count;

                            // Calculate available width for the plot area
                            f32 available_width = ImGui::GetContentRegionAvail().x;
                            f32 plot_width = available_width - ( label_width * 2.0f );

                            // Layout: (max/min labels) | graph | (max/min labels)
                            ImGui::BeginGroup();

                            // Left labels column: max at top, min at bottom
                            ImGui::BeginGroup();
                            ImGui::Text( "%.2f", comp_max_val );
                            ImGui::SetCursorPosY( ImGui::GetCursorPosY() + plot_height - ImGui::GetTextLineHeight() * 2 );
                            ImGui::Text( "%.2f", comp_min_val );
                            ImGui::EndGroup();

                            ImGui::SameLine();

                            // Middle: Graph
                            ImGui::BeginGroup();

                            // Use a simple lambda to extract component values for ImGui::PlotLines
                            auto value_getter = []( void* data, int idx ) -> float {
                                struct PlotData { glm::vec4* values; u32 component; };
                                PlotData* plot_data = ( PlotData* )data;
                                return plot_data->values[ idx ][ plot_data->component ];
                                };

                            struct PlotData { glm::vec4* values; u32 component; };
                            PlotData plot_data{ sampler.data, comp };

                            char label[ 64 ];
                            sprintf( label, "##%s_%u", component_names[ comp ], comp );

                            ImGui::PushStyleColor( ImGuiCol_PlotLines, component_colors[ comp ] );

                            // Draw plot with component name as overlay
                            ImGui::PlotLines( label, value_getter, &plot_data, ( int )sampler.key_frames.size,
                                              0, component_names[ comp ], display_min, display_max, ImVec2( plot_width, plot_height ) );

                            ImGui::PopStyleColor();

                            ImGui::EndGroup();

                            ImGui::SameLine();

                            // Right labels column: max at top, min at bottom
                            ImGui::BeginGroup();
                            ImGui::Text( "%.2f", comp_max_val );
                            ImGui::SetCursorPosY( ImGui::GetCursorPosY() + plot_height - ImGui::GetTextLineHeight() * 2 );
                            ImGui::Text( "%.2f", comp_min_val );
                            ImGui::EndGroup();

                            ImGui::EndGroup();
                        }
                    }
                } else {
                    // Collapsed: add dummy item to reserve space for the lane
                    ImGui::Dummy( ImVec2( lane_content_width, lane_height ) );
                }

                // Capture the cursor position after drawing content to get the actual end position
                ImVec2 lane_cursor_end_pos = ImGui::GetCursorScreenPos();

                // Draw play head indicator as a vertical line across the entire lane
                if ( normalized_time >= 0.0f && normalized_time <= 1.0f ) {
                    ImDrawList* draw_list = ImGui::GetWindowDrawList();
                    f32 playhead_x = lane_cursor_start_pos.x + playhead_x_offset;
                    f32 playhead_y_start = lane_cursor_start_pos.y;
                    f32 playhead_y_end = lane_cursor_end_pos.y;

                    // Draw a bright vertical line for the play head
                    draw_list->AddLine(
                        ImVec2( playhead_x, playhead_y_start ),
                        ImVec2( playhead_x, playhead_y_end ),
                        IM_COL32( 255, 255, 0, 255 ), // Bright yellow
                        2.0f // Line thickness
                    );
                }

                ImGui::Columns( 1 ); // Reset columns
                ImGui::Separator();
                ImGui::PopID();
            }

            ImGui::TreePop();
        }
    }

    if ( animations.size == 0 ) {
        ImGui::TextColored( ImVec4( 0.7f, 0.7f, 0.7f, 1.0f ), "No animations loaded" );
    }
}



} // namespace raptor
