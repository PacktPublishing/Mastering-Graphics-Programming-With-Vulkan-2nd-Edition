
#include "application/window.hpp"
#include "application/input.hpp"
#include "application/game_camera.hpp"

#include "graphics/gpu_device.hpp"
#include "graphics/command_buffer.hpp"
#include "graphics/shader_compiler.hpp"
#include "graphics/gpu_profiler.hpp"
#include "graphics/raptor_imgui.hpp"
#include "graphics/renderer.hpp"
#include "graphics/render_scene.hpp"

#include "external/glm/mat4x4.hpp"
#include "external/glm/gtc/quaternion.hpp"

#include "foundation/file.hpp"
#include "foundation/time.hpp"
#include "foundation/resource_manager.hpp"

#include "external/imgui/imgui.h"

#include "external/tracy/tracy/Tracy.hpp"
#include "external/stb_image.h"
#include "external/cgltf/cgltf.h"

#include <stdio.h>
#include <stdlib.h>

///////////////////////////////////////

enum class MeshDrawType : u8 {
    NoCull_Opaque,
    Cull_Opaque,
    NoCull_Transparent,
    Cull_Transparent,
    Count
};

struct MeshDraw {
    raptor::PBRMaterial     material;

    raptor::BufferHandle    index_buffer;
    raptor::BufferHandle    position_buffer;
    raptor::BufferHandle    tangent_buffer;
    raptor::BufferHandle    normal_buffer;
    raptor::BufferHandle    texcoord_buffer;

    VkIndexType    index_type;
    u32         index_offset;
    u32         position_offset;
    u32         tangent_offset;
    u32         normal_offset;
    u32         texcoord_offset;

    u32         primitive_count;
    u32         draw_index;

    // Indices used for bindless textures.
    u16         diffuse_texture_index;
    u16         roughness_texture_index;
    u16         normal_texture_index;
    u16         occlusion_texture_index;
    u16         emissive_texture_index;

    MeshDrawType type;

    glm::vec4   base_color_factor;
    glm::vec4   metallic_roughness_occlusion_factor;
    glm::mat4   model;

    f32         alpha_cutoff;
    u32         flags;
}; // struct MeshDraw

//
//
struct alignas( 16 ) SceneData {
    glm::mat4   view_projection;
    glm::vec4   eye;
    glm::vec4   light;
    f32         light_range;
    f32         light_intensity;
    u32         padding_[ 2 ];
}; // struct SceneData

static_assert( sizeof( SceneData ) == 112 );

//
//
struct alignas( 16 ) DrawData {
    glm::mat4   model;
    glm::mat4   model_inverse;

    u32         textures[ 4 ]; // diffuse, roughness, normal, occlusion
    glm::vec4   base_color_factor;
    glm::vec4   metallic_roughness_occlusion_factor; // metallic, roughness, occlusion

    float       alpha_cutoff;
    u32         flags;
    u32         padding_[2];
}; // struct DrawData

static_assert( sizeof( DrawData ) == 192 );

//
//
struct GpuEffect {

    raptor::PipelineHandle          pipeline_cull;
    raptor::PipelineHandle          pipeline_no_cull;

}; // struct GpuEffect

// Input callback
static void input_os_messages_callback( void* os_event, void* user_data ) {
    raptor::InputService* input = ( raptor::InputService* )user_data;
    input->on_event( os_event );
}

static u8* get_buffer_data( cgltf_data* gltf_data, u32 buffer_view_index, raptor::Array<void*>& buffers_data, u32* buffer_size = nullptr, char** buffer_name = nullptr ) {
    using namespace raptor;

    cgltf_buffer_view& buffer_view = gltf_data->buffer_views[ buffer_view_index ];

    i32 offset = ( i32 )buffer_view.offset;

    if ( buffer_name != nullptr ) {
        *buffer_name = buffer_view.name;
    }

    if ( buffer_size != nullptr ) {
        *buffer_size = ( u32 )buffer_view.size;
    }

    u32 buffer_view_buffer_index = ( u32 )cgltf_buffer_index( gltf_data, buffer_view.buffer );
    u8* data = ( u8* )buffers_data[ buffer_view_buffer_index ] + offset;

    return data;
}

//
//
static void upload_draw_data( DrawData& draw_data, const MeshDraw& mesh_draw, const f32 global_scale ) {
    draw_data.textures[ 0 ] = mesh_draw.diffuse_texture_index;
    draw_data.textures[ 1 ] = mesh_draw.roughness_texture_index;
    draw_data.textures[ 2 ] = mesh_draw.normal_texture_index;
    draw_data.textures[ 3 ] = mesh_draw.occlusion_texture_index;
    draw_data.base_color_factor = mesh_draw.base_color_factor;
    draw_data.metallic_roughness_occlusion_factor = mesh_draw.metallic_roughness_occlusion_factor;
    draw_data.alpha_cutoff = mesh_draw.alpha_cutoff;
    draw_data.flags = mesh_draw.flags;

    glm::mat4 scale{ 1.0f };
    scale[ 0 ][ 0 ] = global_scale;
    scale[ 1 ][ 1 ] = global_scale;
    scale[ 2 ][ 2 ] = global_scale;

    draw_data.model = scale * mesh_draw.model;
    draw_data.model_inverse = glm::inverse( glm::transpose( draw_data.model ) );
}

