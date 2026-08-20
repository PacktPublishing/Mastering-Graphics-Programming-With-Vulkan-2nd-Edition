#include "graphics/gltf_model.hpp"
#include "graphics/gpu_profiler.hpp"
#include "graphics/raptor_imgui.hpp"
#include "graphics/asynchronous_loader.hpp"
#include "graphics/scene_graph.hpp"

#include "foundation/file.hpp"
#include "foundation/time.hpp"
#include "foundation/numerics.hpp"

#include "external/imgui/imgui.h"
#include "external/stb_image.h"

#include "external/glm/matrix.hpp"
#include "external/glm/mat4x4.hpp"
#include "external/glm/vec3.hpp"
#include "external/glm/gtc/quaternion.hpp"

#include "external/tracy/tracy/Tracy.hpp"
#define CGLTF_IMPLEMENTATION
#include "external/cgltf/cgltf.h"

namespace raptor {

static i32 get_data_offset( i32 accessor_offset, i32 buffer_view_offset ) {

    i32 byte_offset = buffer_view_offset;
    byte_offset += accessor_offset;
    return byte_offset;
}

//
// glTFModel //////////////////////////////////////////////////////////////

void glTFModel::fill_pbr_material( cgltf_data& gltf_scene, Renderer& renderer, u32 image_offset, u32 sampler_offset, cgltf_material& material, PBRMaterial& pbr_material ) {
    GpuDevice& gpu = *renderer.gpu;

    // Handle flags
    if ( material.alpha_mode == cgltf_alpha_mode_mask ) {
        pbr_material.flags |= DrawFlags_AlphaMask;
    } else if ( material.alpha_mode == cgltf_alpha_mode_blend ) {
        // TODO: how to choose when using dithering and traditional blending ?
        pbr_material.flags |= DrawFlags_Transparent;
        //pbr_material.flags |= DrawFlags_AlphaDither;
    }

    pbr_material.flags |= material.double_sided ? DrawFlags_DoubleSided : 0;
    // Alpha cutoff
    pbr_material.alpha_cutoff = material.alpha_cutoff;

    if ( material.has_pbr_metallic_roughness ) {
        pbr_material.base_color_factor = glm::vec4( material.pbr_metallic_roughness.base_color_factor[0],
                                                    material.pbr_metallic_roughness.base_color_factor[1],
                                                    material.pbr_metallic_roughness.base_color_factor[2],
                                                    material.pbr_metallic_roughness.base_color_factor[3] );

        pbr_material.roughness = material.pbr_metallic_roughness.roughness_factor;
        pbr_material.metallic = material.pbr_metallic_roughness.metallic_factor;

        pbr_material.diffuse_texture_index = get_material_texture( gpu, gltf_scene, image_offset, sampler_offset, material.pbr_metallic_roughness.base_color_texture );
        pbr_material.roughness_texture_index = get_material_texture( gpu, gltf_scene, image_offset, sampler_offset, material.pbr_metallic_roughness.metallic_roughness_texture );
    }

    if ( material.emissive_texture.texture != nullptr ) {
        pbr_material.emissive_texture_index = get_material_texture( gpu, gltf_scene, image_offset, sampler_offset, material.emissive_texture );
    } else {
        pbr_material.emissive_texture_index = -1;
    }

    pbr_material.emissive_factor = glm::vec3( material.emissive_factor[0], material.emissive_factor[1], material.emissive_factor[2] );

    if ( material.occlusion_texture.texture != nullptr ) {
        pbr_material.occlusion_texture_index = get_material_texture( gpu, gltf_scene, image_offset, sampler_offset, material.occlusion_texture );
    } else {
        pbr_material.occlusion_texture_index = -1;
    }

    if ( material.normal_texture.texture ) {
        pbr_material.normal_texture_index = get_material_texture( gpu, gltf_scene, image_offset, sampler_offset, material.normal_texture );
    } else {
        pbr_material.normal_texture_index = -1;
    }

    if ( material.occlusion_texture.texture != nullptr ) {
        pbr_material.occlusion = material.occlusion_texture.scale;
    }
}

u16 glTFModel::get_material_texture( GpuDevice& gpu, cgltf_data& gltf_scene, u32 image_offset, u32 sampler_offset, cgltf_texture_view& texture_info ) {

    if ( texture_info.texture != nullptr ) {
        cgltf_texture* gltf_texture = texture_info.texture;
        TextureResource& texture_gpu = render_scene->images[ ( u32 )cgltf_image_index( &gltf_scene, gltf_texture->image ) + image_offset ];

        if ( gltf_texture->sampler != nullptr ) {
            SamplerResource& sampler_gpu = render_scene->samplers[ ( u32 )cgltf_sampler_index( &gltf_scene, gltf_texture->sampler ) + sampler_offset ];

            gpu.link_image_sampler( texture_gpu.image, sampler_gpu.handle );
        }

        return texture_gpu.image_view.index();
    }
    else {
        return k_invalid_scene_texture_index;
    }
}

void glTFModel::init( RenderScene* render_scene_, SceneGraph* scene_graph_, Allocator* resident_allocator_, Renderer* renderer_ ) {

    render_scene = render_scene_;
    resident_allocator = resident_allocator_;
    renderer = renderer_;
    scene_graph = scene_graph_;
}

void glTFModel::load_model( cstring filename, cstring path, ArenaAllocator* temp_allocator ) {

    sizet temp_allocator_initial_marker = temp_allocator->get_marker();

    // Time statistics
    i64 start_scene_loading = time_now();

    // TODO(marco): use our own allocators
    cgltf_options options = { };
    cgltf_result result = cgltf_parse_file( &options, filename, &gltf_data );
    if ( result != cgltf_result_success ) {
        rprint( "Failed to open gltf file %s\n", filename );
        return;
    }

    i64 end_loading_file = time_now();

    StringBuffer temp_name_buffer;
    temp_name_buffer.init( 4096, temp_allocator );

    u32 image_offset = render_scene->images.size;
    u32 sampler_offset = render_scene->samplers.size;

    for ( u32 image_index = 0; image_index < gltf_data->images_count; ++image_index ) {
        cgltf_image& image = gltf_data->images[ image_index ];

        // Reconstruct file path
        char* full_filename = temp_name_buffer.append_use_f( "%s%s", path, image.uri );

        TextureResource* tr = renderer->create_texture_from_file( full_filename, true, false );
        RASSERT( tr != nullptr );

        render_scene->images.push( *tr );
        // Reset name buffer
        temp_name_buffer.clear();
    }

    i64 end_loading_textures_files = time_now();

    i64 end_creating_textures = time_now();

    // Load all samplers
    for ( u32 sampler_index = 0; sampler_index < gltf_data->samplers_count; ++sampler_index ) {
        cgltf_sampler& sampler = gltf_data->samplers[ sampler_index ];

        char* sampler_name = render_scene->names_buffer.append_use_f( "sampler_%u", sampler_index );

        SamplerCreation creation;
        switch ( sampler.min_filter ) {
            case cgltf_filter_type_nearest:
                creation.min_filter = VK_FILTER_NEAREST;
                break;
            case cgltf_filter_type_linear:
                creation.min_filter = VK_FILTER_LINEAR;
                break;
            case cgltf_filter_type_linear_mipmap_nearest:
                creation.min_filter = VK_FILTER_LINEAR;
                creation.mip_filter = VK_SAMPLER_MIPMAP_MODE_NEAREST;
                break;
            case cgltf_filter_type_linear_mipmap_linear:
                creation.min_filter = VK_FILTER_LINEAR;
                creation.mip_filter = VK_SAMPLER_MIPMAP_MODE_LINEAR;
                break;
            case cgltf_filter_type_nearest_mipmap_nearest:
                creation.min_filter = VK_FILTER_NEAREST;
                creation.mip_filter = VK_SAMPLER_MIPMAP_MODE_NEAREST;
                break;
            case cgltf_filter_type_nearest_mipmap_linear:
                creation.min_filter = VK_FILTER_NEAREST;
                creation.mip_filter = VK_SAMPLER_MIPMAP_MODE_LINEAR;
                break;
        }

        creation.mag_filter = sampler.mag_filter == cgltf_filter_type_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;

        switch ( sampler.wrap_s ) {
            case cgltf_wrap_mode_clamp_to_edge:
                creation.address_mode_u = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                break;
            case cgltf_wrap_mode_mirrored_repeat:
                creation.address_mode_u = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
                break;
            case cgltf_wrap_mode_repeat:
                creation.address_mode_u = VK_SAMPLER_ADDRESS_MODE_REPEAT;
                break;
        }

        switch ( sampler.wrap_t ) {
            case cgltf_wrap_mode_clamp_to_edge:
                creation.address_mode_v = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                break;
            case cgltf_wrap_mode_mirrored_repeat:
                creation.address_mode_v = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
                break;
            case cgltf_wrap_mode_repeat:
                creation.address_mode_v = VK_SAMPLER_ADDRESS_MODE_REPEAT;
                break;
        }

        creation.name = sampler_name;

        SamplerResource* sr = renderer->create_sampler( creation );
        RASSERT( sr != nullptr );

        render_scene->samplers.push( *sr );
    }

    i64 end_creating_samplers = time_now();

    // Temporary array of buffer data
    Array<void*> buffers_data;
    buffers_data.init( resident_allocator, ( u32 )gltf_data->buffers_count );

    temp_name_buffer.clear();

    for ( u32 buffer_index = 0; buffer_index < ( u32 )gltf_data->buffers_count; ++buffer_index ) {
        cgltf_buffer& buffer = gltf_data->buffers[ buffer_index ];

        char* full_filename = temp_name_buffer.append_use_f( "%s%s", path, buffer.uri );
        FileReadResult buffer_data = file_read_binary( full_filename, resident_allocator );
        buffers_data.push( buffer_data.data );

        temp_name_buffer.clear();
    }

    i64 end_reading_buffers_data = time_now();

    mesh_aabb[0] = glm::vec3{ FLT_MAX, FLT_MAX, FLT_MAX };
    mesh_aabb[1] = glm::vec3{ -FLT_MAX, -FLT_MAX, -FLT_MAX };

    u32 mesh_offset = render_scene->meshes.size;
    u32 mesh_instances_offset = render_scene->mesh_instances.size;

    // Mesh data is stored as primitive in GLTF lingo. Primitives can be seen as submeshes, to use
    // a more common term. Each primitive/submesh has its own material and can be rendered independently.
    // Cache mesh offset to differentiate 
    Array<u32> gltf_mesh_to_submesh_offset;
    gltf_mesh_to_submesh_offset.init( temp_allocator, ( u32 )gltf_data->meshes_count );

    u32 current_mesh_index = mesh_offset;

    // Temp allocator will be used to allocate meshlets, save now to save the gltf mesh to mesh array
    sizet temp_marker = temp_allocator->get_marker();

    for ( u32 mi = 0; mi < gltf_data->meshes_count; ++mi ) {
        cgltf_mesh& mesh = gltf_data->meshes[ mi ];

        gltf_mesh_to_submesh_offset.push( current_mesh_index );
        current_mesh_index += ( u32 )mesh.primitives_count;

        for ( u32 p = 0; p < mesh.primitives_count; ++p ) {
            cgltf_primitive& mesh_primitive = mesh.primitives[ p ];

            /*if ( mesh_primitive.material != cgltf_INVALID_INT_VALUE ) {
                cgltf_Material& material = gltf_data->materials[ mesh_primitive.material ];

                if ( ( material.alpha_mode.data != nullptr && strcmp( material.alpha_mode.data, "MASK" ) == 0 ) ||
                     ( material.alpha_mode.data != nullptr && strcmp( material.alpha_mode.data, "BLEND" ) == 0 ) ) {
                    continue;
                }
            }*/

            // Add meshes
            Mesh mesh{};
            // Load material defaults: flags is modified after this point.
            mesh.pbr_material = {};

            // Vertex positions
            const cgltf_accessor* position_buffer_accessor = cgltf_find_accessor( &mesh_primitive, cgltf_attribute_type_position, 0 );
            cgltf_buffer_view* position_buffer_view = position_buffer_accessor->buffer_view;
            i32 position_data_offset = get_data_offset( ( i32 )position_buffer_accessor->offset, ( i32 )position_buffer_view->offset );
            f32* vertices = ( f32* )((u8*)buffers_data[ ( u32 )cgltf_buffer_index( gltf_data, position_buffer_view->buffer ) ] + position_data_offset);
            mesh.position_count = ( u32 )position_buffer_accessor->count;
            mesh.position_offset = render_scene->write_vertices_to_staging_buffer( ( glm::vec3* )vertices, mesh.position_count );

            if ( renderer->gpu->mesh_shaders_extension_present ) {
                // Calculate bounding sphere center
                glm::vec3 position_min{ position_buffer_accessor->min[ 0 ], position_buffer_accessor->min[ 1 ], position_buffer_accessor->min[ 2 ] };
                glm::vec3 position_max{ position_buffer_accessor->max[ 0 ], position_buffer_accessor->max[ 1 ], position_buffer_accessor->max[ 2 ] };
                glm::vec3 bounding_center = position_min + position_max;
                bounding_center = bounding_center * 0.5f;

                // Calculate bounding sphere radius
                f32 radius = raptor::max( glm::distance( position_max, bounding_center ), glm::distance( position_min, bounding_center ) );
                mesh.bounding_sphere = { bounding_center.x, bounding_center.y, bounding_center.z, radius };

                mesh.aabb[ 0 ] = position_min;
                mesh.aabb[ 1 ] = position_max;
            }

            // Vertex normals
            const cgltf_accessor* normal_buffer_accessor = cgltf_find_accessor( &mesh_primitive, cgltf_attribute_type_normal, 0 );
            f32* normals = nullptr;
            if ( normal_buffer_accessor != nullptr ) {

                cgltf_buffer_view* normal_buffer_view = normal_buffer_accessor->buffer_view;
                i32 normal_data_offset = get_data_offset( ( i32 )normal_buffer_accessor->offset, ( i32 )normal_buffer_view->offset );
                normals = ( f32* )((u8*)buffers_data[ ( u32 )cgltf_buffer_index( gltf_data, normal_buffer_view->buffer ) ] + normal_data_offset);
                mesh.normal_offset = render_scene->write_normals_to_staging_buffer( ( glm::vec3* )normals, ( u32 )normal_buffer_accessor->count );
                mesh.pbr_material.flags |= DrawFlags_HasNormals;
            }

            // Vertex texture coords
            const cgltf_accessor* tex_coord_buffer_accessor = cgltf_find_accessor( &mesh_primitive, cgltf_attribute_type_texcoord, 0 );
            f32* tex_coords = nullptr;
            if ( tex_coord_buffer_accessor != nullptr ) {

                cgltf_buffer_view* tex_coord_buffer_view = tex_coord_buffer_accessor->buffer_view;
                i32 tex_coord_data_offset = get_data_offset( ( i32 )tex_coord_buffer_accessor->offset, ( i32 )tex_coord_buffer_view->offset );
                tex_coords = ( f32* )((u8*)buffers_data[ ( u32 )cgltf_buffer_index( gltf_data, tex_coord_buffer_view->buffer ) ] + tex_coord_data_offset);
                mesh.texcoord_offset = render_scene->write_texcoords_to_staging_buffer( ( glm::vec2* )tex_coords, ( u32 )tex_coord_buffer_accessor->count );
                mesh.pbr_material.flags |= DrawFlags_HasTexCoords;
            }

            // Vertex tangents
            const cgltf_accessor* tangent_buffer_accessor = cgltf_find_accessor( &mesh_primitive, cgltf_attribute_type_tangent, 0 );
            f32* tangents = nullptr;
            if ( tangent_buffer_accessor != nullptr ) {

                RASSERT( tangent_buffer_accessor->type == cgltf_type_vec4 );
                cgltf_buffer_view* tangent_buffer_view = tangent_buffer_accessor->buffer_view;
                i32 tangent_data_offset = get_data_offset( ( i32 )tangent_buffer_accessor->offset, ( i32 )tangent_buffer_view->offset );
                tangents = ( f32* )((u8*)buffers_data[ ( u32 )cgltf_buffer_index( gltf_data, tangent_buffer_view->buffer ) ] + tangent_data_offset);
                mesh.tangent_offset = render_scene->write_tangents_to_staging_buffer( ( glm::vec4* )tangents, ( u32 )tangent_buffer_accessor->count );
                mesh.pbr_material.flags |= DrawFlags_HasTangents;
            }

            const cgltf_accessor* joints_accessor = cgltf_find_accessor( &mesh_primitive, cgltf_attribute_type_joints, 0 );
            if ( joints_accessor != nullptr ) {
                cgltf_buffer_view* joints_buffer_view = joints_accessor->buffer_view;
                i32 joints_data_offset = get_data_offset( ( i32 )joints_accessor->offset, ( i32 )joints_buffer_view->offset );
                RASSERT( joints_accessor->type == cgltf_type_vec4 );
                RASSERT( joints_accessor->component_type == cgltf_component_type_r_16u );
                u16* joints = ( u16* )((u8*)buffers_data[ ( u32 )cgltf_buffer_index( gltf_data, joints_buffer_view->buffer ) ] + joints_data_offset);
                mesh.joints_offset = render_scene->write_joints_to_staging_buffer( ( glm::u16vec4* )joints, ( u32 )joints_accessor->count );

                mesh.pbr_material.flags |= DrawFlags_HasJoints;
            }

            const cgltf_accessor* weights_accessor = cgltf_find_accessor( &mesh_primitive, cgltf_attribute_type_weights, 0 );
            if ( weights_accessor != nullptr ) {
                cgltf_buffer_view* weights_buffer_view = weights_accessor->buffer_view;
                i32 weights_data_offset = get_data_offset( ( i32 )weights_accessor->offset, ( i32 )weights_buffer_view->offset );
                f32* weights = ( f32* )((u8*)buffers_data[ ( u32 )cgltf_buffer_index( gltf_data, weights_buffer_view->buffer ) ] + weights_data_offset);
                mesh.weights_offset = render_scene->write_weights_to_staging_buffer( ( glm::vec4* )weights, ( u32 )weights_accessor->count );

                mesh.pbr_material.flags |= DrawFlags_HasWeights;
            }

            // Index buffer
            cgltf_buffer_view* index_buffer_view = mesh_primitive.indices->buffer_view;
            u8* buffer_data = ( u8* )buffers_data[ ( u32 )cgltf_buffer_index( gltf_data, index_buffer_view->buffer ) ];
            i32 index_data_offset = get_data_offset( ( i32 )mesh_primitive.indices->offset, ( i32 )mesh_primitive.indices->buffer_view->offset );
            u16* indices = ( u16* )( buffer_data + index_data_offset );

            // Read pbr material data if present
            if ( mesh_primitive.material != nullptr ) {
                cgltf_material& material = *mesh_primitive.material;
                fill_pbr_material( *gltf_data, *renderer, image_offset, sampler_offset, material, mesh.pbr_material );
            }

            if ( mesh_primitive.indices->component_type == cgltf_component_type_r_16u ) {
                mesh.index_type = VK_INDEX_TYPE_UINT16;
                mesh.index_offset_bytes = render_scene->write_indices_to_staging_buffer( ( u32* )indices, ( u32 )( ( mesh_primitive.indices->count + 1 ) / 2 ) );
            } else {
                mesh.index_type = VK_INDEX_TYPE_UINT32;
                mesh.index_offset_bytes = render_scene->write_indices_to_staging_buffer( ( u32* )indices, ( u32 )( mesh_primitive.indices->count ) );
            }
            mesh.index_count = ( u32 )mesh_primitive.indices->count;
            mesh.gpu_mesh_index = render_scene->meshes.size;

            build_meshlets( mesh, temp_allocator );

            // Add mesh with all data
            render_scene->meshes.push( mesh );

            temp_allocator->free_marker( temp_marker );
        }
    }

    cgltf_scene& root_gltf_scene = *gltf_data->scene;

    //
    Array<i32> nodes_to_visit;
    nodes_to_visit.init( temp_allocator, 4 );

    // Calculate total node count: add first the root nodes.
    u32 total_node_count = ( u32 )root_gltf_scene.nodes_count;

    // Add initial nodes
    for ( u32 node_index = 0; node_index < root_gltf_scene.nodes_count; ++node_index ) {
        const i32 node = ( i32 )cgltf_node_index( gltf_data, root_gltf_scene.nodes[ node_index ] );
        nodes_to_visit.push( node );
    }
    // Visit nodes
    while ( nodes_to_visit.size ) {
        i32 node_index = nodes_to_visit.front();
        nodes_to_visit.delete_swap( 0 );

        cgltf_node& node = gltf_data->nodes[ node_index ];
        for ( u32 ch = 0; ch < node.children_count; ++ch ) {
            const i32 children_index = ( i32 )cgltf_node_index( gltf_data, node.children[ ch ] );
            nodes_to_visit.push( children_index );
        }

        // Add only children nodes to the count, as the current node is
        // already calculated when inserting it.
        total_node_count += ( u32 )node.children_count;
    }

    u32 node_offset = scene_graph->node_count();
    u32 new_node_count = node_offset + total_node_count;
    scene_graph->resize( new_node_count );
    scene_graph->init_new_nodes( node_offset, total_node_count );

    // Populate scene graph: visit again
    nodes_to_visit.clear();
    // Add initial nodes
    for ( u32 node_index = 0; node_index < root_gltf_scene.nodes_count; ++node_index ) {
        const i32 node = ( i32 )cgltf_node_index( gltf_data, root_gltf_scene.nodes[ node_index ] );
        nodes_to_visit.push( node );
    }

    u32 total_meshlets = 0;

    while ( nodes_to_visit.size ) {
        i32 gltf_node = nodes_to_visit.front();
        i32 node_index = gltf_node + node_offset;
        nodes_to_visit.delete_swap( 0 );

        cgltf_node& node = gltf_data->nodes[ gltf_node ];

        // Compute local transform: read either raw matrix or individual Scale/Rotation/Translation components
        if ( node.has_matrix ) {
            // CGLM and glTF have the same matrix layout, just memcopy it
            memcpy( &scene_graph->local_matrices[ node_index ], node.matrix, sizeof( glm::mat4 ) );
            scene_graph->updated_nodes.set_bit( node_index );
        }
        else {
            // Handle individual transform components: SRT (scale, rotation, translation)
            glm::vec3 node_scale{ 1.0f, 1.0f, 1.0f };
            if ( node.has_scale ) {
                node_scale = glm::vec3{ node.scale[ 0 ], node.scale[ 1 ], node.scale[ 2 ] };
            }

            glm::vec3 node_translation{ 0.f, 0.f, 0.f };
            if ( node.has_translation ) {
                node_translation = glm::vec3{ node.translation[ 0 ], node.translation[ 1 ], node.translation[ 2 ] };
            }

            // Rotation is written as a plain quaternion
            glm::quat node_rotation = glm::quat( 1.0f, 0.0f, 0.0f, 0.0f );
            if ( node.has_rotation ) {
                node_rotation = glm::quat( node.rotation[ 3 ], node.rotation[ 0 ], node.rotation[ 1 ], node.rotation[ 2 ] );
            }

            Transform transform;
            transform.translation = node_translation;
            transform.scale = node_scale;
            transform.rotation = node_rotation;

            // Final SRT composition
            const glm::mat4 local_matrix = transform.calculate_matrix();
            scene_graph->set_local_matrix( node_index, local_matrix );
        }

        // Handle parent-relationship
        if ( node.children_count ) {
            Hierarchy& node_hierarchy = scene_graph->nodes_hierarchy[ node_index ];
            node_hierarchy.children.init( resident_allocator, ( u32 )node.children_count );

            for ( u32 ch = 0; ch < node.children_count; ++ch) {
                const i32 children_index = ( i32 )cgltf_node_index( gltf_data, node.children[ ch ] );
                i32 global_child_index = children_index + node_offset;
                // Hierarchy& children_hierarchy = scene_graph->nodes_hierarchy[ global_child_index ];
                scene_graph->set_hierarchy( global_child_index, node_index, node_hierarchy.level + 1 );

                nodes_to_visit.push( children_index );
            }
        }

        // Cache node name
        scene_graph->set_debug_data( node_index, node.name );

        if ( node.mesh == nullptr ) {
            continue;
        }

        // Start mesh part
        cgltf_mesh& gltf_mesh = *node.mesh;

        const u32 gltf_mesh_index = ( u32 )cgltf_mesh_index( gltf_data, node.mesh );
        const u32 gltf_submesh_offset = gltf_mesh_to_submesh_offset[ gltf_mesh_index ];

        // Remember: GLTF primitives are conceptually submeshes.
        u32 skin_index_offset = render_scene->skins.size;
        for ( u32 primitive_index = 0; primitive_index < gltf_mesh.primitives_count; ++primitive_index ) {
            MeshInstance mesh_instance{ };
            // Assign scene graph node index
            mesh_instance.scene_graph_node_index = node_index;

            cgltf_primitive& mesh_primitive = gltf_mesh.primitives[ primitive_index ];

            // Cache parent mesh and assign material
            const u32 mesh_primitive_index = gltf_submesh_offset + primitive_index;
            mesh_instance.mesh_index = mesh_primitive_index;

            Mesh& mesh = render_scene->meshes[ mesh_primitive_index ];
            // Cache gpu mesh instance index, used to retrieve data on gpu.
            mesh_instance.gpu_mesh_instance_index = render_scene->mesh_instances.size;

            // Found a skin index, cache it
            mesh.skin_index = i32_max;
            if ( node.skin != nullptr ) {
                cgltf_size skin_index = cgltf_skin_index( gltf_data, node.skin );

                mesh.skin_index = i32( skin_index_offset + skin_index );
            }

            total_meshlets += mesh.meshlet_count;

            render_scene->mesh_instances.push( mesh_instance );
        }
    }

    rprint( "Total meshlet instances %u\n", total_meshlets );

    i64 end_building_meshlets = time_now();

    // Before unloading buffer data, load animations
    for ( u32 animation_index = 0; animation_index < gltf_data->animations_count; ++animation_index ) {
        cgltf_animation& gltf_animation = gltf_data->animations[ animation_index ];

        Animation& animation = render_scene->animations.push_use();
        animation.time_start = FLT_MAX;
        animation.time_end = -FLT_MAX;
        animation.channels.init( resident_allocator, ( u32 )gltf_animation.channels_count, ( u32 )gltf_animation.channels_count );

        u32 max_node_index = 0;

        for ( u32 channel_index = 0; channel_index < gltf_animation.channels_count; ++channel_index ) {

            cgltf_animation_channel& gltf_channel = gltf_animation.channels[ channel_index ];
            AnimationChannel& channel = animation.channels[ channel_index ];

            channel.sampler = ( i32 )cgltf_animation_sampler_index( &gltf_animation, gltf_channel.sampler );
            channel.target_node = ( i32 )cgltf_node_index( gltf_data, gltf_channel.target_node );
            max_node_index = raptor::max( max_node_index, ( u32 )channel.target_node );
            // TODO(marco): translate to raptor target type
            channel.target_type = (raptor::AnimationChannel::TargetType)gltf_channel.target_path;
        }

        animation.animated_transforms.init( resident_allocator, max_node_index + 1, max_node_index + 1 );
        for ( Transform& animated_transform : animation.animated_transforms ) {
            animated_transform.reset();
        }

        animation.samplers.init( resident_allocator, ( u32 )gltf_animation.samplers_count, ( u32 )gltf_animation.samplers_count );
        for ( u32 sampler_index = 0; sampler_index < gltf_animation.samplers_count; ++sampler_index ) {

            cgltf_animation_sampler& gltf_sampler = gltf_animation.samplers[ sampler_index ];
            AnimationSampler& sampler = animation.samplers[ sampler_index ];

            sampler.interpolation_type = ( raptor::AnimationSampler::Interpolation )gltf_sampler.interpolation;

            i32 key_frames_count = 0;

            // Copy keyframe data
            {
                cgltf_accessor& buffer_accessor = *gltf_sampler.input;
                cgltf_buffer_view& buffer_view = *gltf_sampler.input->buffer_view;

                i32 byte_offset = get_data_offset( ( i32 )buffer_accessor.offset, ( i32 )buffer_view.offset );

                u8* buffer_data = ( u8* )buffers_data[ ( u32 )cgltf_buffer_index( gltf_data, buffer_view.buffer ) ] + byte_offset;
                sampler.key_frames.init( resident_allocator, ( u32 )buffer_accessor.count, ( u32 )buffer_accessor.count );

                const f32* key_frames = ( const f32* )buffer_data;
                for ( u32 i = 0; i < buffer_accessor.count; ++i ) {
                    sampler.key_frames[ i ] = key_frames[ i ];

                    animation.time_start = glm::min( animation.time_start, key_frames[ i ] );
                    animation.time_end = glm::max( animation.time_end, key_frames[ i ] );
                }

                key_frames_count = ( i32 )buffer_accessor.count;
            }
            // Copy animation data
            {
                cgltf_accessor& buffer_accessor = *gltf_sampler.output;
                cgltf_buffer_view& buffer_view = *buffer_accessor.buffer_view;

                i32 byte_offset = get_data_offset( ( i32 )buffer_accessor.offset, ( i32 )buffer_view.offset );

                // TODO(marco): this can be 3x of the input if the interpolation is cubic
                RASSERT( buffer_accessor.count == key_frames_count );

                u8* buffer_data = ( u8* )buffers_data[ ( u32 )cgltf_buffer_index( gltf_data, buffer_view.buffer ) ] + byte_offset;

                sampler.data = ( glm::vec4* )rallocaa( sizeof( glm::vec4 ) * buffer_accessor.count, resident_allocator, 16 );

                switch ( buffer_accessor.type ) {
                    case cgltf_type_vec3:
                    {
                        const glm::vec3* animation_data = ( const glm::vec3* )buffer_data;
                        for ( u32 i = 0; i < buffer_accessor.count; ++i ) {
                            sampler.data[ i ] = glm::vec4( animation_data[ i ], 0.f );
                        }
                        break;
                    }
                    case cgltf_type_vec4:
                    {
                        const f32* animation_data = ( const f32* )buffer_data;
                        for ( u32 i = 0; i < buffer_accessor.count; ++i ) {
                            sampler.data[ i ] = glm::vec4{ animation_data[ i * 4 ], animation_data[ i * 4 + 1 ], animation_data[ i * 4 + 2 ], animation_data[ i * 4 + 3 ] };
                        }
                        break;
                    }
                    default:
                    {
                        RASSERT( false );
                        break;
                    }
                }

            }
        }

        //rprint( "Done loading animation %f %f\n", animation.time_start, animation.time_end );
    }

    // Load skins
    for ( u32 si = 0; si < gltf_data->skins_count; ++si ) {
        cgltf_skin& gltf_skin = gltf_data->skins[ si ];

        Skin& skin = render_scene->skins.push_use();
        skin.skeleton_root_index = ( u32 )cgltf_node_index( gltf_data, gltf_skin.skeleton );

        // CGLTF gives joins are cgltf_node*, we store node indices
        const u32 joints_count = ( u32 )gltf_skin.joints_count;
        // Copy joints by reading node indices
        skin.joints.init( resident_allocator, joints_count, joints_count );

        for ( u32 j = 0; j < joints_count; ++j ) {
            cgltf_node* joint_node = gltf_skin.joints[ j ];
            RASSERT( joint_node != nullptr );

            const cgltf_size node_index = cgltf_node_index( gltf_data, joint_node );
            RASSERT( node_index < scene_graph->node_count() );
            scene_graph->set_node_as_bone( ( u32 )node_index );
            skin.joints[ j ] = (u32)node_index;
        }

        // Copy inverse bind matrices
        cgltf_accessor& buffer_accessor = *gltf_skin.inverse_bind_matrices;
        cgltf_buffer_view& buffer_view = *buffer_accessor.buffer_view;

        i32 byte_offset = get_data_offset( ( i32 )buffer_accessor.offset, ( i32 )buffer_view.offset );

        RASSERT( buffer_accessor.count == skin.joints.size );
        skin.inverse_bind_matrices.init( resident_allocator, ( u32 )buffer_accessor.count, ( u32 )buffer_accessor.count );

        u8* buffer_data = ( u8* )buffers_data[ ( u32 )cgltf_buffer_index( gltf_data, buffer_view.buffer ) ] + byte_offset;
        memory_copy( skin.inverse_bind_matrices.data, buffer_data, sizeof( glm::mat4 ) * buffer_accessor.count );
    }

    // Deallocate file-read buffer data
    for ( u32 buffer_index = 0; buffer_index < gltf_data->buffers_count; ++buffer_index ) {
        void* buffer = buffers_data[ buffer_index ];
        resident_allocator->deallocate( buffer );
    }
    buffers_data.shutdown();

    i64 end_creating_buffers = time_now();

    // This is not needed anymore, free all temp memory after.
    //resource_name_buffer.shutdown();
    temp_allocator->free_marker( temp_allocator_initial_marker );

    i64 end_loading = time_now();

    rprint( "Loaded scene %s in %f seconds.\nStats:\n\tReading GLTF file %f seconds\n\tTextures Creating %f seconds\n\tCreating Samplers %f seconds\n\tReading Buffers Data %f seconds\n\tCreating Buffers %f seconds\n", filename,
            time_delta_seconds( start_scene_loading, end_loading ), time_delta_seconds( start_scene_loading, end_loading_file ), time_delta_seconds( end_loading_file, end_creating_textures ),
            time_delta_seconds( end_creating_textures, end_creating_samplers ),
            time_delta_seconds( end_creating_samplers, end_reading_buffers_data ), time_delta_seconds( end_reading_buffers_data, end_creating_buffers ) );
}

void glTFModel::shutdown( Renderer* renderer ) {

    // NOTE(marco): we can't destroy this sooner as textures and buffers
    // hold a pointer to the names stored here
    cgltf_free( gltf_data );
}

} // namespace raptor
