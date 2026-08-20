#include "graphics/fbx_model.hpp"

#include "graphics/asynchronous_loader.hpp"
#include "graphics/scene_graph.hpp"

#include "foundation/time.hpp"
#include "foundation/numerics.hpp"

#include "external/ufbx/ufbx.h"
#include "external/stb_image.h"

namespace raptor {

struct MeshPartInfo {
    u32 start_index;
    u32 part_count;
};

void FbxModel::init( RenderScene* render_scene_, SceneGraph* scene_graph_, Allocator* resident_allocator_, Renderer* renderer_ ) {
    resident_allocator = resident_allocator_;
    renderer = renderer_;
    scene_graph = scene_graph_;
    render_scene = render_scene_;
}

void FbxModel::load_model( cstring filename, cstring path, ArenaAllocator* temp_allocator ) {
    sizet temp_allocator_initial_marker = temp_allocator->get_marker();

    i64 start_scene_loading = time_now();

    ufbx_error error;
    ufbx_load_opts opts = { };
    opts.target_unit_meters = 1.0f;
    opts.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    fbx_scene = ufbx_load_file( filename, &opts, &error );

    i64 end_loading_file = time_now();

    StringBuffer temp_name_buffer;
    temp_name_buffer.init( 4096, temp_allocator );

    for ( u32 texture_index = 0; texture_index < fbx_scene->textures.count; ++texture_index ) {
        ufbx_texture* texture = fbx_scene->textures.data[ texture_index ];

        // Reconstruct file path
        char* full_filename = temp_name_buffer.append_use_f( "%s%s", path, texture->filename.data );

        TextureResource* tr = renderer->create_texture_from_file( full_filename, true, false );
        RASSERT( tr != nullptr );

        render_scene->images.push( *tr );

        // Reset name buffer
        temp_name_buffer.clear();
    }

    i64 end_loading_textures = time_now();

    sizet temp_marker = temp_allocator->get_marker();
    u32 mesh_offset = render_scene->meshes.size;

    //
    Array<ufbx_node*> nodes_to_visit;
    nodes_to_visit.init( resident_allocator, 4 );
    ufbx_node* root_node = fbx_scene->root_node;
    RASSERT( root_node != nullptr );

    FlatHashMap<ufbx_node*, u32> node_to_index;
    node_to_index.init( resident_allocator, 64 );

    // Calculate total node count: add first the root nodes.
    u32 total_node_count = ( u32 )root_node->children.count;

    // Add initial nodes
    u32 node_index = 0;
    for ( ; node_index < root_node->children.count; node_index++ ) {
        ufbx_node* child = root_node->children[ node_index ];

        node_to_index.insert( child, node_index );
        nodes_to_visit.push( child );
    }

    // Visit nodes
    while ( nodes_to_visit.size ) {
        ufbx_node* node = nodes_to_visit.front();
        nodes_to_visit.delete_swap( 0 );

        for ( u32 ch = 0; ch < node->children.count; ++ch ) {
            ufbx_node* child = node->children[ ch ];

            node_to_index.insert( child, node_index );
            nodes_to_visit.push( child );

            node_index++;
        }

        total_node_count += ( u32 )node->children.count;
    }

    u32 node_offset = scene_graph->node_count();
    u32 new_node_count = node_offset + total_node_count;
    scene_graph->resize( new_node_count );
    scene_graph->init_new_nodes( node_offset, total_node_count );

    FlatHashMap<ufbx_mesh*, MeshPartInfo> mesh_to_index;
    mesh_to_index.init( resident_allocator, 64 );

    mesh_aabb[0] = glm::vec3{ FLT_MAX, FLT_MAX, FLT_MAX };
    mesh_aabb[1] = glm::vec3{ FLT_MIN, FLT_MIN, FLT_MIN };

    u32 total_mesh_parts = 0;
    for ( u32 mesh_index = 0; mesh_index < fbx_scene->meshes.count; ++mesh_index ) {
        ufbx_mesh* fbx_mesh = fbx_scene->meshes.data[ mesh_index ];

        for ( u32 mesh_part_idx = 0; mesh_part_idx < fbx_mesh->material_parts.count; ++mesh_part_idx ) {
            ufbx_mesh_part& mesh_part = fbx_mesh->material_parts.data[ mesh_part_idx ];

            Mesh mesh{ };
            mesh.pbr_material = {};

            // NOTE(marco): materials might be different per-instance, and we would need to move the material to the
            // MeshInstance struct. We won't do this for now.
            ufbx_material* material = fbx_mesh->materials.data[ mesh_part.index ];

            if ( material->pbr.base_color.texture != nullptr ) {
                mesh.pbr_material.diffuse_texture_index = material->pbr.base_color.texture->file_index;
            } else {
                mesh.pbr_material.diffuse_texture_index = k_invalid_scene_texture_index;
            }

            if ( material->shader_type == UFBX_SHADER_FBX_LAMBERT  ) {
                mesh.pbr_material.base_color_factor = glm::vec4( material->fbx.diffuse_color.value_vec3.x,
                                                                 material->fbx.diffuse_color.value_vec3.y,
                                                                 material->fbx.diffuse_color.value_vec3.z,
                                                                 1.0f );
            } else {
                mesh.pbr_material.base_color_factor = glm::vec4( material->pbr.base_factor.value_vec4.x,
                                                                 material->pbr.base_factor.value_vec4.y,
                                                                 material->pbr.base_factor.value_vec4.z,
                                                                 material->pbr.base_factor.value_vec4.w );
            }

            if ( material->pbr.normal_map.texture != nullptr ) {
                mesh.pbr_material.normal_texture_index = material->pbr.normal_map.texture->file_index;
            } else {
                mesh.pbr_material.normal_texture_index = k_invalid_scene_texture_index;
            }

            if ( material->pbr.metalness.texture != nullptr ) {
                mesh.pbr_material.metalness_texture_index = material->pbr.metalness.texture->file_index;
            } else {
                mesh.pbr_material.metalness_texture_index = k_invalid_scene_texture_index;
            }

            if ( material->pbr.roughness.texture != nullptr ) {
                mesh.pbr_material.roughness_texture_index = material->pbr.roughness.texture->file_index;
            } else {
                mesh.pbr_material.roughness_texture_index = k_invalid_scene_texture_index;
            }

            if ( material->pbr.metalness.has_value ) {
                mesh.pbr_material.metallic = material->pbr.metalness.value_real;
            }
            if ( material->pbr.roughness.has_value ) {
                mesh.pbr_material.roughness = material->pbr.roughness.value_real;
            }

            Array<u32> face_indices;
            face_indices.init( temp_allocator, 3 * ( u32 )fbx_mesh->max_face_triangles, 3 * ( u32 )fbx_mesh->max_face_triangles );

            Array<glm::vec3> vertices;
            vertices.init( temp_allocator, ( u32 )mesh_part.num_triangles );

            Array<glm::vec3> normals;
            normals.init( temp_allocator, ( u32 )mesh_part.num_triangles );

            Array<glm::vec3> tangents;
            tangents.init( temp_allocator, ( u32 )mesh_part.num_triangles );

            Array<glm::vec2> texcoords;
            texcoords.init( temp_allocator, ( u32 )mesh_part.num_triangles );

            Array<glm::u16vec4> joints;
            joints.init( temp_allocator, ( u32 )mesh_part.num_triangles );

            Array<glm::vec4> weights;
            weights.init( temp_allocator, ( u32 )mesh_part.num_triangles );

            bool has_skin = fbx_mesh->skin_deformers.count > 0;
            ufbx_skin_deformer* skin = nullptr;
            if ( has_skin ) skin = fbx_mesh->skin_deformers.data[ 0 ];

            // NOTE(marco): we don't support animated meshes that don't have a skeleton
            RASSERT( has_skin && skin->clusters.count > 0 );

            for ( u32 face = 0; face < fbx_mesh->faces.count; ++face ) {
                ufbx_face& fbx_face = fbx_mesh->faces.data[ face ];

                u32 num_triangles = ufbx_triangulate_face( face_indices.data, face_indices.size, fbx_mesh, fbx_face );
                for ( u32 t = 0; t < num_triangles * 3; ++t ) {
                    u32 vertex_index = face_indices[ t ];

                    ufbx_vec3 vertex = ufbx_get_vertex_vec3( &fbx_mesh->vertex_position, vertex_index );
                    vertices.push( glm::vec3( vertex.x, vertex.y, vertex.z ) );

                    if ( fbx_mesh->vertex_normal.exists ) {
                        ufbx_vec3 normal = ufbx_get_vertex_vec3( &fbx_mesh->vertex_normal, vertex_index );
                        normals.push( glm::vec3( normal.x, normal.y, normal.z ) );
                    }

                    if ( fbx_mesh->vertex_tangent.exists ) {
                        ufbx_vec3 tangent = ufbx_get_vertex_vec3( &fbx_mesh->vertex_tangent, vertex_index );
                        tangents.push( glm::vec3( tangent.x, tangent.y, tangent.z ) );
                    }

                    if ( fbx_mesh->vertex_uv.exists ) {
                        ufbx_vec2 uv = ufbx_get_vertex_vec2( &fbx_mesh->vertex_uv, vertex_index );
                        texcoords.push( glm::vec2( uv.x, uv.y ) );
                    }

                    if ( has_skin ) {
                        u32 real_vertex_index = fbx_mesh->vertex_position.indices[ vertex_index ];
                        ufbx_skin_vertex skin_vertex = skin->vertices.data[ real_vertex_index ];
                        i32 num_weights = ( i32 )skin_vertex.num_weights;
                        if (num_weights > 4) {
                            num_weights = 4;
                        }

                        float total_weight = 0.0f;
                        glm::u16vec4& joint_values = joints.push_use();
                        glm::vec4& weight_values = weights.push_use();
                        for ( i32 i = 0; i < num_weights; i++ ) {
                            ufbx_skin_weight skin_weight = skin->weights.data[ skin_vertex.weight_begin + i ];
                            RASSERT( skin_weight.cluster_index < 65536 );
                            joint_values[ i ] = ( u16 )skin_weight.cluster_index;
                            weight_values[ i ] = skin_weight.weight;
                            total_weight += skin_weight.weight;
                        }

                        // FBX does not guarantee that skin weights are normalized, and we may even
                        // be dropping some, so we must renormalize them.
                        for ( i32 i = 0; i < num_weights; i++ ) {
                            weight_values[ i ] /= total_weight;
                        }

                        for ( i32 i = num_weights; i < 4; i++ ) {
                            joint_values[ i ] = 0;
                            weight_values[ i ] = 0.0f;
                        }
                    }
                }
            }

            ufbx_vertex_stream streams[6] = {};

            u32 stream_count = 0;
            streams[ stream_count++ ] ={ .data = vertices.data, .vertex_count = vertices.size, .vertex_size = sizeof( glm::vec3 ) };
            if ( fbx_mesh->vertex_normal.exists ) {
                streams[ stream_count++ ] = { .data = normals.data, .vertex_count = normals.size, .vertex_size = sizeof( glm::vec3 ) };
            }
            if ( fbx_mesh->vertex_tangent.exists ) {
                streams[ stream_count++ ] = { .data = tangents.data, .vertex_count = tangents.size, .vertex_size = sizeof( glm::vec3 ) };
            }
            if ( fbx_mesh->vertex_uv.exists ) {
                streams[ stream_count++ ] = { .data = texcoords.data, .vertex_count = texcoords.size, .vertex_size = sizeof( glm::vec2 ) };
            }

            if ( has_skin ) {
                streams[ stream_count++ ] = { .data = joints.data, .vertex_count = joints.size, .vertex_size = sizeof( glm::u16vec4 ) };
                streams[ stream_count++ ] = { .data = weights.data, .vertex_count = weights.size, .vertex_size = sizeof( glm::vec4 ) };
            }

            Array<u32> indices;
            indices.init( temp_allocator, ( u32 )mesh_part.num_triangles * 3, ( u32 )mesh_part.num_triangles * 3 );

            u32 num_vertices = ( u32 )ufbx_generate_indices( streams, stream_count, indices.data, indices.size, nullptr, nullptr );
            mesh.position_count = num_vertices;
            mesh.position_offset = render_scene->write_vertices_to_staging_buffer( vertices.data, num_vertices );

            if ( renderer->gpu->mesh_shaders_extension_present ) {
                // Calculate bounding sphere center
                glm::vec3 position_min{ FLT_MAX };
                glm::vec3 position_max{ FLT_MIN };

                for ( u32 i = 0; i < num_vertices; ++i ) {
                    const glm::vec3& pos = vertices[ i ];

                    position_min = glm::min( position_min, pos );
                    position_max = glm::max( position_max, pos );
                }

                glm::vec3 bounding_center = position_min + position_max;
                bounding_center = bounding_center * 0.5f;

                // Calculate bounding sphere radius
                f32 radius = raptor::max( glm::distance( position_max, bounding_center ), glm::distance( position_min, bounding_center ) );
                mesh.bounding_sphere = { bounding_center.x, bounding_center.y, bounding_center.z, radius };

                mesh.aabb[ 0 ] = position_min;
                mesh.aabb[ 1 ] = position_max;
            }

            mesh.index_offset_bytes = render_scene->write_indices_to_staging_buffer( indices.data, indices.size );
            mesh.index_count = indices.size;
            mesh.index_type = VK_INDEX_TYPE_UINT32;

            if ( fbx_mesh->vertex_normal.exists ) {
                mesh.normal_offset = render_scene->write_normals_to_staging_buffer( normals.data, num_vertices );
                mesh.pbr_material.flags |= DrawFlags_HasNormals;
            }

            if ( fbx_mesh->vertex_tangent.exists ) {
                mesh.tangent_offset = render_scene->write_tangents_to_staging_buffer( tangents.data, num_vertices );
                mesh.pbr_material.flags |= DrawFlags_HasTangents;
            }

            if ( fbx_mesh->vertex_uv.exists ) {
                mesh.texcoord_offset = render_scene->write_texcoords_to_staging_buffer( texcoords.data, num_vertices );
                mesh.pbr_material.flags |= DrawFlags_HasTexCoords;
            }

            if ( has_skin ) {
                mesh.joints_offset = render_scene->write_joints_to_staging_buffer( joints.data, num_vertices );
                mesh.weights_offset = render_scene->write_weights_to_staging_buffer( weights.data, num_vertices );
                mesh.pbr_material.flags |= DrawFlags_HasJoints;
                mesh.pbr_material.flags |= DrawFlags_HasWeights;
            }

            mesh.gpu_mesh_index = render_scene->meshes.size;

            // TODO(marco): this needs to be fixed... we should build meshlet per mesh _instance_!
            build_meshlets( mesh, temp_allocator );

            render_scene->meshes.push( mesh );

            temp_allocator->free_marker( temp_marker );
        }

        MeshPartInfo info;
        info.start_index = total_mesh_parts;
        info.part_count = ( u32 )fbx_mesh->material_parts.count;
        mesh_to_index.insert( fbx_mesh, info );

        total_mesh_parts += info.part_count;
    }

    i64 end_loading_geometry = time_now();

    // Load skins
    FlatHashMap<ufbx_skin_deformer*, u32> skin_to_index;
    skin_to_index.init( temp_allocator, 64 );

    u32 skin_offset = render_scene->skins.size;
    for ( u32 si = 0; si < fbx_scene->skin_deformers.count; ++si ) {
        ufbx_skin_deformer* fbx_skin = fbx_scene->skin_deformers.data[ si ];

        skin_to_index.insert( fbx_skin, skin_offset + si );

        Skin& skin = render_scene->skins.push_use();

        const u32 joints_count = ( u32 )fbx_skin->clusters.count;

        if ( joints_count == 0 ) {
            continue;
        }

        // Copy joints by reading node indices
        skin.joints.init( resident_allocator, joints_count, joints_count );

        const u32 joint_buffer_size = sizeof( glm::mat4 ) * joints_count;
        skin.inverse_bind_matrices.init( resident_allocator, joints_count, joints_count );

        for ( u32 j = 0; j < joints_count; ++j ) {
            ufbx_skin_cluster* joint_node = fbx_skin->clusters.data[ j ];

            u32 node_index = node_to_index.get_structure( joint_node->bone_node ).value + node_offset;
            RASSERT( node_index < scene_graph->node_count() );
            scene_graph->set_node_as_bone( node_index );
            skin.joints[ j ] = ( u32 )node_index;

            ufbx_matrix& joint_transform = joint_node->geometry_to_bone;
            skin.inverse_bind_matrices[ j ] = glm::mat4( joint_transform.m00, joint_transform.m10, joint_transform.m20, 0.0f,
                                                         joint_transform.m01, joint_transform.m11, joint_transform.m21, 0.0f,
                                                         joint_transform.m02, joint_transform.m12, joint_transform.m22, 0.0f,
                                                         joint_transform.m03, joint_transform.m13, joint_transform.m23, 1.0f );
        }
    }

    // Populate scene graph: visit again
    nodes_to_visit.clear();
    node_index = 0;
    for ( ; node_index < root_node->children.count; node_index++ ) {
        ufbx_node* child = root_node->children[ node_index ];

        nodes_to_visit.push( child );
    }

    while ( nodes_to_visit.size ) {
        ufbx_node* fbx_node = nodes_to_visit.front();
        u32 scene_graph_node_index = node_to_index.get_structure( fbx_node ).value + node_offset;
        nodes_to_visit.delete_swap( 0 );

        glm::vec3 node_translation = { fbx_node->local_transform.translation.x,
                                       fbx_node->local_transform.translation.y,
                                       fbx_node->local_transform.translation.z };

        glm::vec3 node_scale = { fbx_node->local_transform.scale.x,
                                 fbx_node->local_transform.scale.y,
                                 fbx_node->local_transform.scale.z };

        glm::quat node_rotation = { fbx_node->local_transform.rotation.w,
                                    fbx_node->local_transform.rotation.x,
                                    fbx_node->local_transform.rotation.y,
                                    fbx_node->local_transform.rotation.z };

        Transform transform;
        transform.translation = node_translation;
        transform.scale = node_scale;
        transform.rotation = node_rotation;

        // Final SRT composition
        const glm::mat4 local_matrix = transform.calculate_matrix();
        scene_graph->set_local_matrix( scene_graph_node_index, local_matrix );

        // Handle parent-relationship
        if ( fbx_node->children.count ) {
            Hierarchy& node_hierarchy = scene_graph->nodes_hierarchy[ scene_graph_node_index ];
            node_hierarchy.children.init( resident_allocator, ( u32 )fbx_node->children.count );

            for ( u32 ch = 0; ch < fbx_node->children.count; ++ch) {
                ufbx_node* child = fbx_node->children[ ch ];
                const i32 children_index = node_to_index.get_structure( child ).value;
                i32 global_child_index = children_index + node_offset;
                Hierarchy& children_hierarchy = scene_graph->nodes_hierarchy[ global_child_index ];
                scene_graph->set_hierarchy( global_child_index, scene_graph_node_index, node_hierarchy.level + 1 );

                nodes_to_visit.push( child );
            }
        }

        // Cache node name
        scene_graph->set_debug_data( scene_graph_node_index, fbx_node->name.data );

        if ( fbx_node->mesh == nullptr ) {
            continue;
        }

        ufbx_mesh* fbx_mesh = fbx_node->mesh;
        MeshPartInfo mesh_info = mesh_to_index.get_structure( fbx_mesh ).value;

        u32 skin_index_offset = render_scene->skins.size;

        // Create one MeshInstance per mesh part (similar to glTF primitives)
        for ( u32 mesh_part_idx = 0; mesh_part_idx < mesh_info.part_count; ++mesh_part_idx ) {
            u32 mesh_index = mesh_offset + mesh_info.start_index + mesh_part_idx;

            MeshInstance mesh_instance{ };
            // Assign scene graph node index
            mesh_instance.scene_graph_node_index = scene_graph_node_index;

            // Cache parent mesh and assign material
            mesh_instance.mesh_index = mesh_index;

            Mesh& mesh = render_scene->meshes[ mesh_index ];
            // Cache gpu mesh instance index, used to retrieve data on gpu.
            mesh_instance.gpu_mesh_instance_index = render_scene->mesh_instances.size;

            // Found a skin index, cache it
            mesh.skin_index = i32_max;
            if ( fbx_mesh->skin_deformers.count ) {
                // TODO(marco): poses are (?) skins equivalent in gltf
                RASSERT( fbx_mesh->skin_deformers.count == 1 ); // For now we only support one skin per mesh
                mesh.skin_index = ( i32 )skin_to_index.get_structure( fbx_mesh->skin_deformers.data[ 0 ] ).value;
            }

            render_scene->mesh_instances.push( mesh_instance );
        }
    }

    nodes_to_visit.shutdown();
    node_to_index.shutdown();
    mesh_to_index.shutdown();

    temp_allocator->free_marker( temp_marker );

    ufbx_anim* fbx_animation = fbx_scene->anim;
    if ( fbx_animation->layers.count == 1 ) {
        load_animation( fbx_scene, fbx_animation );
    }
}

void FbxModel::shutdown( Renderer* renderer ) {

    ufbx_free_scene( fbx_scene );
}

void FbxModel::add_animation( cstring filename, cstring path, ArenaAllocator* temp_allocator ) {
    ufbx_error error;
    ufbx_load_opts opts = { };
    opts.target_unit_meters = 1.0f;
    opts.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    ufbx_scene* anim_scene = ufbx_load_file( filename, &opts, &error );

    ufbx_anim* fbx_animation = anim_scene->anim;
    if ( fbx_animation->layers.count == 1 ) {
        load_animation( anim_scene, fbx_animation );
    }
    ufbx_free_scene( anim_scene );
}

void FbxModel::load_animation( ufbx_scene* fbx_scene, ufbx_anim* fbx_animation ) {
    u32 animation_offset = render_scene->animations.size;

    ufbx_error error;
    ufbx_baked_anim* baked_animation = ufbx_bake_anim( fbx_scene, fbx_animation, nullptr, &error );

    if ( baked_animation->nodes.count == 0 ) {
        return;
    }

    Animation& animation = render_scene->animations.push_use();
    animation.time_start = ( f32 )fbx_animation->time_begin;
    animation.time_end = ( f32 )fbx_animation->time_end;

    // RASSERT( fbx_animation_layer->anim_values.count == fbx_animation_layer->anim_props.count );
    u32 channels_count = ( u32 )baked_animation->nodes.count;

    animation.channels.init( resident_allocator, channels_count * 3 );
    animation.samplers.init( resident_allocator, channels_count * 3 );

    u32 max_node_index = 0;

    for ( u32 channel_index = 0; channel_index < channels_count; ++channel_index ) {
        ufbx_baked_node* bake_node = &baked_animation->nodes.data[ channel_index ];
        ufbx_node* node = fbx_scene->nodes.data[ bake_node->typed_id ];

        u32 node_index = scene_graph->get_node_index( node->name.data );
        max_node_index = raptor::max( max_node_index, node_index );

        RASSERT( node != nullptr );
        RASSERT( node->name.data != nullptr );

        {
            AnimationChannel& channel = animation.channels.push_use();
            channel.sampler = channel_index * 3;
            channel.target_type = AnimationChannel::TargetType::Translation;
            channel.target_node = node_index;

            AnimationSampler& sampler = animation.samplers.push_use();
            sampler.interpolation_type = AnimationSampler::Interpolation::Linear;

            u32 keyframes_count = ( u32 )bake_node->translation_keys.count;
            sampler.key_frames.init( resident_allocator, keyframes_count );
            sampler.data = ( glm::vec4* )rallocaa( sizeof( glm::vec4 ) * keyframes_count, resident_allocator, 16 );

            for ( u32 k = 0; k < keyframes_count; ++k ) {
                f32 time = ( f32 )bake_node->translation_keys.data[ k ].time;
                sampler.key_frames.push( time );

                f32 value_x = bake_node->translation_keys.data[ k ].value.x;
                f32 value_y = bake_node->translation_keys.data[ k ].value.y;
                f32 value_z = bake_node->translation_keys.data[ k ].value.z;

                sampler.data[ k ] = glm::vec4( value_x, value_y, value_z, 0.f );
            }
        }

        {
            AnimationChannel& channel = animation.channels.push_use();
            channel.sampler = channel_index * 3 + 1;
            channel.target_type = AnimationChannel::TargetType::Rotation;
            channel.target_node = node_index;

            AnimationSampler& sampler = animation.samplers.push_use();
            sampler.interpolation_type = AnimationSampler::Interpolation::Linear;

            u32 keyframes_count = ( u32 )bake_node->rotation_keys.count;
            sampler.key_frames.init( resident_allocator, keyframes_count );
            sampler.data = ( glm::vec4* )rallocaa( sizeof( glm::vec4 ) * keyframes_count, resident_allocator, 16 );

            for ( u32 k = 0; k < keyframes_count; ++k ) {
                f32 time = ( f32 )bake_node->rotation_keys.data[ k ].time;
                sampler.key_frames.push( time );

                f32 value_x = bake_node->rotation_keys.data[ k ].value.x;
                f32 value_y = bake_node->rotation_keys.data[ k ].value.y;
                f32 value_z = bake_node->rotation_keys.data[ k ].value.z;
                f32 value_w = bake_node->rotation_keys.data[ k ].value.w;

                sampler.data[ k ] = glm::vec4( value_x, value_y, value_z, value_w );
            }
        }

        {
            AnimationChannel& channel = animation.channels.push_use();
            channel.sampler = channel_index * 3 + 2;
            channel.target_type = AnimationChannel::TargetType::Scale;
            channel.target_node = node_index;

            AnimationSampler& sampler = animation.samplers.push_use();
            sampler.interpolation_type = AnimationSampler::Interpolation::Linear;

            u32 keyframes_count = ( u32 )bake_node->scale_keys.count;
            sampler.key_frames.init( resident_allocator, keyframes_count );
            sampler.data = ( glm::vec4* )rallocaa( sizeof( glm::vec4 ) * keyframes_count, resident_allocator, 16 );

            for ( u32 k = 0; k < keyframes_count; ++k ) {
                f32 time = ( f32 )bake_node->scale_keys.data[ k ].time;
                sampler.key_frames.push( time );

                f32 value_x = bake_node->scale_keys.data[ k ].value.x;
                f32 value_y = bake_node->scale_keys.data[ k ].value.y;
                f32 value_z = bake_node->scale_keys.data[ k ].value.z;

                sampler.data[ k ] = glm::vec4( value_x, value_y, value_z, 0.0f );
            }
        }
    }

    ufbx_free_baked_anim( baked_animation );

    animation.animated_transforms.init( resident_allocator, max_node_index + 1, max_node_index + 1 );
    for ( Transform& animated_transform : animation.animated_transforms ) {
        animated_transform.reset();
    }
}

} // namespace raptor