//
//
static void draw_mesh( raptor::Renderer& renderer, raptor::CommandBuffer* gpu_commands,
                       raptor::DescriptorSetHandle scene_descriptor_set, MeshDraw& mesh_draw ) {
    gpu_commands->bind_vertex_buffer( mesh_draw.position_buffer, 0, mesh_draw.position_offset );

    if ( mesh_draw.tangent_buffer.is_valid() ) {
        gpu_commands->bind_vertex_buffer( mesh_draw.tangent_buffer, 1, mesh_draw.tangent_offset );
    }
    gpu_commands->bind_vertex_buffer( mesh_draw.normal_buffer, 2, mesh_draw.normal_offset );

    if ( mesh_draw.texcoord_buffer.is_valid() ) {
        gpu_commands->bind_vertex_buffer( mesh_draw.texcoord_buffer, 3, mesh_draw.texcoord_offset );
    }

    gpu_commands->bind_index_buffer( mesh_draw.index_buffer, mesh_draw.index_offset, mesh_draw.index_type );

    gpu_commands->bind_descriptor_set( { renderer.gpu->bindless_descriptor_set, scene_descriptor_set }, { } );

    gpu_commands->draw_indexed( raptor::TopologyType::Triangle, mesh_draw.primitive_count, 1, 0, 0, mesh_draw.draw_index );
}

//
//
struct Scene {

    raptor::Array<MeshDraw>                 mesh_draws;

    // All graphics resources used by the scene
    raptor::Array<raptor::TextureResource>  images;
    raptor::Array<raptor::SamplerResource>  samplers;
    raptor::Array<raptor::BufferHandle>     buffers;

    raptor::BufferHandle                    scene_buffer;
    raptor::DescriptorSetHandle             scene_descriptor_set;

    raptor::ImageHandle                     dummy_texture;
    raptor::ImageViewHandle                 dummy_texture_view;
    raptor::SamplerHandle                   dummy_sampler;

    cgltf_data*                             gltf_scene; // Source gltf scene

}; // struct GltfScene

static void scene_load_from_gltf( cstring filename, raptor::Renderer& renderer, raptor::Allocator* allocator, Scene& scene, raptor::Array<void*>& buffers_data ) {

    using namespace raptor;

    // Load all textures
    cgltf_options options = { };
    cgltf_data* gltf_data = nullptr;
    cgltf_result result = cgltf_parse_file( &options, filename, &gltf_data );
    if ( result != cgltf_result_success ) {
        rprint( "Failed to open gltf file %s\n", filename );
        exit( -1 );
    }
    scene.gltf_scene = gltf_data;

    scene.images.init( allocator, ( u32 )gltf_data->images_count, ( u32 )gltf_data->images_count );

    for ( u32 image_index = 0; image_index < gltf_data->images_count; ++image_index ) {
        cgltf_image& image = gltf_data->images[ image_index ];

        int comp, width, height;

        uint8_t* image_data = stbi_load( image.uri, &width, &height, &comp, 4 );
        if ( !image_data ) {
            rprint( "Error loading texture %s", image.uri );
            continue;
        }

        u32 mip_levels = 1;
        u32 w = width;
        u32 h = height;

        while ( w > 1 && h > 1 ) {
            w /= 2;
            h /= 2;

            ++mip_levels;
        }

        ImageCreation tc;
        tc.set_data( image_data ).set_format_type( VK_FORMAT_R8G8B8A8_UNORM, TextureType::Texture2D ).set_flags( 0 ).set_size( ( u16 )width, ( u16 )height, 1 ).set_name( image.uri ).set_mips( mip_levels );
        TextureResource* tr = renderer.create_texture( tc );
        RASSERT( tr != nullptr );

        scene.images[ image_index ] = *tr;
    }

    GpuDevice& gpu = *renderer.gpu;

    ImageCreation texture_creation{ };
    u32 zero_value = 0;
    texture_creation.set_name( "dummy_texture" ).set_size( 1, 1, 1 ).set_format_type( VK_FORMAT_R8G8B8A8_UNORM, TextureType::Texture2D ).set_flags( 1 ).set_data( &zero_value );
    scene.dummy_texture = gpu.create_image( texture_creation );
    scene.dummy_texture_view = gpu.create_image_view( {
        .parent_image = scene.dummy_texture, .view_type = VK_IMAGE_VIEW_TYPE_2D,
        .sub_resource ={ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }, .name = "dummy_texture_view" } );

    SamplerCreation sampler_creation{ };
    sampler_creation.min_filter = VK_FILTER_LINEAR;
    sampler_creation.mag_filter = VK_FILTER_LINEAR;
    sampler_creation.address_mode_u = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_creation.address_mode_v = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    scene.dummy_sampler = gpu.create_sampler( sampler_creation );

    gpu.link_image_sampler( scene.dummy_texture, scene.dummy_sampler );

    StringBuffer resource_name_buffer;
    resource_name_buffer.init( rkilo( 64 ), allocator );

    scene.samplers.init( allocator, ( u32 )gltf_data->samplers_count );

    for ( u32 sampler_index = 0; sampler_index < ( u32 )gltf_data->samplers_count; ++sampler_index ) {
        cgltf_sampler& sampler = gltf_data->samplers[ sampler_index ];

        char* sampler_name = resource_name_buffer.append_use_f( "sampler_%u", sampler_index );

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

        SamplerResource* sr = renderer.create_sampler( creation );
        RASSERT( sr != nullptr );

        scene.samplers.push( *sr );
    }

    buffers_data.init( allocator, ( u32 )gltf_data->buffers_count );

    for ( u32 buffer_index = 0; buffer_index < ( u32 )gltf_data->buffers_count; ++buffer_index ) {
        cgltf_buffer& buffer = gltf_data->buffers[ buffer_index ];

        FileReadResult buffer_data = file_read_binary( buffer.uri, allocator );
        buffers_data.push( buffer_data.data );
    }

    scene.buffers.init( allocator, ( u32 )gltf_data->buffer_views_count );

    for ( u32 buffer_index = 0; buffer_index < ( u32 )gltf_data->buffer_views_count; ++buffer_index ) {
        char* buffer_name = nullptr;
        u32 buffer_size = 0;
        u8* data = get_buffer_data( gltf_data, buffer_index, buffers_data, &buffer_size, &buffer_name );

        // NOTE(marco): the target attribute of a BufferView is not mandatory, so we prepare for both uses
        VkBufferUsageFlags flags = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;

        if ( buffer_name == nullptr ) {
            buffer_name = resource_name_buffer.append_use_f( "buffer_%u", buffer_index );
        } else {
            // NOTE(marco); some buffers might have the same name, which causes issues in the renderer cache
            buffer_name = resource_name_buffer.append_use_f( "%s_%u", buffer_name, buffer_index );
        }

        BufferHandle buffer = renderer.create_buffer_with_upload( {
            .size = buffer_size,
            .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
            .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
            .name = buffer_name
        }, {
            .data = data,
            .policy = BufferUploadPolicy::HostWrite
        } );

        scene.buffers.push( buffer );
    }

    resource_name_buffer.shutdown();

    // A node can reference a mesh containing multiple primitives. Reserve one
    // draw entry for every primitive that can be emitted by the scene traversal.
    u32 draw_capacity = 0;
    for ( u32 node_index = 0; node_index < ( u32 )gltf_data->nodes_count; ++node_index ) {
        const cgltf_node& node = gltf_data->nodes[ node_index ];
        if ( node.mesh != nullptr ) {
            draw_capacity += ( u32 )node.mesh->primitives_count;
        }
    }

    scene.mesh_draws.init( allocator, draw_capacity );
}

