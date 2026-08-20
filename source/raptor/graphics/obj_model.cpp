#include "graphics/obj_model.hpp"
#include "graphics/gpu_profiler.hpp"
#include "graphics/raptor_imgui.hpp"
#include "graphics/scene_graph.hpp"
#include "graphics/asynchronous_loader.hpp"

#include "foundation/file.hpp"
#include "foundation/time.hpp"
#include "foundation/numerics.hpp"

#include "external/stb_image.h"

#include "external/glm/matrix.hpp"
#include "external/glm/mat4x4.hpp"
#include "external/glm/vec3.hpp"
#include "external/tracy/tracy/Tracy.hpp"
#include "external/meshoptimizer/meshoptimizer.h"

#include <assimp/cimport.h>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

static const bool k_enable_physics = false;

namespace raptor {

static bool is_shared_vertex( PhysicsVertex* vertices, PhysicsVertex& src, u32 dst ) {
    u32 shared_count = 0;

    f32 max_distance = 0.0f;
    f32 min_distance = 10000.0f;

    for ( u32 j = 0; j < src.joint_count; ++j ) {
        PhysicsVertex& joint_vertex = vertices[ src.joints[ j ].vertex_index ];
        f32 distance = glm::distance( src.start_position, joint_vertex.start_position );

        max_distance = ( distance > max_distance ) ? distance : max_distance;
        min_distance = ( distance < min_distance ) ? distance : min_distance;
    }

    // NOTE(marco): this is to add joints with the next-next vertex either in horizontal
    // or vertical direction.
    min_distance *= 2;
    max_distance = ( min_distance > max_distance ) ? min_distance : max_distance;

    PhysicsVertex& dst_vertex = vertices[ dst ];
    f32 distance = glm::distance( src.start_position, dst_vertex.start_position );

    // NOTE(marco): this only works if we work with a plane with equal size subdivision
    return ( distance <= max_distance );
}

static void compute_joints( aiMesh* mesh, PhysicsMesh* physics_mesh ) {
    // NOTE(marco): compute cloth joints
    for ( u32 face_index = 0; face_index < mesh->mNumFaces; ++face_index ) {
        u32 index_a = mesh->mFaces[ face_index ].mIndices[ 0 ];
        u32 index_b = mesh->mFaces[ face_index ].mIndices[ 1 ];
        u32 index_c = mesh->mFaces[ face_index ].mIndices[ 2 ];

        PhysicsVertex& vertex_a = physics_mesh->vertices[ index_a ];
        vertex_a.add_joint( index_b );
        vertex_a.add_joint( index_c );

        PhysicsVertex& vertex_b = physics_mesh->vertices[ index_b ];
        vertex_b.add_joint( index_a );
        vertex_b.add_joint( index_c );

        PhysicsVertex& vertex_c = physics_mesh->vertices[ index_c ];
        vertex_c.add_joint( index_a );
        vertex_c.add_joint( index_b );

        // NOTE(marco): check for adjacent triangles to get diagonal joints
        for ( u32 other_face_index = 0; other_face_index < mesh->mNumFaces; ++other_face_index ) {
            if ( other_face_index == face_index ) {
                continue;
            }

            u32 other_index_a = mesh->mFaces[ other_face_index ].mIndices[ 0 ];
            u32 other_index_b = mesh->mFaces[ other_face_index ].mIndices[ 1 ];
            u32 other_index_c = mesh->mFaces[ other_face_index ].mIndices[ 2 ];

            // check for vertex_a
            if ( other_index_a == index_b && other_index_b == index_c ) {
                if ( is_shared_vertex( physics_mesh->vertices.data, vertex_a, other_index_c ) ) {
                    vertex_a.add_joint( other_index_c );
                }
            }
            if ( other_index_a == index_c && other_index_b == index_b ) {
                if ( is_shared_vertex( physics_mesh->vertices.data, vertex_a, other_index_c ) ) {
                    vertex_a.add_joint( other_index_c );
                }
            }
            if ( other_index_a == index_b && other_index_c == index_c ) {
                if ( is_shared_vertex( physics_mesh->vertices.data, vertex_a, other_index_b ) ) {
                    vertex_a.add_joint( other_index_b );
                }
            }
            if ( other_index_a == index_c && other_index_c == index_b ) {
                if ( is_shared_vertex( physics_mesh->vertices.data, vertex_a, other_index_b ) ) {
                    vertex_a.add_joint( other_index_b );
                }
            }
            if ( other_index_c == index_b && other_index_b == index_c ) {
                if ( is_shared_vertex( physics_mesh->vertices.data, vertex_a, other_index_a ) ) {
                    vertex_a.add_joint( other_index_a );
                }
            }
            if ( other_index_c == index_c && other_index_b == index_b ) {
                if ( is_shared_vertex( physics_mesh->vertices.data, vertex_a, other_index_a ) ) {
                    vertex_a.add_joint( other_index_a );
                }
            }

            // check for vertex_b
            if ( other_index_a == index_a && other_index_b == index_c ) {
                if ( is_shared_vertex( physics_mesh->vertices.data, vertex_b, other_index_c ) ) {
                    vertex_b.add_joint( other_index_c );
                }
            }
            if ( other_index_a == index_c && other_index_b == index_a ) {
                if ( is_shared_vertex( physics_mesh->vertices.data, vertex_b, other_index_c ) ) {
                    vertex_b.add_joint( other_index_c );
                }
            }
            if ( other_index_a == index_a && other_index_c == index_c ) {
                if ( is_shared_vertex( physics_mesh->vertices.data, vertex_b, other_index_b ) ) {
                    vertex_b.add_joint( other_index_b );
                }
            }
            if ( other_index_a == index_c && other_index_c == index_a ) {
                if ( is_shared_vertex( physics_mesh->vertices.data, vertex_b, other_index_b ) ) {
                    vertex_b.add_joint( other_index_b );
                }
            }
            if ( other_index_c == index_a && other_index_b == index_c ) {
                if ( is_shared_vertex( physics_mesh->vertices.data, vertex_b, other_index_a ) ) {
                    vertex_b.add_joint( other_index_a );
                }
            }
            if ( other_index_c == index_c && other_index_b == index_a ) {
                if ( is_shared_vertex( physics_mesh->vertices.data, vertex_b, other_index_a) ) {
                    vertex_b.add_joint( other_index_a );
                }
            }

            // check for vertex_c
            if ( other_index_a == index_a && other_index_b == index_b ) {
                if ( is_shared_vertex( physics_mesh->vertices.data, vertex_c, other_index_c ) ) {
                    vertex_c.add_joint( other_index_c );
                }
            }
            if ( other_index_a == index_b && other_index_b == index_a ) {
                if ( is_shared_vertex( physics_mesh->vertices.data, vertex_c, other_index_c ) ) {
                    vertex_c.add_joint( other_index_c );
                }
            }
            if ( other_index_a == index_a && other_index_c == index_b ) {
                if ( is_shared_vertex( physics_mesh->vertices.data, vertex_c, other_index_b ) ) {
                    vertex_c.add_joint( other_index_b );
                }
            }
            if ( other_index_a == index_b && other_index_c == index_a ) {
                if ( is_shared_vertex( physics_mesh->vertices.data, vertex_c, other_index_b ) ) {
                    vertex_c.add_joint( other_index_b );
                }
            }

            if ( other_index_c == index_a && other_index_b == index_b ) {
                if ( is_shared_vertex( physics_mesh->vertices.data, vertex_c, other_index_a ) ) {
                    vertex_c.add_joint( other_index_a );
                }
            }
            if ( other_index_c == index_b && other_index_b == index_a ) {
                if ( is_shared_vertex( physics_mesh->vertices.data, vertex_c, other_index_a ) ) {
                    vertex_c.add_joint( other_index_a );
                }
            }
        }
    }
}

void ObjModel::init( RenderScene* render_scene_, SceneGraph* scene_graph_, Allocator* resident_allocator_, Renderer* renderer_ ) {
    resident_allocator = resident_allocator_;
    renderer = renderer_;
    scene_graph = scene_graph_;
    render_scene = render_scene_;

    texture_map.init( resident_allocator, 127 );
    buffers.init( resident_allocator, 16 );
}

void ObjModel::load_model( cstring filename, cstring path, ArenaAllocator* temp_allocator ) {
    sizet temp_allocator_initial_marker = temp_allocator->get_marker();

    // Time statistics
    i64 start_scene_loading = time_now();

    assimp_scene = aiImportFile( filename,
        aiProcess_CalcTangentSpace       |
        aiProcess_GenNormals             |
        aiProcess_Triangulate            |
        aiProcess_JoinIdenticalVertices  |
        aiProcess_GlobalScale            |
        aiProcess_SortByPType);

    i64 end_loading_file = time_now();

    // If the import failed, report it
    if( assimp_scene == nullptr ) {
        RASSERT(false);
        return;
    }

    SamplerCreation sampler_creation{ };
    sampler_creation.set_address_mode_uv( VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT ).set_min_mag_mip( VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR);
    sampler = renderer->create_sampler( sampler_creation );

    render_scene->samplers.push( *sampler );

    Array<PBRMaterial> materials;
    materials.init( resident_allocator, assimp_scene->mNumMaterials );

    for ( u32 material_index = 0; material_index < assimp_scene->mNumMaterials; ++material_index ) {
        aiMaterial* material = assimp_scene->mMaterials[ material_index ];

        // Important to init with default values.
        PBRMaterial raptor_material{ };

        aiString texture_file;

        // TODO(marco): check AI_MATKEY_SHADING_MODEL to understand which shading model is being used

        if ( aiGetMaterialString( material, AI_MATKEY_TEXTURE( aiTextureType_DIFFUSE, 0 ), &texture_file ) == AI_SUCCESS ) {
            raptor_material.diffuse_texture_index = load_texture( texture_file.C_Str(), path, temp_allocator );
        } else {
            raptor_material.diffuse_texture_index = k_invalid_scene_texture_index;
        }

        if ( aiGetMaterialString( material, AI_MATKEY_TEXTURE( aiTextureType_NORMALS, 0 ), &texture_file ) == AI_SUCCESS )
        {
            raptor_material.normal_texture_index = load_texture( texture_file.C_Str(), path, temp_allocator );
        } else {
            raptor_material.normal_texture_index = k_invalid_scene_texture_index;
        }

        if ( aiGetMaterialString( material, AI_MATKEY_TEXTURE( aiTextureType_METALNESS, 0 ), &texture_file ) == AI_SUCCESS )
        {
            raptor_material.metalness_texture_index = load_texture( texture_file.C_Str(), path, temp_allocator );
        } else {
            raptor_material.metalness_texture_index = k_invalid_scene_texture_index;
        }

        if ( aiGetMaterialString( material, AI_MATKEY_TEXTURE( aiTextureType_DIFFUSE_ROUGHNESS, 0 ), &texture_file ) == AI_SUCCESS ) {
            raptor_material.roughness_texture_index = load_texture( texture_file.C_Str(), path, temp_allocator );
        } else {
            raptor_material.roughness_texture_index = k_invalid_scene_texture_index;
        }

        if ( aiGetMaterialString( material, AI_MATKEY_TEXTURE( aiTextureType_AMBIENT_OCCLUSION, 0 ), &texture_file ) == AI_SUCCESS )
        {
            raptor_material.occlusion_texture_index = load_texture( texture_file.C_Str(), path, temp_allocator );
        } else {
            raptor_material.occlusion_texture_index = k_invalid_scene_texture_index;
        }

        if ( aiGetMaterialString( material, AI_MATKEY_TEXTURE( aiTextureType_OPACITY, 0 ), &texture_file ) == AI_SUCCESS )
        {
            raptor_material.alpha_texture_index = load_texture( texture_file.C_Str(), path, temp_allocator );
        } else {
            raptor_material.alpha_texture_index = k_invalid_scene_texture_index;
        }

        aiColor4D color;
        if ( aiGetMaterialColor( material, AI_MATKEY_COLOR_DIFFUSE, &color ) == AI_SUCCESS ) {
            raptor_material.base_color_factor = { color.r, color.g, color.b, 1.0f };
        }
        else {
            raptor_material.base_color_factor = { 1.0f, 1.0f, 1.0f, 1.0f };
        }

        float f_value;
        if ( aiGetMaterialFloat( material, AI_MATKEY_SHININESS, &f_value ) == AI_SUCCESS ) {
            const f32 specular_exp = f_value;

            raptor_material.occlusion = raptor::max( powf( ( 1.f - specular_exp ), 2.f ), 0.0001f );
        }

        if ( aiGetMaterialFloat( material, AI_MATKEY_METALLIC_FACTOR, &f_value ) == AI_SUCCESS ) {
            raptor_material.metallic = f_value;
        } else {
            raptor_material.metallic = 1.0f;
        }

        if ( aiGetMaterialFloat( material, AI_MATKEY_ROUGHNESS_FACTOR, &f_value ) == AI_SUCCESS ) {
            raptor_material.roughness = f_value;
        } else {
            raptor_material.roughness = 1.0f;
        }

        if ( aiGetMaterialFloat( material, AI_MATKEY_OPACITY, &f_value ) == AI_SUCCESS ) {
            raptor_material.base_color_factor.w = f_value;
        }

        materials.push( raptor_material );
    }

    i64 end_loading_textures_files = time_now();

    i64 end_creating_textures = time_now();

    Array<u32> indices;
    indices.init( resident_allocator, rkilo( 64 ) );

    FlatHashMap<u32, u32> aimesh_to_mesh;
    aimesh_to_mesh.init( resident_allocator, assimp_scene->mNumMeshes );

    // Build meshlets
    const sizet max_vertices = 64;
    const sizet max_triangles = 124;
    const f32 cone_weight = 0.0f;

    sizet temp_marker = temp_allocator->get_marker();

    u32 mesh_offset = render_scene->meshes.size;
    u32 mesh_instances_offset = render_scene->mesh_instances.size;

    for ( u32 mesh_index = 0; mesh_index < assimp_scene->mNumMeshes; ++mesh_index ) {
        mesh_aabb[0] = glm::vec3{ FLT_MAX, FLT_MAX, FLT_MAX };
        mesh_aabb[1] = glm::vec3{ FLT_MIN, FLT_MIN, FLT_MIN };

        aiMesh* mesh = assimp_scene->mMeshes[ mesh_index ];

        Mesh render_mesh{ };

        PhysicsMesh* physics_mesh = nullptr;

        if ( k_enable_physics ) {
            physics_mesh = ( PhysicsMesh* )resident_allocator->allocate( sizeof( PhysicsMesh ), 64 );

            physics_mesh->vertices.init( resident_allocator, mesh->mNumVertices );
        }

        if ( ( mesh->mPrimitiveTypes & aiPrimitiveType_TRIANGLE ) == 0 ) {
            continue;
        }

        aimesh_to_mesh.insert( mesh_index, render_scene->meshes.size );

        u32 meshlet_vertex_offset = render_scene->meshlets_vertex_positions.size;
        for ( u32 vertex_index = 0; vertex_index < mesh->mNumVertices; ++vertex_index ) {
            glm::vec3 position{
                mesh->mVertices[ vertex_index ].x,
                mesh->mVertices[ vertex_index ].y,
                mesh->mVertices[ vertex_index ].z
            };

            glm::vec3 normal = glm::vec3{
                mesh->mNormals[ vertex_index ].x,
                mesh->mNormals[ vertex_index ].y,
                mesh->mNormals[ vertex_index ].z
            };

            if ( k_enable_physics ) {
                PhysicsVertex physics_vertex{ };
                physics_vertex.start_position = position;
                physics_vertex.previous_position = position;
                physics_vertex.position = position;
                physics_vertex.mass = 1.0f;
                physics_vertex.fixed = false;
                physics_vertex.normal = normal;

                physics_mesh->vertices.push( physics_vertex );
            }
        }

        glm::vec3 position_min{ mesh_aabb[ 0 ].x, mesh_aabb[ 0 ].y, mesh_aabb[ 0 ].z };
        glm::vec3 position_max{ mesh_aabb[ 1 ].x, mesh_aabb[ 1 ].y, mesh_aabb[ 1 ].z };
        glm::vec3 bounding_center = position_min + position_max;
        bounding_center = bounding_center * 0.5f;

        f32 radius = raptor::max( glm::distance( position_max, bounding_center ), glm::distance( position_min, bounding_center ) );
        render_mesh.bounding_sphere = { bounding_center.x, bounding_center.y, bounding_center.z, radius };

        u32 indices_count = mesh->mNumFaces * 3;
        indices.clear();
        if ( indices_count > indices.capacity ) {
            indices.grow( indices_count );
        }

        for ( u32 face_index = 0; face_index < mesh->mNumFaces; ++face_index ) {
            RASSERT( mesh->mFaces[ face_index ].mNumIndices == 3 );

            u32 index_a = mesh->mFaces[ face_index ].mIndices[ 0 ];
            u32 index_b = mesh->mFaces[ face_index ].mIndices[ 1 ];
            u32 index_c = mesh->mFaces[ face_index ].mIndices[ 2 ];

            indices.push( index_a );
            indices.push( index_b );
            indices.push( index_c );
        }

        if ( k_enable_physics ) {
            compute_joints( mesh, physics_mesh );
        }

        static_assert( sizeof( glm::vec3 ) == sizeof( aiVector3D ) );
        static_assert( sizeof( glm::vec2 ) == sizeof( aiVector2D ) );

        render_mesh.position_offset = render_scene->write_vertices_to_staging_buffer( ( glm::vec3* )mesh->mVertices, mesh->mNumVertices );

        render_mesh.tangent_offset = render_scene->write_tangents_to_staging_buffer( ( glm::vec3* )mesh->mTangents, mesh->mNumVertices );

        render_mesh.normal_offset = render_scene->write_normals_to_staging_buffer( ( glm::vec3* )mesh->mNormals, mesh->mNumVertices );

        render_mesh.texcoord_offset = render_scene->write_texcoords_to_staging_buffer( ( glm::vec3* )mesh->mTextureCoords[ 0 ], mesh->mNumVertices, true );

        render_mesh.index_offset_bytes = render_scene->write_indices_to_staging_buffer( indices.data, indices.size );
        render_mesh.index_type = VK_INDEX_TYPE_UINT32;

        render_mesh.index_count = indices.size;

        render_mesh.physics_mesh = physics_mesh;

        render_mesh.pbr_material = materials[ mesh->mMaterialIndex ];
        render_mesh.pbr_material.flags = DrawFlags_HasNormals;
        render_mesh.pbr_material.flags |= DrawFlags_HasTangents;
        render_mesh.pbr_material.flags |= DrawFlags_HasTexCoords;

        render_mesh.gpu_mesh_index = render_scene->meshes.size;

        build_meshlets( render_mesh, temp_allocator );

        temp_allocator->free_marker( temp_marker );

        // Physics data
        // Physics data.
        if ( k_enable_physics ) {
            GpuDevice& gpu = *renderer->gpu;

            const VkDeviceSize physics_buffer_size = sizeof( PhysicsMeshGpuData ) + VkDeviceSize( mesh->mNumVertices ) * sizeof( PhysicsVertexGpuData );

            // CPU staging buffer.
            BufferHandle physics_cpu_buffer = gpu.create_buffer( {
                .size = physics_buffer_size,
                .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                    VMA_ALLOCATION_CREATE_MAPPED_BIT,
                .name = "physics_mesh_data_cpu" } );

            Buffer* mapped_physics_buffer = gpu.get_buffer( physics_cpu_buffer );
            RASSERT( mapped_physics_buffer );
            RASSERT( mapped_physics_buffer->mapped_data );

            u8* mapped_data = ( u8* )mapped_physics_buffer->mapped_data;

            PhysicsMeshGpuData* mesh_data = ( PhysicsMeshGpuData* )mapped_data;
            mesh_data->index_count = render_mesh.index_count;
            mesh_data->vertex_count = mesh->mNumVertices;

            PhysicsVertexGpuData* vertex_data = ( PhysicsVertexGpuData* )( mapped_data + sizeof( PhysicsMeshGpuData ) );

            Array<VkDrawIndirectCommand> indirect_commands;
            indirect_commands.init( resident_allocator, physics_mesh->vertices.size, physics_mesh->vertices.size );

            for ( u32 vertex_index = 0; vertex_index < physics_mesh->vertices.size; ++vertex_index ) {
                const PhysicsVertex& cpu_data = physics_mesh->vertices[ vertex_index ];

                PhysicsVertexGpuData& gpu_data = vertex_data[ vertex_index ];
                gpu_data = {};
                gpu_data.position = cpu_data.position;
                gpu_data.start_position = cpu_data.start_position;
                gpu_data.previous_position = cpu_data.previous_position;
                gpu_data.normal = cpu_data.normal;
                gpu_data.joint_count = cpu_data.joint_count;
                gpu_data.velocity = cpu_data.velocity;
                gpu_data.mass = cpu_data.mass;
                gpu_data.force = cpu_data.force;

                for ( u32 joint_index = 0; joint_index < cpu_data.joint_count; ++joint_index ) {
                    gpu_data.joints[ joint_index ] = cpu_data.joints[ joint_index ].vertex_index;
                }

                VkDrawIndirectCommand& indirect_command = indirect_commands[ vertex_index ];
                indirect_command = {};
                indirect_command.vertexCount = 2;
                indirect_command.instanceCount = cpu_data.joint_count;
            }

            gpu.flush_buffer( physics_cpu_buffer, 0, physics_buffer_size );

            // Device-local physics buffer.
            BufferHandle physics_gpu_buffer = gpu.create_buffer( {
                .size = physics_buffer_size,
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                .name = "physics_mesh_data_gpu" } );

            physics_mesh->gpu_buffer = physics_gpu_buffer;

            buffers.push( physics_cpu_buffer );
            buffers.push( physics_gpu_buffer );

            renderer->async_loader->request_buffer_copy( physics_cpu_buffer, physics_gpu_buffer );

            // Indirect commands.
            const VkDeviceSize indirect_buffer_size =
                VkDeviceSize( indirect_commands.size ) * sizeof( VkDrawIndirectCommand );

            BufferHandle indirect_cpu_buffer = gpu.create_buffer( {
                .size = indirect_buffer_size,
                .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                    VMA_ALLOCATION_CREATE_MAPPED_BIT,
                .name = "physics_indirect_buffer_cpu" } );

            Buffer* mapped_indirect_buffer = gpu.get_buffer( indirect_cpu_buffer );
            RASSERT( mapped_indirect_buffer );
            RASSERT( mapped_indirect_buffer->mapped_data );

            memcpy( mapped_indirect_buffer->mapped_data, indirect_commands.data, indirect_buffer_size );
            gpu.flush_buffer( indirect_cpu_buffer, 0, indirect_buffer_size );

            BufferHandle indirect_gpu_buffer = gpu.create_buffer( {
                .size = indirect_buffer_size,
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                         VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                .name = "physics_indirect_buffer_gpu" } );

            physics_mesh->draw_indirect_buffer = indirect_gpu_buffer;

            buffers.push( indirect_cpu_buffer );
            buffers.push( indirect_gpu_buffer );

            renderer->async_loader->request_buffer_copy( indirect_cpu_buffer, indirect_gpu_buffer );

            indirect_commands.shutdown();
        }

        // TODO(marco): set proper flags based on material type
        render_mesh.pbr_material.flags |= DrawFlags_Phong;
        if ( render_mesh.pbr_material.base_color_factor.w < 1.0f || render_mesh.pbr_material.alpha_texture_index != k_invalid_scene_texture_index ) {
            render_mesh.pbr_material.flags |= DrawFlags_Transparent;
        }

        render_scene->meshes.push( render_mesh );
    }

    materials.shutdown();
    indices.shutdown();

    // Assimp stores the node hierarchy in pointers, rather than indices. We need to manually where each node is stored
    aiNode* root_node = assimp_scene->mRootNode;

    Array<aiNode*> nodes_to_visit;
    nodes_to_visit.init( temp_allocator, 4 );

    FlatHashMap<aiNode*, u32> node_to_index;
    node_to_index.init( temp_allocator, 64 );

    // Calculate total node count: add first the root nodes.
    u32 total_node_count = root_node->mNumChildren;

    // Add initial nodes
    u32 node_index = 0;
    for ( u32 node_index = 0; node_index < root_node->mNumChildren; ++node_index ) {
        nodes_to_visit.push( root_node->mChildren[ node_index ] );
        node_to_index.insert( root_node->mChildren[ node_index ], node_index );
    }
    // Visit nodes
    while ( nodes_to_visit.size ) {
        aiNode* node = nodes_to_visit.front();
        nodes_to_visit.delete_swap( 0 );

        for ( u32 ch = 0; ch < node->mNumChildren; ++ch ) {
            const u32 children_index = node_index++;
            nodes_to_visit.push( node->mChildren[ ch ] );
            node_to_index.insert( node->mChildren[ ch ], children_index );
        }

        // Add only children nodes to the count, as the current node is
        // already calculated when inserting it.
        total_node_count += ( u32 )node->mNumChildren;
    }

    u32 node_offset = scene_graph->node_count();
    u32 new_node_count = node_offset + total_node_count;
    scene_graph->resize( new_node_count );
    scene_graph->init_new_nodes( node_offset, total_node_count );

    // Populate scene graph: visit again
    nodes_to_visit.clear();
    // Add initial nodes
   for ( u32 node_index = 0; node_index < root_node->mNumChildren; ++node_index ) {
        nodes_to_visit.push( root_node->mChildren[ node_index ] );
   }

    u32 total_meshlets = 0;

    while ( nodes_to_visit.size ) {
        aiNode* node = nodes_to_visit.front();
        u32 node_index = node_to_index.get_structure( node ).value + node_offset;
        nodes_to_visit.delete_swap( 0 );

        aiMatrix4x4 transform = node->mTransformation;
        transform.Transpose();
        // TODO(marco): double check that we are copying the right values
        memcpy( &scene_graph->local_matrices[ node_index ], &transform, sizeof( glm::mat4 ) );
        scene_graph->updated_nodes.set_bit( node_index );

        // Handle parent-relationship
        if ( node->mNumChildren ) {
            Hierarchy& node_hierarchy = scene_graph->nodes_hierarchy[ node_index ];
            node_hierarchy.children.init( resident_allocator, node->mNumChildren );

            for ( u32 ch = 0; ch < node->mNumChildren; ++ch) {
                aiNode* child = node->mChildren[ ch ];
                u32 global_child_index = node_to_index.get_structure( child ).value + node_offset;
                Hierarchy& children_hierarchy = scene_graph->nodes_hierarchy[ global_child_index ];
                scene_graph->set_hierarchy( global_child_index, node_index, node_hierarchy.level + 1 );

                nodes_to_visit.push( child );
            }
        }

        // Cache node name
        scene_graph->set_debug_data( node_index, node->mName.C_Str() );

        // u32 gltf_mesh_offset = render_scene->gltf_mesh_to_mesh_offset[ node->mMeshes[ 0 ] ];

        for ( u32 primitive_index = 0; primitive_index < node->mNumMeshes; ++primitive_index ) {
            MeshInstance mesh_instance{ };
            // Assign scene graph node index
            mesh_instance.scene_graph_node_index = node_index;

            u32 aimesh_index = node->mMeshes[ primitive_index ];
            aiMesh* assimp_mesh = assimp_scene->mMeshes[ aimesh_index ];

            u32 mesh_primitive_index = aimesh_to_mesh.get_structure( aimesh_index ).value + mesh_offset;

            // Cache parent mesh and assign material
            mesh_instance.mesh_index = mesh_primitive_index;

            Mesh& mesh = render_scene->meshes[ mesh_primitive_index ];
            // Cache gpu mesh instance index, used to retrieve data on gpu.
            mesh_instance.gpu_mesh_instance_index = render_scene->mesh_instances.size;

            mesh.skin_index = i32_max;

            total_meshlets += mesh.meshlet_count;

            render_scene->mesh_instances.push( mesh_instance );
        }
    }

    aimesh_to_mesh.shutdown();

    sizet mesh_count = render_scene->meshes.size - mesh_offset;
    /*sizet geometry_transform_buffer_size = sizeof( VkTransformMatrixKHR ) * mesh_count;

    BufferHandle geometry_transform_buffer = renderer->gpu->create_buffer( {
        .type_flags = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
        .usage = ResourceUsageType::Immutable, .size = (u32)geometry_transform_buffer_size,
        .persistent = 1, .name = "geometry_transform_buffer" });
    renderer->render_blackboard.geometry_transform_buffers.push( geometry_transform_buffer );

    Array<VkTransformMatrixKHR> geometry_transform;
    u32 transform_count = u32( render_scene->mesh_instances.size - mesh_instances_offset );
    geometry_transform.init( temp_allocator, transform_count, transform_count );

    for ( u32 mesh_index = 0; mesh_index < transform_count; ++mesh_index ) {
        MeshInstance& mesh_instance = render_scene->mesh_instances[ mesh_index + mesh_instances_offset];
        RASSERT( mesh_instance.mesh_index != u32_max );
        Mesh& mesh = render_scene->meshes[ mesh_instance.mesh_index ];

        VkAccelerationStructureGeometryKHR geometry{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
        geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geometry.flags =  mesh.is_transparent() ? 0 : VK_GEOMETRY_OPAQUE_BIT_KHR;

        u32 vertex_count = mesh.primitive_count / 3;

        geometry.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        geometry.geometry.triangles.vertexData.deviceAddress = renderer->gpu->get_buffer_device_address( mesh.position_buffer ) + mesh.position_offset;
        geometry.geometry.triangles.vertexStride = sizeof( float ) * 3;
        geometry.geometry.triangles.maxVertex = vertex_count;
        geometry.geometry.triangles.indexType = mesh.index_type;
        geometry.geometry.triangles.indexData.deviceAddress = renderer->gpu->get_buffer_device_address( mesh.index_buffer ) + mesh.index_offset;
        geometry.geometry.triangles.transformData.deviceAddress = renderer->gpu->get_buffer_device_address( geometry_transform_buffer );

        renderer->render_blackboard.geometries.push( geometry );

        VkAccelerationStructureBuildRangeInfoKHR build_range_info{ };
        build_range_info.primitiveCount = vertex_count;
        build_range_info.primitiveOffset = 0;
        build_range_info.transformOffset = sizeof( VkTransformMatrixKHR ) * mesh_index;

        renderer->render_blackboard.build_range_infos.push( build_range_info );

        mat4s& local_transform = scene_graph->local_matrices[ mesh_instance.scene_graph_node_index ];
        VkTransformMatrixKHR& transform = geometry_transform[ mesh_index ];
        for ( int y = 0; y < 3; ++y ) {
            for ( int x = 0; x < 4; ++x ) {
                transform.matrix[ y ][ x ] = local_transform.raw[ y ][ x ];
            }
        }
    }

    Buffer* gpu_geometry_transform_buffer = renderer->gpu->get_buffer( geometry_transform_buffer );
    memcpy( gpu_geometry_transform_buffer->mapped_data, geometry_transform.data, geometry_transform_buffer_size );*/

    temp_allocator->free_marker( temp_allocator_initial_marker );

    i64 end_reading_buffers_data = time_now();

    i64 end_creating_buffers = time_now();

    i64 end_loading = time_now();

    rprint( "Loaded scene %s in %f seconds.\nStats:\n\tReading obj file %f seconds\n\tTextures Creating %f seconds\n\tReading Buffers Data %f seconds\n\tCreating Buffers %f seconds\n", filename,
            time_delta_seconds( start_scene_loading, end_loading ), time_delta_seconds( start_scene_loading, end_loading_file ), time_delta_seconds( end_loading_file, end_creating_textures ),
            time_delta_seconds( end_creating_textures, end_reading_buffers_data ), time_delta_seconds( end_reading_buffers_data, end_creating_buffers ) );
}

u32 ObjModel::load_texture( cstring texture_path, cstring path, ArenaAllocator* temp_allocator ) {
    cstring texture_name = render_scene->names_buffer.append_use( texture_path );

    u64 texture_key = hash_calculate( texture_name );
    auto map_it = texture_map.find( texture_key );
    if ( map_it.is_valid() ) {
        ImageHandle existing_texture;
        existing_texture.id = texture_map.get( map_it );
        return existing_texture.index();
    }

    StringBuffer name_buffer;
    name_buffer.init( 4096, temp_allocator );

    // Reconstruct file path
    char* full_filename = name_buffer.append_use_f( "%s%s", path, texture_path );

    TextureResource* tr = renderer->create_texture_from_file( full_filename, true, false );
    RASSERT( tr != nullptr );

    renderer->gpu->link_image_sampler( tr->image, sampler->handle );

    render_scene->images.push( *tr );
    // Reset name buffer
    name_buffer.shutdown();

    texture_map.insert( texture_key, tr->image.id );

    return tr->image_view.index();
}

void ObjModel::shutdown( Renderer* renderer ) {

    GpuDevice& gpu = *renderer->gpu;

    for ( u32 i = 0; i < buffers.size; ++i ) {
        renderer->gpu->destroy_buffer( buffers[ i ] );
    }

    // Free scene buffers
    buffers.shutdown();

    // TODO(marco): this could be freed earlier
    aiReleaseImport( assimp_scene );

    texture_map.shutdown();
}

} // namespace raptor