//
//
static bool populate_mesh_buffers( raptor::Allocator* allocator, raptor::Renderer& renderer, Scene& scene, raptor::Array<void*>& buffers_data, cgltf_primitive& mesh_primitive, MeshDraw& mesh_draw ) {
    using namespace raptor;
    cgltf_data* gltf_data = scene.gltf_scene;
    GpuDevice& gpu = *renderer.gpu;

    // Create index buffer
    cgltf_accessor& indices_accessor = *mesh_primitive.indices;
    RASSERT( indices_accessor.component_type == cgltf_component_type_r_32u ||
                indices_accessor.component_type == cgltf_component_type_r_16u );
    mesh_draw.index_type = ( indices_accessor.component_type == cgltf_component_type_r_32u ) ?
        VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;

    cgltf_buffer_view& indices_buffer_view = *indices_accessor.buffer_view;
    u32 index_buffer_index = ( u32 )cgltf_buffer_view_index( gltf_data, indices_accessor.buffer_view );
    BufferHandle indices_buffer_gpu = scene.buffers[ index_buffer_index ];
    mesh_draw.index_buffer = indices_buffer_gpu;
    mesh_draw.index_offset = ( u32 )indices_accessor.offset;
    mesh_draw.primitive_count = ( u32 )indices_accessor.count;
    RASSERT( ( mesh_draw.primitive_count % 3 ) == 0 );

    glm::vec3* position_data = nullptr;
    u32* index_data_32 = ( u32* )get_buffer_data( gltf_data, index_buffer_index, buffers_data );
    u16* index_data_16 = ( u16* )index_data_32;
    u32 vertex_count = 0;

    const cgltf_accessor* position_buffer_accessor = cgltf_find_accessor( &mesh_primitive, cgltf_attribute_type_position, 0 );
    if ( position_buffer_accessor != nullptr ) {
        cgltf_buffer_view* position_buffer_view = position_buffer_accessor->buffer_view;
        u32 position_buffer_index = ( u32 )cgltf_buffer_view_index( gltf_data, position_buffer_view );
        BufferHandle position_buffer_gpu = scene.buffers[ position_buffer_index ];

        vertex_count = ( u32 )position_buffer_accessor->count;

        mesh_draw.position_buffer = position_buffer_gpu;
        mesh_draw.position_offset = ( u32 )position_buffer_accessor->offset;

        position_data = ( glm::vec3* )get_buffer_data( gltf_data, position_buffer_index, buffers_data );
    } else {
        RASSERTM( false, "No position data found!" );
        return false;
    }

    const cgltf_accessor* normal_buffer_accessor = cgltf_find_accessor( &mesh_primitive, cgltf_attribute_type_normal, 0 );
    if ( normal_buffer_accessor != nullptr ) {
        cgltf_buffer_view* normal_buffer_view = normal_buffer_accessor->buffer_view;
        BufferHandle normal_buffer_gpu = scene.buffers[ ( u32 )cgltf_buffer_view_index( gltf_data, normal_buffer_view ) ];

        mesh_draw.normal_buffer = normal_buffer_gpu;
        mesh_draw.normal_offset = ( u32 )normal_buffer_accessor->offset;
    } else {
        // NOTE(marco): we could compute this at runtime
        Array<glm::vec3> normals_array{ };
        normals_array.init( allocator, vertex_count, vertex_count );
        memset( normals_array.data, 0, normals_array.size * sizeof( glm::vec3 ) );

        u32 index_count = mesh_draw.primitive_count;
        for ( u32 index = 0; index < index_count; index += 3 ) {
            u32 i0 = indices_accessor.component_type == cgltf_component_type_r_32u ? index_data_32[ index ] : index_data_16[ index ];
            u32 i1 = indices_accessor.component_type == cgltf_component_type_r_32u ? index_data_32[ index + 1 ] : index_data_16[ index + 1 ];
            u32 i2 = indices_accessor.component_type == cgltf_component_type_r_32u ? index_data_32[ index + 2 ] : index_data_16[ index + 2 ];

            glm::vec3 p0 = position_data[ i0 ];
            glm::vec3 p1 = position_data[ i1 ];
            glm::vec3 p2 = position_data[ i2 ];

            glm::vec3 a = p1 - p0;
            glm::vec3 b = p2 - p0;

            glm::vec3 normal = glm::cross( a, b );

            normals_array[ i0 ] = normals_array[ i0 ] + normal;
            normals_array[ i1 ] = normals_array[ i1 ] + normal;
            normals_array[ i2 ] = normals_array[ i2 ] + normal;
        }

        for ( u32 vertex = 0; vertex < vertex_count; ++vertex ) {
            normals_array[ vertex ] = glm::normalize( normals_array[ vertex ] );
        }

        const VkDeviceSize normals_buffer_size = VkDeviceSize( normals_array.size ) * sizeof( glm::vec3 );

        mesh_draw.normal_buffer = renderer.create_buffer_with_upload( {
            .size = normals_buffer_size,
            .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
            .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
            .name = "normals"
        }, {
            .data = normals_array.data,
            .policy = BufferUploadPolicy::HostWrite
        } );
        mesh_draw.normal_offset = 0;

        // custom_mesh_buffers.push( mesh_draw.normal_buffer );

        normals_array.shutdown();
    }

    const cgltf_accessor* tangent_buffer_accessor = cgltf_find_accessor( &mesh_primitive, cgltf_attribute_type_tangent, 0 );
    if ( tangent_buffer_accessor != nullptr ) {
        cgltf_buffer_view* tangent_buffer_view = tangent_buffer_accessor->buffer_view;
        BufferHandle tangent_buffer_gpu = scene.buffers[ ( u32 )cgltf_buffer_view_index( gltf_data, tangent_buffer_view ) ];

        mesh_draw.tangent_buffer = tangent_buffer_gpu;
        mesh_draw.tangent_offset = ( u32 )tangent_buffer_accessor->offset;

        mesh_draw.flags |= DrawFlags_HasTangents;
    }

    const cgltf_accessor* texcoord_buffer_accessor = cgltf_find_accessor( &mesh_primitive, cgltf_attribute_type_texcoord, 0 );
    if ( texcoord_buffer_accessor != nullptr ) {
        cgltf_buffer_view* texcoord_buffer_view = texcoord_buffer_accessor->buffer_view;
        BufferHandle texcoord_buffer_gpu = scene.buffers[ ( u32 )cgltf_buffer_view_index( gltf_data, texcoord_buffer_view ) ];

        mesh_draw.texcoord_buffer = texcoord_buffer_gpu;
        mesh_draw.texcoord_offset = ( u32 )texcoord_buffer_accessor->offset;
    }

    return true;
}

static void scene_free_gpu_resources( Scene& scene, raptor::Renderer& renderer ) {
    raptor::GpuDevice& gpu = *renderer.gpu;

    gpu.destroy_descriptor_set( scene.scene_descriptor_set );
    gpu.destroy_buffer( scene.scene_buffer );

    gpu.destroy_image_view( scene.dummy_texture_view );
    gpu.destroy_image( scene.dummy_texture );
    gpu.destroy_sampler( scene.dummy_sampler );

    scene.mesh_draws.shutdown();
}

static void scene_unload( Scene& scene ) {
    // Free scene buffers
    scene.samplers.shutdown();
    scene.images.shutdown();
    scene.buffers.shutdown();

    // NOTE(marco): we can't destroy this sooner as textures and buffers
    // hold a pointer to the names stored here
    cgltf_free( scene.gltf_scene );
}

static int mesh_draw_type_compare( const void* a, const void* b ) {
    const MeshDraw* mesh_a = ( const MeshDraw* )a;
    const MeshDraw* mesh_b = ( const MeshDraw* )b;

    if ( (u32)mesh_a->type < (u32)mesh_b->type ) {
        return -1;
    }

    if ( (u32)mesh_a->type > (u32)mesh_b->type ) {
        return  1;
    }

    return 0;
}

static bool get_mesh_material( raptor::Renderer& renderer, Scene& scene, cgltf_material& material, MeshDraw& mesh_draw ) {
    using namespace raptor;

    bool transparent = false;
    GpuDevice& gpu = *renderer.gpu;
    cgltf_data* gltf_data = scene.gltf_scene;

    if ( material.alpha_mode == cgltf_alpha_mode_mask ) {
        mesh_draw.alpha_cutoff = material.alpha_cutoff;
        mesh_draw.flags |= DrawFlags_AlphaMask;
        transparent = true;
    }

    if ( material.alpha_mode == cgltf_alpha_mode_blend ) {
        transparent = true;
    }

    if ( material.has_pbr_metallic_roughness ) {
        mesh_draw.material.base_color_factor = {
            material.pbr_metallic_roughness.base_color_factor[0],
            material.pbr_metallic_roughness.base_color_factor[1],
            material.pbr_metallic_roughness.base_color_factor[2],
            material.pbr_metallic_roughness.base_color_factor[3],
        };

        mesh_draw.base_color_factor = mesh_draw.material.base_color_factor;

        if ( material.pbr_metallic_roughness.base_color_texture.texture != nullptr ) {
            cgltf_texture& diffuse_texture = *material.pbr_metallic_roughness.base_color_texture.texture;
            u32 diffuse_texture_index = ( u32 )cgltf_image_index( gltf_data, diffuse_texture.image );
            TextureResource& diffuse_texture_gpu = scene.images[ diffuse_texture_index ];

            SamplerHandle sampler_handle = scene.dummy_sampler;
            if ( diffuse_texture.sampler != nullptr ) {
                sampler_handle = scene.samplers[ ( u32 )cgltf_sampler_index( gltf_data, diffuse_texture.sampler ) ].handle;
            }

            gpu.link_image_sampler( diffuse_texture_gpu.image, sampler_handle );

            mesh_draw.diffuse_texture_index = diffuse_texture_gpu.image_view.index();
        } else {
            mesh_draw.diffuse_texture_index = scene.dummy_texture_view.index();
        }

        if ( material.pbr_metallic_roughness.metallic_roughness_texture.texture != nullptr ) {
            cgltf_texture& roughness_texture = *material.pbr_metallic_roughness.metallic_roughness_texture.texture;
            u32 roughness_texture_index = ( u32 )cgltf_image_index( gltf_data, roughness_texture.image );
            TextureResource& roughness_texture_gpu = scene.images[ roughness_texture_index ];

            SamplerHandle sampler_handle = scene.dummy_sampler;
            if ( roughness_texture.sampler != nullptr ) {
                sampler_handle = scene.samplers[ ( u32 )cgltf_sampler_index( gltf_data, roughness_texture.sampler ) ].handle;
            }

            gpu.link_image_sampler( roughness_texture_gpu.image, sampler_handle );

            mesh_draw.roughness_texture_index = roughness_texture_gpu.image_view.index();
        } else {
            mesh_draw.roughness_texture_index = scene.dummy_texture_view.index();
        }

        mesh_draw.metallic_roughness_occlusion_factor.x = material.pbr_metallic_roughness.metallic_factor;
        mesh_draw.metallic_roughness_occlusion_factor.y = material.pbr_metallic_roughness.roughness_factor;
    }

    if ( material.occlusion_texture.texture != nullptr ) {
        cgltf_texture& occlusion_texture = *material.occlusion_texture.texture;

        // NOTE(marco): this could be the same as the roughness texture, but for now we treat it as a separate
        // texture
        u32 occlusion_texture_index = ( u32 )cgltf_image_index( gltf_data, occlusion_texture.image );
        TextureResource& occlusion_texture_gpu = scene.images[ occlusion_texture_index ];

        SamplerHandle sampler_handle = scene.dummy_sampler;
        if ( occlusion_texture.sampler != nullptr ) {
            sampler_handle = scene.samplers[ ( u32 )cgltf_sampler_index( gltf_data, occlusion_texture.sampler ) ].handle;
        }

        gpu.link_image_sampler( occlusion_texture_gpu.image, sampler_handle );

        mesh_draw.occlusion_texture_index = occlusion_texture_gpu.image_view.index();
    } else {
        mesh_draw.occlusion_texture_index = scene.dummy_texture_view.index();
    }

    mesh_draw.metallic_roughness_occlusion_factor.z = material.occlusion_texture.scale;

    mesh_draw.material.emissive_factor = glm::vec3{
        material.emissive_factor[ 0 ],
        material.emissive_factor[ 1 ],
        material.emissive_factor[ 2 ],
    };

    if ( material.emissive_texture.texture != nullptr ) {
        cgltf_texture& emissive_texture = *material.emissive_texture.texture;

        // NOTE(marco): this could be the same as the roughness texture, but for now we treat it as a separate
        // texture
        u32 emissive_texture_index = ( u32 )cgltf_image_index( gltf_data, emissive_texture.image );
        TextureResource& emissive_texture_gpu = scene.images[ emissive_texture_index ];

        SamplerHandle sampler_handle = scene.dummy_sampler;
        if ( emissive_texture.sampler != nullptr ) {
            sampler_handle = scene.samplers[ ( u32 )cgltf_sampler_index( gltf_data, emissive_texture.sampler ) ].handle;
        }

        gpu.link_image_sampler( emissive_texture_gpu.image, sampler_handle );

        mesh_draw.emissive_texture_index = emissive_texture_gpu.image_view.index();
    } else {
        mesh_draw.emissive_texture_index = scene.dummy_texture_view.index();
    }

    if ( material.normal_texture.texture != nullptr ) {
        cgltf_texture& normal_texture = *material.normal_texture.texture;
        u32 normal_texture_index = ( u32 )cgltf_image_index( gltf_data, normal_texture.image );
        TextureResource& normal_texture_gpu = scene.images[ normal_texture_index ];

        SamplerHandle sampler_handle = scene.dummy_sampler;
        if ( normal_texture.sampler != nullptr ) {
            sampler_handle = scene.samplers[ ( u32 )cgltf_sampler_index( gltf_data, normal_texture.sampler ) ].handle;
        }

        gpu.link_image_sampler( normal_texture_gpu.image, sampler_handle );

        mesh_draw.normal_texture_index = normal_texture_gpu.image_view.index();
    } else {
        mesh_draw.normal_texture_index = scene.dummy_texture_view.index();
    }

    return transparent;
}

int main( int argc, char** argv ) {

    if ( argc < 2 ) {
        printf( "Usage: chapter2 [path to glTF model]\n");
        InjectDefault3DModel();
    }

    using namespace raptor;
    // Init services
    MemoryService::instance()->init( nullptr );
    Allocator* allocator = &MemoryService::instance()->system_allocator;

    ArenaAllocator scratch_allocator;
    scratch_allocator.init( rmega( 8 ) );

    // window
    WindowConfiguration wconf{ 1280, 800, "Chapter 2: Bindless", &MemoryService::instance()->system_allocator};
    raptor::Window window;
    window.init( &wconf );

    InputService input;
    input.init( allocator );

    // Callback register: input needs to react to OS messages.
    window.register_os_messages_callback( input_os_messages_callback, &input );

    // graphics
    GpuDeviceCreation dc;
    dc.enable_bindless = true;
    dc.force_disable_mesh_shaders = true;
    dc.set_window( window.width, window.height, window.platform_handle ).set_allocator( allocator );

    dc.resource_pool_creation.buffers = 1024;

    GpuDevice gpu;
    gpu.init( dc );

    ResourceManager rm;
    rm.init( allocator, nullptr );

    GpuVisualProfiler gpu_profiler;
    gpu_profiler.init( allocator, gpu.gpu_timestamp_frequency, 100, dc.gpu_time_queries_per_frame );

    RendererResourcePoolCreation rrpc{ };
    rrpc.buffers = dc.resource_pool_creation.buffers;

    Renderer renderer;
    renderer.init( { &gpu, allocator, rrpc } );
    renderer.set_loaders( &rm );

    ImGuiService* imgui = ImGuiService::instance();
    ImGuiServiceConfiguration imgui_config{ &gpu, &renderer, window.platform_handle };
    imgui->init( &imgui_config );

    GameCamera game_camera;
    game_camera.camera.init_perpective( 0.1f, 4000.f, 60.f, wconf.width * 1.f / wconf.height );
    game_camera.init( true, 20.f, 6.f, 0.1f );

    time_service_init();

    Directory cwd{ };
    directory_current(&cwd);

    char gltf_base_path[512]{ };
    memcpy( gltf_base_path, argv[ 1 ], strlen( argv[ 1] ) );
    file_directory_from_path( gltf_base_path );

    directory_change( gltf_base_path );

    char gltf_file[512]{ };
    memcpy( gltf_file, argv[ 1 ], strlen( argv[ 1] ) );
    file_name_from_path( gltf_file );

    Scene scene;
    Array<void*> buffers_data;
    scene_load_from_gltf( gltf_file, renderer, allocator, scene, buffers_data );

    // NOTE(marco): restore working directory
    directory_change( cwd.path );

    // Pipelines
    PipelineHandle scene_pipelines[ (u32)MeshDrawType::Count ];

    ShaderStateHandle shader_state;
    PipelineLayoutHandle scene_pipeline_layout;
    {
        // Create pipeline state
        PipelineCreation pipeline_creation{ };

        ArenaScope scoped_allocator( &scratch_allocator );

        StringBuffer path_buffer;
        path_buffer.init( 1024, scoped_allocator.allocator );

        const char* vert_file = "glsl/chapter2/main.vert";
        char* vert_path = path_buffer.append_use_f("%s%s", RAPTOR_SHADER_FOLDER, vert_file );
        FileReadResult vert_code = file_read_text( vert_path, scoped_allocator.allocator );

        const char* frag_file = "glsl/chapter2/main.frag";
        char* frag_path = path_buffer.append_use_f("%s%s", RAPTOR_SHADER_FOLDER, frag_file );
        FileReadResult frag_code = file_read_text( frag_path, scoped_allocator.allocator );

        // Vertex input
        // TODO(marco): could these be inferred from SPIR-V?
        pipeline_creation.vertex_input = {
            .bindings = {
                { 0, 12, VK_VERTEX_INPUT_RATE_VERTEX },
                { 1, 16, VK_VERTEX_INPUT_RATE_VERTEX },
                { 2, 12, VK_VERTEX_INPUT_RATE_VERTEX },
                { 3,  8, VK_VERTEX_INPUT_RATE_VERTEX }
            },
            .attributes = {
                { 0, 0, VK_FORMAT_R32G32B32_SFLOAT }, // position
                { 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT }, // tangent
                { 2, 2, VK_FORMAT_R32G32B32_SFLOAT }, // normal
                { 3, 3, VK_FORMAT_R32G32_SFLOAT }  // texcoord
            }
        };

        // Depth
        pipeline_creation.depth_stencil = { .depth_comparison = VK_COMPARE_OP_LESS_OR_EQUAL, .depth_enable = true, .depth_write_enable = true };

        ShaderCompilationCreation shader_compilation_creation = {
            .stages = {
                {
                    .source_code = vert_code.data,
                    .type = VK_SHADER_STAGE_VERTEX_BIT,
                },
                {
                    .source_code = frag_code.data,
                    .type = VK_SHADER_STAGE_FRAGMENT_BIT,
                }
            },
            .name = "scene"
        };

        // Compile shader and reflect it
        ShaderReflection shader_reflection;
        shader_state = renderer.create_shader_state( shader_compilation_creation, "chapter2", &shader_reflection );

        scene_pipeline_layout = renderer.create_pipeline_layout( shader_reflection );

        // Create opaque no cull pipeline
        pipeline_creation.shader = shader_state;
        pipeline_creation.layout = scene_pipeline_layout;
        pipeline_creation.render_pass_output = gpu.get_swapchain_output();
        pipeline_creation.name = "scene_no_cull";
        scene_pipelines[ (u32)MeshDrawType::NoCull_Opaque ] = renderer.create_pipeline( shader_reflection, pipeline_creation );

        // Create opaque cull pipeline
        pipeline_creation.rasterization.cull_mode = VK_CULL_MODE_BACK_BIT;
        pipeline_creation.name = "scene_cull";
        scene_pipelines[ (u32)MeshDrawType::Cull_Opaque ] = renderer.create_pipeline( shader_reflection, pipeline_creation );

        // Add blend state for transparent pipelines
        pipeline_creation.blend_state.blend_states = {
            {
                .source_color = VK_BLEND_FACTOR_SRC_ALPHA,
                .destination_color = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                .color_operation = VK_BLEND_OP_ADD,
            }
        };

        // Create transparent no cull pipeline
        pipeline_creation.rasterization.cull_mode = VK_CULL_MODE_NONE;
        pipeline_creation.name = "scene_no_cull_transparent";
        scene_pipelines[ (u32)MeshDrawType::NoCull_Transparent ] = renderer.create_pipeline( shader_reflection, pipeline_creation );

        // Create transparent cull pipeline
        pipeline_creation.rasterization.cull_mode = VK_CULL_MODE_BACK_BIT;
        pipeline_creation.name = "scene_cull_transparent";
        scene_pipelines[ (u32)MeshDrawType::Cull_Transparent ] = renderer.create_pipeline( shader_reflection, pipeline_creation );

        cgltf_data* gltf_data = scene.gltf_scene;
        cgltf_scene* root_gltf_scene = gltf_data->scene;

        Array<i32> node_parents;
        node_parents.init( scoped_allocator.allocator, ( u32 )gltf_data->nodes_count, ( u32 )gltf_data->nodes_count );

        Array<u32> node_stack;
        node_stack.init( scoped_allocator.allocator, 8 );

        Array<glm::mat4> node_matrix;
        node_matrix.init( scoped_allocator.allocator, ( u32 )gltf_data->nodes_count, ( u32 )gltf_data->nodes_count );

        for ( u32 node_index = 0; node_index < ( u32 )root_gltf_scene->nodes_count; ++node_index ) {
            u32 root_node = ( u32 )cgltf_node_index( gltf_data, root_gltf_scene->nodes[ node_index ] );
            node_parents[ root_node ] = -1;
            node_stack.push( root_node );
        }

        while ( node_stack.size ) {
            u32 node_index = node_stack.back();
            node_stack.pop();
            cgltf_node& node = gltf_data->nodes[ node_index ];

            glm::mat4 local_matrix{ };

            if ( node.has_matrix  ) {
                // CGLM and glTF have the same matrix layout, just memcopy it
                memcpy( &local_matrix, node.matrix, sizeof( glm::mat4 ) );
            }
            else {
                glm::vec3 node_scale{ 1.0f, 1.0f, 1.0f };
                if ( node.has_scale ) {
                    node_scale = glm::vec3{ node.scale[0], node.scale[1], node.scale[2] };
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

                local_matrix = transform.calculate_matrix();
            }

            node_matrix[ node_index ] = local_matrix;

            for ( u32 child_index = 0; child_index < node.children_count; ++child_index ) {
                u32 child_node_index = ( u32 )cgltf_node_index( gltf_data, node.children[ child_index ] );
                node_parents[ child_node_index ] = node_index;
                node_stack.push( child_node_index );
            }

            if ( node.mesh == nullptr ) {
                continue;
            }

            cgltf_mesh& mesh = *node.mesh;

            glm::mat4 final_matrix = local_matrix;
            i32 node_parent = node_parents[ node_index ];
            while( node_parent != -1 ) {
                final_matrix = node_matrix[ node_parent ] * final_matrix;
                node_parent = node_parents[ node_parent ];
            }

            // Gltf primitives are conceptually submeshes.
            for ( u32 primitive_index = 0; primitive_index < mesh.primitives_count; ++primitive_index ) {
                MeshDraw mesh_draw{ };

                mesh_draw.model = final_matrix;

                cgltf_primitive& mesh_primitive = mesh.primitives[ primitive_index ];

                if ( !populate_mesh_buffers( allocator, renderer, scene, buffers_data, mesh_primitive, mesh_draw ) ) {
                    continue;
                }

                // Choose mesh draw type based on gltf material
                cgltf_material& material = *mesh_primitive.material;

                bool transparent = get_mesh_material( renderer, scene, material, mesh_draw );

                if ( transparent ) {
                    if ( material.double_sided ) {
                        mesh_draw.type = MeshDrawType::NoCull_Transparent;
                    } else {
                        mesh_draw.type = MeshDrawType::Cull_Transparent;
                    }
                } else {
                    if ( material.double_sided ) {
                        mesh_draw.type = MeshDrawType::NoCull_Opaque;
                    } else {
                        mesh_draw.type = MeshDrawType::Cull_Opaque;
                    }
                }

                scene.mesh_draws.push( mesh_draw );
            }
        }
    }

    for ( u32 buffer_index = 0; buffer_index < scene.gltf_scene->buffers_count; ++buffer_index ) {
        void* buffer = buffers_data[ buffer_index ];
        allocator->deallocate( buffer );
    }
    buffers_data.shutdown();

    qsort( scene.mesh_draws.data, scene.mesh_draws.size, sizeof( MeshDraw ), mesh_draw_type_compare );

    for ( u32 mesh_index = 0; mesh_index < scene.mesh_draws.size; ++mesh_index ) {
        scene.mesh_draws[ mesh_index ].draw_index = mesh_index;
    }

    // Scene globals and all per-draw records share one persistently mapped SSBO.
    const VkDeviceSize scene_buffer_size =
        sizeof( SceneData ) + sizeof( DrawData ) * scene.mesh_draws.size;

    scene.scene_buffer = gpu.create_buffer( {
        .size = scene_buffer_size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                            VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .name = "scene_buffer" } );

    PipelineHandle scene_no_cull = scene_pipelines[ ( u32 )MeshDrawType::NoCull_Opaque ];
    DescriptorSetLayoutHandle layout_handle =
        gpu.get_descriptor_set_layout( scene_no_cull, k_material_descriptor_set_index );
    ShaderReflectionInfo* reflection_info = renderer.get_shader_reflection( scene_no_cull );

    DescriptorSetCreation ds_creation{ };
    ds_creation.layout = layout_handle;
    ds_creation.ssbos = {
        { .buffer = scene.scene_buffer,
          .binding = renderer.get_binding_index( reflection_info, "scene" ) }
    };
    scene.scene_descriptor_set = gpu.create_descriptor_set( ds_creation );

    i64 begin_frame_tick = time_now();

    glm::vec3 light = glm::vec3{ 0.0f, 4.0f, 0.0f };

    float model_scale = 1.0f;
    float light_range = 20.0f;
    float light_intensity = 80.0f;

    while ( !window.requested_exit ) {
        ZoneScopedN("RenderLoop");

        // New frame
        if ( !window.minimized ) {
            gpu.wait_for_previous_frame();
            VkResult result = gpu.acquire_next_swapchain_image();
            if ( result == VK_ERROR_OUT_OF_DATE_KHR ) {
                gpu.resize_swapchain();
            }
            gpu.update_descriptors();
            gpu.reset_pools();
        }

        window.handle_os_messages();
        input.new_frame();

        if ( window.resized ) {
            gpu.resize( window.width, window.height );
            window.resized = false;

            game_camera.camera.set_aspect_ratio( ( f32 )window.width / ( f32 )window.height );
        }
        // This MUST be AFTER os messages!
        imgui->new_frame();

        const i64 current_tick = time_now();
        f32 delta_time = ( f32 )time_delta_seconds( begin_frame_tick, current_tick );
        begin_frame_tick = current_tick;

        input.update( delta_time );
        game_camera.update( &input, window.width, window.height, delta_time );
        window.center_mouse( game_camera.mouse_dragging );

        glm::vec3 camera_position = game_camera.camera.position;

        if ( ImGui::Begin( "Raptor ImGui" ) ) {
            ImGui::InputFloat( "Model scale", &model_scale, 0.001f );
            ImGui::SliderFloat3( "Light position", (float*)&light[0], -10.f, 10.f );
            ImGui::SliderFloat( "Light range", &light_range, 0.01f, 100.f );
            ImGui::SliderFloat( "Light intensity", &light_intensity, 0.01f, 100.f );
            ImGui::InputFloat3( "Camera position", (float*)&camera_position[0] );
            ImGui::InputFloat3( "Camera target movement", (float*)&game_camera.target_movement[0] );
        }
        ImGui::End();

        if ( ImGui::Begin( "GPU" ) ) {
            gpu_profiler.imgui_draw();
        }
        ImGui::End();

        MemoryService::instance()->imgui_draw();

        {
            // Update scene globals and every per-draw record with one mapping
            // and one flush.
            Buffer* gpu_scene_buffer = gpu.get_buffer( scene.scene_buffer );
            RASSERT( gpu_scene_buffer && gpu_scene_buffer->mapped_data );

            u8* mapped_data = ( u8* )gpu_scene_buffer->mapped_data;
            SceneData* scene_data = ( SceneData* )mapped_data;
            DrawData* draw_data = ( DrawData* )( mapped_data + sizeof( SceneData ) );

            scene_data->view_projection = game_camera.camera.view_projection;
            scene_data->eye = glm::vec4{ camera_position, 1.0f };
            scene_data->light = glm::vec4{ light.x, light.y, light.z, 1.0f };
            scene_data->light_range = light_range;
            scene_data->light_intensity = light_intensity;

            for ( u32 mesh_index = 0; mesh_index < scene.mesh_draws.size; ++mesh_index ) {
                MeshDraw& mesh_draw = scene.mesh_draws[ mesh_index ];
                upload_draw_data( draw_data[ mesh_draw.draw_index ], mesh_draw, model_scale );
            }

            const VkDeviceSize used_size =
                sizeof( SceneData ) + sizeof( DrawData ) * scene.mesh_draws.size;
            gpu.flush_buffer( scene.scene_buffer, 0, used_size );
        }

        imgui->finalize_draw_data();

        if ( !window.minimized ) {
            raptor::CommandBuffer* gfx_cb = gpu.allocate_command_buffer( 0, gpu.current_frame, CommandQueueType::Graphics );
            gfx_cb->begin();
            gpu.gpu_profiler->begin_command_buffer( gfx_cb );
            /*util_add_image_barrier( &gpu, gfx_cb->vk_command_buffer, gpu.get_current_swapchain_image(),
                                    RESOURCE_STATE_RENDER_TARGET,
                                    0, 1, false );*/
            gfx_cb->add_image_barrier(
                gpu.get_current_swapchain_image(),
                raptor::range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 ),
                ImageSyncState{
                    .stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    .access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    .layout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL
                }
            );

            gfx_cb->push_marker( "Frame" );

            Span<const ImageViewHandle> render_targets{ gpu.get_current_swapchain_image_view() };
            Span<const VkAttachmentLoadOp> load_operations{ VK_ATTACHMENT_LOAD_OP_CLEAR };
            Span<const VkClearValue> clear_values{
                VkClearValue{ .color = { 0.3f, 0.3f, 0.3f, 1.0f } },
            };
            VkClearValue depth_stencil_clear{ .depthStencil = { 1.0f, 0 } };
            gfx_cb->begin_render_pass( render_targets, load_operations, clear_values,
                                             gpu.get_current_swapchain_depth_image_view(),
                                             VK_ATTACHMENT_LOAD_OP_CLEAR, depth_stencil_clear,
                                             { }, 1, 0);

            gfx_cb->set_fullscreen_scissor();
            gfx_cb->set_fullscreen_viewport();

            gfx_cb->push_marker( "Main" );

            MeshDrawType last_mesh_draw_type = MeshDrawType::Count;
            // TODO(marco): loop by material so that we can deal with multiple passes
            for ( u32 mesh_index = 0; mesh_index < scene.mesh_draws.size; ++mesh_index ) {
                MeshDraw& mesh_draw = scene.mesh_draws[ mesh_index ];

                if ( mesh_draw.type != last_mesh_draw_type ) {

                    PipelineHandle pipeline = scene_pipelines[ (u32)mesh_draw.type ];

                    gfx_cb->bind_pipeline( pipeline );

                    last_mesh_draw_type = mesh_draw.type;
                }

                draw_mesh( renderer, gfx_cb, scene.scene_descriptor_set, mesh_draw );
            }

            gfx_cb->pop_marker();

            imgui->render( *gfx_cb, false );

            gfx_cb->end_render_pass();
            gfx_cb->pop_marker();

            gpu_profiler.update( gpu );

            gfx_cb->add_image_barrier(
                gpu.get_current_swapchain_image(),
                raptor::range_aspect( VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 ),
                ImageSyncState{
                    .stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    .access = VK_ACCESS_2_MEMORY_READ_BIT,
                    .layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                }
            );
            /*util_add_image_barrier( &gpu, gfx_cb->vk_command_buffer, gpu.get_current_swapchain_image(),
                                    RESOURCE_STATE_PRESENT,
                                    0, 1, false );*/
            gpu.update_bindless_resources();

            // Send commands to GPU
            gpu.gpu_profiler->end_command_buffer( gfx_cb );
            gfx_cb->end();

            StaticArray<CommandBuffer*, 2> cbs;
            cbs.push( gfx_cb );

            GpuSubmitSync present_sync = gpu.build_present_sync( VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT );

            gpu.queue_submit( CommandQueueType::Graphics, cbs.as_span(),
                              present_sync.waits.as_span(),
                              present_sync.signals.as_span() );

            gpu.present();
            gpu.resolve_timestamps();
            gpu.process_pending_resource_deletion();
        } else {
            ImGui::Render();
        }

        FrameMark;
    }

    imgui->shutdown();

    gpu_profiler.shutdown();

    scene_free_gpu_resources( scene, renderer );

    for ( PipelineHandle pipeline : scene_pipelines ) {
        gpu.destroy_pipeline( pipeline );
    }
    gpu.destroy_pipeline_layout( scene_pipeline_layout );
    renderer.destroy_shader_state( shader_state );

    rm.shutdown();
    renderer.shutdown();

    scene_unload( scene );

    input.shutdown();
    window.unregister_os_messages_callback( input_os_messages_callback );
    window.shutdown();

    scratch_allocator.shutdown();

    MemoryService::instance()->shutdown();

    return 0;
}
