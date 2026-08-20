
#include "application/window.hpp"
#include "application/input.hpp"
#include "application/game_camera.hpp"

#include "graphics/gpu_device.hpp"
#include "graphics/command_buffer.hpp"
#include "graphics/renderer.hpp"
#include "graphics/render_scene.hpp"
#include "graphics/raptor_imgui.hpp"
#include "graphics/gpu_profiler.hpp"
#include "graphics/shader_compiler.hpp"
#include "graphics/spirv_parser.hpp"

#include "external/glm/mat4x4.hpp"
#include "external/glm/gtc/quaternion.hpp"
#include "external/glm/matrix.hpp"

#include "external/imgui/imgui.h"

#include "external/tracy/tracy/Tracy.hpp"
#include "external/stb_image.h"
#include "external/cgltf/cgltf.h"

#include "foundation/file.hpp"
#include "foundation/resource_manager.hpp"
#include "foundation/time.hpp"

#include <stdlib.h>

///////////////////////////////////////

// Rendering resources
raptor::BufferHandle                    vb;
raptor::BufferHandle                    ib;
raptor::PipelineHandle                  pipeline;
raptor::BufferHandle                    scene_buffer;
raptor::DescriptorSetLayoutHandle       dsl;
raptor::PipelineLayoutHandle            pl;
raptor::ShaderStateHandle               shader_state;

f32 rx, ry;

const char* vs_code = R"FOO(#version 450
uint MaterialFeatures_ColorTexture     = 1 << 0;
uint MaterialFeatures_NormalTexture    = 1 << 1;
uint MaterialFeatures_RoughnessTexture = 1 << 2;
uint MaterialFeatures_OcclusionTexture = 1 << 3;
uint MaterialFeatures_EmissiveTexture =  1 << 4;
uint MaterialFeatures_TangentVertexAttribute = 1 << 5;
uint MaterialFeatures_TexcoordVertexAttribute = 1 << 6;

struct DrawData {
    vec4 base_color_factor;
    mat4 model;
    mat4 model_inv;

    vec3  emissive_factor;
    float metallic_factor;

    float roughness_factor;
    float occlusion_factor;
    uint  flags;
    uint  padding;
};

layout(std430, binding = 0) readonly buffer SceneBuffer {
    mat4 m;
    mat4 vp;
    vec4 eye;
    vec4 light;

    DrawData draws[];
} scene;

layout(location=0) in vec3 position;
layout(location=1) in vec4 tangent;
layout(location=2) in vec3 normal;
layout(location=3) in vec2 texCoord0;

layout (location = 0) out vec2 vTexcoord0;
layout (location = 1) out vec3 vNormal;
layout (location = 2) out vec4 vTangent;
layout (location = 3) out vec4 vPosition;
layout (location = 4) flat out uint vDrawIndex;

void main() {
    uint draw_index = uint( gl_InstanceIndex );
    DrawData draw_data = scene.draws[ draw_index ];

    gl_Position = scene.vp * scene.m * draw_data.model * vec4(position, 1);
    vPosition = scene.m * draw_data.model * vec4(position, 1.0);

    if ( ( draw_data.flags & MaterialFeatures_TexcoordVertexAttribute ) != 0 ) {
        vTexcoord0 = texCoord0;
    }
    vNormal = mat3( draw_data.model_inv ) * normal;

    if ( ( draw_data.flags & MaterialFeatures_TangentVertexAttribute ) != 0 ) {
        vTangent = tangent;
    }

    vDrawIndex = draw_index;
}
)FOO";

        const char* fs_code = R"FOO(#version 450
uint MaterialFeatures_ColorTexture     = 1 << 0;
uint MaterialFeatures_NormalTexture    = 1 << 1;
uint MaterialFeatures_RoughnessTexture = 1 << 2;
uint MaterialFeatures_OcclusionTexture = 1 << 3;
uint MaterialFeatures_EmissiveTexture =  1 << 4;
uint MaterialFeatures_TangentVertexAttribute = 1 << 5;
uint MaterialFeatures_TexcoordVertexAttribute = 1 << 6;

struct DrawData {
    vec4 base_color_factor;
    mat4 model;
    mat4 model_inv;

    vec3  emissive_factor;
    float metallic_factor;

    float roughness_factor;
    float occlusion_factor;
    uint  flags;
    uint  padding;
};

layout(std430, binding = 0) readonly buffer SceneBuffer {
    mat4 m;
    mat4 vp;
    vec4 eye;
    vec4 light;

    DrawData draws[];
} scene;

layout (binding = 2) uniform sampler2D diffuseTexture;
layout (binding = 3) uniform sampler2D roughnessMetalnessTexture;
layout (binding = 4) uniform sampler2D occlusionTexture;
layout (binding = 5) uniform sampler2D emissiveTexture;
layout (binding = 6) uniform sampler2D normalTexture;

layout (location = 0) in vec2 vTexcoord0;
layout (location = 1) in vec3 vNormal;
layout (location = 2) in vec4 vTangent;
layout (location = 3) in vec4 vPosition;
layout (location = 4) flat in uint vDrawIndex;

layout (location = 0) out vec4 frag_color;

#define PI 3.1415926538

vec3 decode_srgb( vec3 c ) {
    vec3 result;
    if ( c.r <= 0.04045) {
        result.r = c.r / 12.92;
    } else {
        result.r = pow( ( c.r + 0.055 ) / 1.055, 2.4 );
    }

    if ( c.g <= 0.04045) {
        result.g = c.g / 12.92;
    } else {
        result.g = pow( ( c.g + 0.055 ) / 1.055, 2.4 );
    }

    if ( c.b <= 0.04045) {
        result.b = c.b / 12.92;
    } else {
        result.b = pow( ( c.b + 0.055 ) / 1.055, 2.4 );
    }

    return clamp( result, 0.0, 1.0 );
}

vec3 encode_srgb( vec3 c ) {
    vec3 result;
    if ( c.r <= 0.0031308) {
        result.r = c.r * 12.92;
    } else {
        result.r = 1.055 * pow( c.r, 1.0 / 2.4 ) - 0.055;
    }

    if ( c.g <= 0.0031308) {
        result.g = c.g * 12.92;
    } else {
        result.g = 1.055 * pow( c.g, 1.0 / 2.4 ) - 0.055;
    }

    if ( c.b <= 0.0031308) {
        result.b = c.b * 12.92;
    } else {
        result.b = 1.055 * pow( c.b, 1.0 / 2.4 ) - 0.055;
    }

    return clamp( result, 0.0, 1.0 );
}

float heaviside( float v ) {
    if ( v > 0.0 ) return 1.0;
    else return 0.0;
}

void main() {

    DrawData draw_data = scene.draws[ vDrawIndex ];

    vec4 base_color_factor = draw_data.base_color_factor;
    vec3 emissive_factor = draw_data.emissive_factor;
    float metallic_factor = draw_data.metallic_factor;
    float roughness_factor = draw_data.roughness_factor;
    float occlusion_factor = draw_data.occlusion_factor;
    uint flags = draw_data.flags;

    mat3 TBN = mat3( 1.0 );

    if ( ( flags & MaterialFeatures_TangentVertexAttribute ) != 0 ) {
        vec3 tangent = normalize( vTangent.xyz );
        vec3 bitangent = cross( normalize( vNormal ), tangent ) * vTangent.w;

        TBN = mat3(
            tangent,
            bitangent,
            normalize( vNormal )
        );
    }
    else {
        // NOTE(marco): taken from https://community.khronos.org/t/computing-the-tangent-space-in-the-fragment-shader/52861
        vec3 Q1 = dFdx( vPosition.xyz );
        vec3 Q2 = dFdy( vPosition.xyz );
        vec2 st1 = dFdx( vTexcoord0 );
        vec2 st2 = dFdy( vTexcoord0 );

        vec3 T = normalize(  Q1 * st2.t - Q2 * st1.t );
        vec3 B = normalize( -Q1 * st2.s + Q2 * st1.s );

        // the transpose of texture-to-eye space matrix
        TBN = mat3(
            T,
            B,
            normalize( vNormal )
        );
    }

    // vec3 V = normalize(eye.xyz - vPosition.xyz);
    // vec3 L = normalize(light.xyz - vPosition.xyz);
    // vec3 N = normalize(vNormal);
    // vec3 H = normalize(L + V);

    vec3 V = normalize( scene.eye.xyz - vPosition.xyz );
    vec3 L = normalize( scene.light.xyz - vPosition.xyz );
    // NOTE(marco): normal textures are encoded to [0, 1] but need to be mapped to [-1, 1] value
    vec3 N = normalize( vNormal );
    if ( ( flags & MaterialFeatures_NormalTexture ) != 0 ) {
        N = normalize( texture(normalTexture, vTexcoord0).rgb * 2.0 - 1.0 );
        N = normalize( TBN * N );
    }
    vec3 H = normalize( L + V );

    float roughness = roughness_factor;
    float metalness = metallic_factor;

    if ( ( flags & MaterialFeatures_RoughnessTexture ) != 0 ) {
        // Red channel for occlusion value
        // Green channel contains roughness values
        // Blue channel contains metalness
        vec4 rm = texture(roughnessMetalnessTexture, vTexcoord0);

        roughness *= rm.g;
        metalness *= rm.b;
    }

    float ao = 1.0f;
    if ( ( flags & MaterialFeatures_OcclusionTexture ) != 0 ) {
        ao = texture(occlusionTexture, vTexcoord0).r;
    }

    float alpha = pow(roughness, 2.0);

    vec4 base_colour = base_color_factor;
    if ( ( flags & MaterialFeatures_ColorTexture ) != 0 ) {
        vec4 albedo = texture( diffuseTexture, vTexcoord0 );
        base_colour.rgb *= decode_srgb( albedo.rgb );
        base_colour.a *= albedo.a;
    }

    vec3 emissive = vec3( 0 );
    if ( ( flags & MaterialFeatures_EmissiveTexture ) != 0 ) {
        vec4 e = texture(emissiveTexture, vTexcoord0);

        emissive += decode_srgb( e.rgb ) * emissive_factor;
    }

    // https://www.khronos.org/registry/glTF/specs/2.0/glTF-2.0.html#specular-brdf
    float NdotH = dot(N, H);
    float alpha_squared = alpha * alpha;
    float d_denom = ( NdotH * NdotH ) * ( alpha_squared - 1.0 ) + 1.0;
    float distribution = ( alpha_squared * heaviside( NdotH ) ) / ( PI * d_denom * d_denom );

    float NdotL = clamp( dot(N, L), 0, 1 );

    if ( NdotL > 1e-5 ) {
        float NdotV = dot(N, V);
        float HdotL = dot(H, L);
        float HdotV = dot(H, V);

        float visibility = ( heaviside( HdotL ) / ( abs( NdotL ) + sqrt( alpha_squared + ( 1.0 - alpha_squared ) * ( NdotL * NdotL ) ) ) ) * ( heaviside( HdotV ) / ( abs( NdotV ) + sqrt( alpha_squared + ( 1.0 - alpha_squared ) * ( NdotV * NdotV ) ) ) );

        float specular_brdf = visibility * distribution;

        vec3 diffuse_brdf = (1 / PI) * base_colour.rgb;

        // NOTE(marco): f0 in the formula notation refers to the base colour here
        vec3 conductor_fresnel = specular_brdf * ( base_colour.rgb + ( 1.0 - base_colour.rgb ) * pow( 1.0 - abs( HdotV ), 5 ) );

        // NOTE(marco): f0 in the formula notation refers to the value derived from ior = 1.5
        float f0 = 0.04; // pow( ( 1 - ior ) / ( 1 + ior ), 2 )
        float fr = f0 + ( 1 - f0 ) * pow(1 - abs( HdotV ), 5 );
        vec3 fresnel_mix = mix( diffuse_brdf, vec3( specular_brdf ), fr );

        vec3 material_colour = mix( fresnel_mix, conductor_fresnel, metalness );

        material_colour = emissive + mix( material_colour, material_colour * ao, occlusion_factor);

        frag_color = vec4( encode_srgb( material_colour ), base_colour.a );
    } else {
        frag_color = vec4( base_colour.rgb * 0.1, base_colour.a );
    }
}
)FOO";

enum MaterialFeatures {
    MaterialFeatures_ColorTexture     = 1 << 0,
    MaterialFeatures_NormalTexture    = 1 << 1,
    MaterialFeatures_RoughnessTexture = 1 << 2,
    MaterialFeatures_OcclusionTexture = 1 << 3,
    MaterialFeatures_EmissiveTexture  = 1 << 4,

    MaterialFeatures_TangentVertexAttribute = 1 << 5,
    MaterialFeatures_TexcoordVertexAttribute = 1 << 6,
};

struct alignas( 16 ) DrawData {
    glm::vec4 base_color_factor;
    glm::mat4 model;
    glm::mat4 model_inv;

    glm::vec3 emissive_factor;
    f32   metallic_factor;

    f32   roughness_factor;
    f32   occlusion_factor;
    u32   flags;
    u32   padding;
};

static_assert( sizeof( DrawData ) == 176 );

struct MeshDraw {
    raptor::BufferHandle index_buffer;
    raptor::BufferHandle position_buffer;
    raptor::BufferHandle tangent_buffer;
    raptor::BufferHandle normal_buffer;
    raptor::BufferHandle texcoord_buffer;

    DrawData draw_data;

    u32 index_offset;
    u32 position_offset;
    u32 tangent_offset;
    u32 normal_offset;
    u32 texcoord_offset;

    u32 draw_index;

    u32 count;

    VkIndexType index_type;

    raptor::DescriptorSetHandle descriptor_set;
};

struct alignas( 16 ) SceneData {
    glm::mat4 m;
    glm::mat4 vp;
    glm::vec4 eye;
    glm::vec4 light;
};

static_assert( sizeof( SceneData ) == 160 );

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

int main( int argc, char** argv ) {

    if ( argc < 2 ) {
        printf( "Usage: chapter1 [path to glTF model]\n");
        InjectDefault3DModel();
    }

    using namespace raptor;

    time_service_init();

    // Init services
    MemoryServiceConfiguration memory_configuration;
    memory_configuration.maximum_dynamic_size = rgiga( 2ull );

    MemoryService::instance()->init( nullptr );
    time_service_init();

    Allocator* allocator = &MemoryService::instance()->system_allocator;

    ArenaAllocator scratch_allocator;
    scratch_allocator.init( rmega( 8 ) );

    // window
    WindowConfiguration wconf{ 1280, 800, "Chapter 1: Introduction", allocator };
    raptor::Window window;
    window.init( &wconf );

    InputService input_handler;
    input_handler.init( allocator );

    // Callback register
    window.register_os_messages_callback( input_os_messages_callback, &input_handler );

    // graphics
    GpuDeviceCreation dc;
    dc.resource_pool_creation.buffers = 1024;
    dc.debug_options.set_default();
    dc.set_window( window.width, window.height, window.platform_handle ).set_allocator( allocator );
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
    game_camera.camera.init_perpective( 0.1f, 100.f, 60.f, wconf.width * 1.f / wconf.height );
    game_camera.init( true, 20.f, 6.f, 0.1f );

    Directory cwd{ };
    directory_current( &cwd );

    char gltf_base_path[512]{ };
    memcpy( gltf_base_path, argv[ 1 ], strlen( argv[ 1] ) );
    file_directory_from_path( gltf_base_path );

    directory_change( gltf_base_path );

    char filename[512]{ };
    memcpy( filename, argv[ 1 ], strlen( argv[ 1] ) );
    file_name_from_path( filename );

    cgltf_options options = { };
    cgltf_data* gltf_data = nullptr;
    cgltf_result result = cgltf_parse_file( &options, filename, &gltf_data );
    if ( result != cgltf_result_success ) {
        rprint( "Failed to open gltf file %s\n", filename );
        exit( -1 );
    }

    Array<TextureResource> images;
    images.init( allocator, ( u32 )gltf_data->images_count, ( u32 )gltf_data->images_count );

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

        images[ image_index ] = *tr;
    }

    ImageCreation texture_creation{ };
    u32 zero_value = 0;
    texture_creation.set_name( "dummy_texture" ).set_size( 1, 1, 1 ).set_format_type( VK_FORMAT_R8G8B8A8_UNORM, TextureType::Texture2D ).set_flags( 1 ).set_data( &zero_value );
    ImageHandle dummy_texture = gpu.create_image( texture_creation );
    ImageViewHandle dummy_texture_view = gpu.create_image_view( {
        .parent_image = dummy_texture, .view_type = VK_IMAGE_VIEW_TYPE_2D,
        .sub_resource ={ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }, .name = "dummy_texture_view" } );

    SamplerCreation sampler_creation{ };
    sampler_creation.min_filter = VK_FILTER_LINEAR;
    sampler_creation.mag_filter = VK_FILTER_LINEAR;
    sampler_creation.address_mode_u = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_creation.address_mode_v = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    SamplerHandle dummy_sampler = gpu.create_sampler( sampler_creation );

    gpu.link_image_sampler( dummy_texture, dummy_sampler );

    StringBuffer resource_name_buffer;
    resource_name_buffer.init( rkilo( 64 ), allocator );

    Array<SamplerResource> samplers;
    samplers.init( allocator, ( u32 )gltf_data->samplers_count );

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

        samplers.push( *sr );
    }

    Array<void*> buffers_data;
    buffers_data.init( allocator, ( u32 )gltf_data->buffers_count );

    for ( u32 buffer_index = 0; buffer_index < ( u32 )gltf_data->buffers_count; ++buffer_index ) {
        cgltf_buffer& buffer = gltf_data->buffers[ buffer_index ];

        FileReadResult buffer_data = file_read_binary( buffer.uri, allocator );
        buffers_data.push( buffer_data.data );
    }

    Array<BufferHandle> buffers;
    buffers.init( allocator, ( u32 )gltf_data->buffer_views_count );

    for ( u32 buffer_index = 0; buffer_index < ( u32 )gltf_data->buffer_views_count; ++buffer_index ) {
        char* buffer_name = nullptr;
        u32 buffer_size = 0;
        u8* data = get_buffer_data( gltf_data, buffer_index, buffers_data, &buffer_size, &buffer_name );

        if ( buffer_name == nullptr ) {
            buffer_name = resource_name_buffer.append_use_f( "buffer_%u", buffer_index );
        } else {
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

        RASSERT( buffer.is_valid() );
        buffers.push( buffer );
    }

    // NOTE(marco): restore working directory
    directory_change( cwd.path );

    u32 draw_capacity = 0;
    for ( u32 node_index = 0; node_index < ( u32 )gltf_data->nodes_count; ++node_index ) {
        const cgltf_node& node = gltf_data->nodes[ node_index ];
        if ( node.mesh ) {
            draw_capacity += ( u32 )node.mesh->primitives_count;
        }
    }

    Array<MeshDraw> mesh_draws;
    mesh_draws.init( allocator, draw_capacity );

    Array<BufferHandle> custom_mesh_buffers{ };
    custom_mesh_buffers.init( allocator, 8 );

    glm::vec4 dummy_data[ 3 ]{};
    BufferHandle dummy_attribute_buffer = renderer.create_buffer_with_upload( {
        .size = sizeof( dummy_data ),
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
        .name = "dummy_attribute_buffer"
    }, {
        .data = dummy_data,
        .policy = BufferUploadPolicy::HostWrite
    } );

    {
        // Create pipeline state
        PipelineCreation pipeline_creation{ };

        // Vertex input
        // TODO(marco): component format should be based on buffer view type
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

        // Render pass
        pipeline_creation.render_pass_output = gpu.get_swapchain_output();
        // Depth
        pipeline_creation.depth_stencil = { .depth_comparison = VK_COMPARE_OP_LESS_OR_EQUAL, .depth_enable = true, .depth_write_enable = true };

        // Shader state
         ShaderCompilationCreation shader_compilation_creation = {
            .stages = {
                {
                    .source_code = vs_code,
                    .type = VK_SHADER_STAGE_VERTEX_BIT,
                },
                {
                    .source_code = fs_code,
                    .type = VK_SHADER_STAGE_FRAGMENT_BIT,
                }
            },
            .name = "default_shader"
        };

        ShaderStateCreation shader_stage_creation;
        shader_stage_creation.names_buffer = &resource_name_buffer;

        StringBuffer shader_code_buffer;
        shader_code_buffer.init( rmega( 3 ), &scratch_allocator );

        // Parse and accumulate all bindings from all stages
        spirv::ParseResult parse_result = {};

        for ( u32 s = 0; s < shader_compilation_creation.stages.size; ++s ) {
            ShaderCompilationStage& shader_compilation_stage = shader_compilation_creation.stages[ s ];

            VkShaderModuleCreateInfo shader_stage = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            Span<const u32> spirv_bytecode;
            bool shader_changed = false; // TODO(marco): ignored for now
            bool result = ShaderCompiler::compile_and_cache_shader( shader_compilation_stage, spirv_bytecode, renderer.resource_cache.binary_data_folder,
                                                                    &scratch_allocator, "default_technique",
                                                                    shader_compilation_creation.name.data, nullptr, nullptr, false,
                                                                    shader_compilation_creation.slang_input, false, shader_changed );
            if ( !result ) {
                RASSERTM( false, "Failed to compile shader stage %s", shader_compilation_creation.name );
                exit( -1 );
            }

            spirv::parse_binary( spirv_bytecode.data, spirv_bytecode.size, shader_code_buffer, *shader_stage_creation.names_buffer, &parse_result );

            shader_stage.pCode = spirv_bytecode.data;
            shader_stage.codeSize = spirv_bytecode.size;

            shader_stage_creation.add_stage( shader_stage, shader_compilation_stage.type );
        }

        shader_state = gpu.create_shader_state( shader_stage_creation );
        pipeline_creation.shader = shader_state;

        pipeline_creation.rasterization.cull_mode = VK_CULL_MODE_BACK_BIT;

        // Descriptor set layout
        DescriptorSetLayoutCreation descriptor_set_layout_creation{
            .bindings = {
                { 0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 }, // SceneBuffer
                { 2,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 }, // diffuseTexture
                { 3,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 }, // roughnessMetalnessTexture
                { 4,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 }, // occlusionTexture
                { 5,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 }, // emissiveTexture
                { 6,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 }, // normalTexture
            },
            .name = "default_descriptor_set_layout"
        };
        // Setting it into pipeline
        dsl = gpu.create_descriptor_set_layout( descriptor_set_layout_creation );

        PipelineLayoutCreation pipeline_layout_creation = {
            .layouts = { dsl },
            .name = "default_pipeline_layout"
        };
        pl = gpu.create_pipeline_layout( pipeline_layout_creation );

        pipeline_creation.layout = pl;

        const VkDeviceSize draw_data_offset = sizeof( SceneData );
        const VkDeviceSize scene_buffer_size = draw_data_offset + sizeof( DrawData ) * draw_capacity;

        scene_buffer = gpu.create_buffer( {
            .size = scene_buffer_size,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
            .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .name = "scene_buffer" } );

        pipeline = gpu.create_pipeline( pipeline_creation );

        cgltf_scene* root_gltf_scene = gltf_data->scene;

        Array<i32> node_parents;
        node_parents.init( allocator, ( u32 )gltf_data->nodes_count, ( u32 )gltf_data->nodes_count );

        Array<u32> node_stack;
        node_stack.init( allocator, 8 );

        Array<glm::mat4> node_matrix;
        node_matrix.init( allocator, ( u32 )gltf_data->nodes_count, ( u32 )gltf_data->nodes_count );

        for ( u32 node_index = 0; node_index < ( u32 )gltf_data->nodes_count; ++node_index ) {
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
                glm::quat node_rotation = glm::quat{ 1.f, 0.f, 0.f, 0.f };
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

            // TODO(marco): refactor texture binding code
            for ( u32 primitive_index = 0; primitive_index < mesh.primitives_count; ++primitive_index ) {
                MeshDraw mesh_draw{ };

                mesh_draw.draw_data.model = final_matrix;

                cgltf_primitive& mesh_primitive = mesh.primitives[ primitive_index ];

                cgltf_accessor& indices_accessor = *mesh_primitive.indices;
                RASSERT( indices_accessor.component_type == cgltf_component_type_r_32u ||
                         indices_accessor.component_type == cgltf_component_type_r_16u );
                mesh_draw.index_type = ( indices_accessor.component_type == cgltf_component_type_r_32u ) ?
                    VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;

                cgltf_buffer_view& indices_buffer_view = *indices_accessor.buffer_view;
                u32 index_buffer_index = ( u32 )cgltf_buffer_view_index( gltf_data, indices_accessor.buffer_view );
                BufferHandle indices_buffer_gpu = buffers[ index_buffer_index ];
                mesh_draw.index_buffer = indices_buffer_gpu;
                mesh_draw.index_offset = ( u32 )indices_accessor.offset;
                mesh_draw.count = ( u32 )indices_accessor.count;
                RASSERT( ( mesh_draw.count % 3 ) == 0 );

                glm::vec3* position_data = nullptr;
                u32* index_data_32 = ( u32* )get_buffer_data( gltf_data, index_buffer_index, buffers_data );
                u16* index_data_16 = ( u16* )index_data_32;
                u32 vertex_count = 0;

                const cgltf_accessor* position_buffer_accessor = cgltf_find_accessor( &mesh_primitive, cgltf_attribute_type_position, 0 );
                if ( position_buffer_accessor != nullptr ) {
                    cgltf_buffer_view* position_buffer_view = position_buffer_accessor->buffer_view;
                    u32 position_buffer_index = ( u32 )cgltf_buffer_view_index( gltf_data, position_buffer_view );
                    BufferHandle position_buffer_gpu = buffers[ position_buffer_index ];

                    vertex_count = ( u32 )position_buffer_accessor->count;

                    mesh_draw.position_buffer = position_buffer_gpu;
                    mesh_draw.position_offset = ( u32 )position_buffer_accessor->offset;

                    position_data = ( glm::vec3* )get_buffer_data( gltf_data, position_buffer_index, buffers_data );
                } else {
                    RASSERTM( false, "No position data found!" );
                    continue;
                }

                const cgltf_accessor* normal_buffer_accessor = cgltf_find_accessor( &mesh_primitive, cgltf_attribute_type_normal, 0 );
                if ( normal_buffer_accessor != nullptr ) {
                    cgltf_buffer_view* normal_buffer_view = normal_buffer_accessor->buffer_view;
                    BufferHandle normal_buffer_gpu = buffers[ ( u32 )cgltf_buffer_view_index( gltf_data, normal_buffer_view ) ];

                    mesh_draw.normal_buffer = normal_buffer_gpu;
                    mesh_draw.normal_offset = ( u32 )normal_buffer_accessor->offset;
                } else {
                    // NOTE(marco): we could compute this at runtime
                    Array<glm::vec3> normals_array{ };
                    normals_array.init( allocator, vertex_count, vertex_count );
                    memset( normals_array.data, 0, normals_array.size * sizeof( glm::vec3 ) );

                    u32 index_count = mesh_draw.count;
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
                    custom_mesh_buffers.push( mesh_draw.normal_buffer );

                    normals_array.shutdown();
                }

                const cgltf_accessor* tangent_buffer_accessor = cgltf_find_accessor( &mesh_primitive, cgltf_attribute_type_tangent, 0 );
                if ( tangent_buffer_accessor != nullptr ) {
                    cgltf_buffer_view* tangent_buffer_view = tangent_buffer_accessor->buffer_view;
                    BufferHandle tangent_buffer_gpu = buffers[ ( u32 )cgltf_buffer_view_index( gltf_data, tangent_buffer_view ) ];

                    mesh_draw.tangent_buffer = tangent_buffer_gpu;
                    mesh_draw.tangent_offset = ( u32 )tangent_buffer_accessor->offset;

                    mesh_draw.draw_data.flags |= MaterialFeatures_TangentVertexAttribute;
                }

                const cgltf_accessor* texcoord_buffer_accessor = cgltf_find_accessor( &mesh_primitive, cgltf_attribute_type_texcoord, 0 );
                if ( texcoord_buffer_accessor != nullptr ) {
                    cgltf_buffer_view* texcoord_buffer_view = texcoord_buffer_accessor->buffer_view;
                    BufferHandle texcoord_buffer_gpu = buffers[ ( u32 )cgltf_buffer_view_index( gltf_data, texcoord_buffer_view ) ];

                    mesh_draw.texcoord_buffer = texcoord_buffer_gpu;
                    mesh_draw.texcoord_offset = ( u32 )texcoord_buffer_accessor->offset;

                    mesh_draw.draw_data.flags |= MaterialFeatures_TexcoordVertexAttribute;
                }

                RASSERTM( mesh_primitive.material != nullptr, "Mesh with no material is not supported!" );
                cgltf_material& material = *mesh_primitive.material;

                // Descriptor Set
                DescriptorSetCreation ds_creation{};
                ds_creation.layout = dsl;

                TextureDescriptor set_textures[ 5 ]{ };

                ds_creation.ssbos = {
                    { .buffer = scene_buffer, .binding = 0 }
                };

                if ( material.has_pbr_metallic_roughness ) {
                    mesh_draw.draw_data.base_color_factor = {
                        material.pbr_metallic_roughness.base_color_factor[0],
                        material.pbr_metallic_roughness.base_color_factor[1],
                        material.pbr_metallic_roughness.base_color_factor[2],
                        material.pbr_metallic_roughness.base_color_factor[3],
                    };

                    if ( material.pbr_metallic_roughness.base_color_texture.texture != nullptr ) {
                        cgltf_texture& diffuse_texture = *material.pbr_metallic_roughness.base_color_texture.texture;
                        u32 diffuse_texture_index = ( u32 )cgltf_image_index( gltf_data, diffuse_texture.image );
                        TextureResource& diffuse_texture_gpu = images[ diffuse_texture_index ];

                        SamplerHandle sampler_handle = dummy_sampler;
                        if ( diffuse_texture.sampler != nullptr ) {
                            sampler_handle = samplers[ ( u32 )cgltf_sampler_index( gltf_data, diffuse_texture.sampler ) ].handle;
                        }

                        gpu.link_image_sampler( diffuse_texture_gpu.image, sampler_handle );

                        set_textures[ 0 ] = { .texture = diffuse_texture_gpu.image_view, .binding = 2 };

                        mesh_draw.draw_data.flags |= MaterialFeatures_ColorTexture;
                    } else {
                        set_textures[ 0 ] = { .texture = dummy_texture_view, .binding = 2 };
                    }

                    if ( material.pbr_metallic_roughness.metallic_roughness_texture.texture != nullptr ) {
                        cgltf_texture& roughness_texture = *material.pbr_metallic_roughness.metallic_roughness_texture.texture;
                        u32 roughness_texture_index = ( u32 )cgltf_image_index( gltf_data, roughness_texture.image );
                        TextureResource& roughness_texture_gpu = images[ roughness_texture_index ];

                        SamplerHandle sampler_handle = dummy_sampler;
                        if ( roughness_texture.sampler != nullptr ) {
                            sampler_handle = samplers[ ( u32 )cgltf_sampler_index( gltf_data, roughness_texture.sampler ) ].handle;
                        }

                        gpu.link_image_sampler( roughness_texture_gpu.image, sampler_handle );

                        set_textures[ 1 ] = { .texture = roughness_texture_gpu.image_view, .binding = 3 };

                        mesh_draw.draw_data.flags |= MaterialFeatures_RoughnessTexture;
                    } else {
                        set_textures[ 1 ] = { .texture = dummy_texture_view, .binding = 3 };
                    }

                    mesh_draw.draw_data.metallic_factor = material.pbr_metallic_roughness.metallic_factor;
                    mesh_draw.draw_data.roughness_factor = material.pbr_metallic_roughness.roughness_factor;
                }

                if ( material.occlusion_texture.texture != nullptr ) {
                    cgltf_texture& occlusion_texture = *material.occlusion_texture.texture;

                    // NOTE(marco): this could be the same as the roughness texture, but for now we treat it as a separate
                    // texture
                    u32 occlusion_texture_index = ( u32 )cgltf_image_index( gltf_data, occlusion_texture.image );
                    TextureResource& occlusion_texture_gpu = images[ occlusion_texture_index ];

                    SamplerHandle sampler_handle = dummy_sampler;
                    if ( occlusion_texture.sampler != nullptr ) {
                        sampler_handle = samplers[ ( u32 )cgltf_sampler_index( gltf_data, occlusion_texture.sampler ) ].handle;
                    }

                    gpu.link_image_sampler( occlusion_texture_gpu.image, sampler_handle );

                    set_textures[ 2 ] = { .texture = occlusion_texture_gpu.image_view, .binding = 4 };

                    mesh_draw.draw_data.occlusion_factor = material.occlusion_texture.scale;
                    mesh_draw.draw_data.flags |= MaterialFeatures_OcclusionTexture;
                } else {
                    mesh_draw.draw_data.occlusion_factor = 1.0f;
                    set_textures[ 2 ] = { .texture = dummy_texture_view, .binding = 4 };
                }

                mesh_draw.draw_data.emissive_factor = glm::vec3{
                    material.emissive_factor[ 0 ],
                    material.emissive_factor[ 1 ],
                    material.emissive_factor[ 2 ],
                };

                if ( material.emissive_texture.texture != nullptr ) {
                    cgltf_texture& emissive_texture = *material.emissive_texture.texture;

                    // NOTE(marco): this could be the same as the roughness texture, but for now we treat it as a separate
                    // texture
                    u32 emissive_texture_index = ( u32 )cgltf_image_index( gltf_data, emissive_texture.image );
                    TextureResource& emissive_texture_gpu = images[ emissive_texture_index ];

                    SamplerHandle sampler_handle = dummy_sampler;
                    if ( emissive_texture.sampler != nullptr ) {
                        sampler_handle = samplers[ ( u32 )cgltf_sampler_index( gltf_data, emissive_texture.sampler ) ].handle;
                    }

                    gpu.link_image_sampler( emissive_texture_gpu.image, sampler_handle );

                    set_textures[ 3 ] = { .texture = emissive_texture_gpu.image_view, .binding = 5 };

                    mesh_draw.draw_data.flags |= MaterialFeatures_EmissiveTexture;
                } else {
                    set_textures[ 3 ] = { .texture = dummy_texture_view, .binding = 5 };
                }

                if ( material.normal_texture.texture != nullptr ) {
                    cgltf_texture& normal_texture = *material.normal_texture.texture;
                    u32 normal_texture_index = ( u32 )cgltf_image_index( gltf_data, normal_texture.image );
                    TextureResource& normal_texture_gpu = images[ normal_texture_index ];

                    SamplerHandle sampler_handle = dummy_sampler;
                    if ( normal_texture.sampler != nullptr ) {
                        sampler_handle = samplers[ ( u32 )cgltf_sampler_index( gltf_data, normal_texture.sampler ) ].handle;
                    }

                    gpu.link_image_sampler( normal_texture_gpu.image, sampler_handle );

                    set_textures[ 4 ] = { .texture = normal_texture_gpu.image_view, .binding = 6 };

                    mesh_draw.draw_data.flags |= MaterialFeatures_NormalTexture;
                } else {
                    set_textures[ 4 ] = { .texture = dummy_texture_view, .binding = 6 };
                }

                ds_creation.textures = {
                    set_textures[0],
                    set_textures[1],
                    set_textures[2],
                    set_textures[3],
                    set_textures[4],
                };

                mesh_draw.descriptor_set = gpu.create_descriptor_set( ds_creation );

                mesh_draw.draw_index = mesh_draws.size;
                mesh_draws.push( mesh_draw );
            }
        }

        node_parents.shutdown();
        node_stack.shutdown();
        node_matrix.shutdown();

        rx = 0.0f;
        ry = 0.0f;
    }

    for ( u32 buffer_index = 0; buffer_index < gltf_data->buffers_count; ++buffer_index ) {
        void* buffer = buffers_data[ buffer_index ];
        allocator->deallocate( buffer );
    }
    buffers_data.shutdown();

    i64 begin_frame_tick = time_now();

    f32 yaw = 0.0f;
    f32 pitch = 0.0f;

    float model_scale = 1.0f;

    while ( !window.requested_exit ) {
        ZoneScoped;

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

        if ( window.resized ) {
            gpu.resize( window.width, window.height );
            window.resized = false;

            game_camera.camera.set_aspect_ratio( window.width * 1.f / window.height );
        }
        // This MUST be AFTER os messages!
        imgui->new_frame();
        input_handler.new_frame();

        const i64 current_tick = time_now();
        f32 delta_time = ( f32 )time_delta_seconds( begin_frame_tick, current_tick );
        begin_frame_tick = current_tick;

        input_handler.update( delta_time );
        game_camera.update( &input_handler, window.width, window.height, delta_time );
        window.center_mouse( game_camera.mouse_dragging );

        game_camera.camera.set_zoom( 1.0f );
        game_camera.camera.update();

        if ( ImGui::Begin( "Raptor ImGui" ) ) {
            ImGui::InputFloat("Model scale", &model_scale, 0.001f);
        }
        ImGui::End();

        if ( ImGui::Begin( "GPU" ) ) {
            gpu_profiler.imgui_draw();
        }
        ImGui::End();

        glm::mat4 global_model = { };
        {
            // Update scene and per-draw data in a single storage buffer.
            Buffer* gpu_scene_buffer = gpu.get_buffer( scene_buffer );
            RASSERT( gpu_scene_buffer && gpu_scene_buffer->mapped_data );
            u8* mapped_data = ( u8* )gpu_scene_buffer->mapped_data;
            if ( mapped_data ) {
                // if ( input_handler.is_mouse_down( MouseButtons::MOUSE_BUTTONS_LEFT ) && !ImGui::GetIO().WantCaptureMouse) {
                //     pitch += ( input_handler.mouse_position.y - input_handler.previous_mouse_position.y ) * 0.1f;
                //     yaw += ( input_handler.mouse_position.x - input_handler.previous_mouse_position.x ) * 0.3f;

                //     pitch = clamp( pitch, -60.0f, 60.0f );

                //     if ( yaw > 360.0f ) {
                //         yaw -= 360.0f;
                //     }

                //     mat3s rxm = glms_mat4_pick3( glms_rotate_make( glm_rad( -pitch ), glm::vec3{ 1.0f, 0.0f, 0.0f } ) );
                //     mat3s rym = glms_mat4_pick3( glms_rotate_make( glm_rad( -yaw ), glm::vec3{ 0.0f, 1.0f, 0.0f } ) );

                //     look = glms_mat3_mulv( rxm, glm::vec3{ 0.0f, 0.0f, -1.0f } );
                //     look = glms_mat3_mulv( rym, look );

                //     right = glms_cross( look, glm::vec3{ 0.0f, 1.0f, 0.0f });
                // }

                // if ( input_handler.is_key_down( Keys::KEY_W ) ) {
                //     eye = glms_vec3_add( eye, glms_vec3_scale( look, 5.0f * delta_time ) );
                // } else if ( input_handler.is_key_down( Keys::KEY_S ) ) {
                //     eye = glms_vec3_sub( eye, glms_vec3_scale( look, 5.0f * delta_time ) );
                // }

                // if ( input_handler.is_key_down( Keys::KEY_D ) ) {
                //     eye = glms_vec3_add( eye, glms_vec3_scale( right, 5.0f * delta_time ) );
                // } else if ( input_handler.is_key_down( Keys::KEY_A ) ) {
                //     eye = glms_vec3_sub( eye, glms_vec3_scale( right, 5.0f * delta_time ) );
                // }

                glm::mat4 view = game_camera.camera.view;
                glm::mat4 projection = game_camera.camera.projection;

                // Calculate view projection matrix
                glm::mat4 view_projection = game_camera.camera.view_projection;

                // Rotate model
                rx += 1.0f * delta_time;
                ry += 2.0f * delta_time;

                glm::mat4 rxm = glm::rotate( glm::mat4( 1.0f ), rx, glm::vec3{ 1.0f, 0.0f, 0.0f } );
                glm::mat4 rym = glm::rotate( glm::mat4( 1.0f ), glm::radians( 45.0f ), glm::vec3{ 0.0f, 1.0f, 0.0f } );

                glm::mat4 sm = glm::scale( glm::mat4( 1.0f ), glm::vec3{ model_scale, model_scale, model_scale } );
                global_model = rym * sm;

                SceneData* scene_data = ( SceneData* )mapped_data;
                scene_data->vp = view_projection;
                scene_data->m = global_model;
                scene_data->eye = glm::vec4{ game_camera.camera.position, 1.0f };
                scene_data->light = glm::vec4{ 2.0f, 2.0f, 0.0f, 1.0f };

                DrawData* draw_data = ( DrawData* )( mapped_data + sizeof( SceneData ) );
                for ( u32 mesh_index = 0; mesh_index < mesh_draws.size; ++mesh_index ) {
                    MeshDraw& mesh_draw = mesh_draws[ mesh_index ];
                    mesh_draw.draw_data.model_inv = glm::inverse( glm::transpose( global_model * mesh_draw.draw_data.model ) );
                    draw_data[ mesh_draw.draw_index ] = mesh_draw.draw_data;
                }

                const VkDeviceSize used_size = sizeof( SceneData ) + sizeof( DrawData ) * mesh_draws.size;
                gpu.flush_buffer( scene_buffer, 0, used_size );
            }
        }

        imgui->finalize_draw_data();

        if ( !window.minimized ) {
            raptor::CommandBuffer* gfx_cb = gpu.allocate_command_buffer( 0, gpu.current_frame, CommandQueueType::Graphics );
            gfx_cb->begin();
            gpu.gpu_profiler->begin_command_buffer( gfx_cb );

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

            gfx_cb->bind_pipeline( pipeline );
            gfx_cb->set_fullscreen_scissor();
            gfx_cb->set_fullscreen_viewport();
            gfx_cb->set_depth_bias_enabled( false );

            gfx_cb->push_marker( "main" );

            for ( u32 mesh_index = 0; mesh_index < mesh_draws.size; ++mesh_index ) {
                MeshDraw& mesh_draw = mesh_draws[ mesh_index ];

                gfx_cb->bind_vertex_buffer( mesh_draw.position_buffer, 0, mesh_draw.position_offset );
                gfx_cb->bind_vertex_buffer( mesh_draw.normal_buffer, 2, mesh_draw.normal_offset );

                if ( mesh_draw.draw_data.flags & MaterialFeatures_TangentVertexAttribute ) {
                    gfx_cb->bind_vertex_buffer( mesh_draw.tangent_buffer, 1, mesh_draw.tangent_offset );
                } else {
                    gfx_cb->bind_vertex_buffer( dummy_attribute_buffer, 1, 0 );
                }

                if ( mesh_draw.draw_data.flags & MaterialFeatures_TexcoordVertexAttribute ) {
                    gfx_cb->bind_vertex_buffer( mesh_draw.texcoord_buffer, 3, mesh_draw.texcoord_offset );
                } else {
                    gfx_cb->bind_vertex_buffer( dummy_attribute_buffer, 3, 0 );
                }

                gfx_cb->bind_index_buffer( mesh_draw.index_buffer, mesh_draw.index_offset, mesh_draw.index_type );
                gfx_cb->bind_descriptor_set( { mesh_draw.descriptor_set }, { } );

                gfx_cb->draw_indexed( TopologyType::Triangle, mesh_draw.count, 1, 0, 0, mesh_draw.draw_index );
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

            // Send commands to GPU
            gpu.gpu_profiler->end_command_buffer( gfx_cb );
            gfx_cb->end();

            // Collect all command buffers for submission
            StaticArray<CommandBuffer*, 2> cbs;
            cbs.push( gfx_cb );

            GpuSubmitSync present_sync = gpu.build_present_sync( VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT );

            gpu.queue_submit( CommandQueueType::Graphics, cbs.as_span(),
                              present_sync.waits.as_span(),
                              present_sync.signals.as_span() );

            gpu.present();
            gpu.resolve_timestamps();

        } else {
            ImGui::Render();
        }

        FrameMark;
    }

    for ( u32 mesh_index = 0; mesh_index < mesh_draws.size; ++mesh_index ) {
        MeshDraw& mesh_draw = mesh_draws[ mesh_index ];
        gpu.destroy_descriptor_set( mesh_draw.descriptor_set );
    }

    for ( u32 mi = 0; mi < custom_mesh_buffers.size; ++mi ) {
        gpu.destroy_buffer( custom_mesh_buffers[ mi ] );
    }
    custom_mesh_buffers.shutdown();

    gpu.destroy_buffer( dummy_attribute_buffer );

    gpu.destroy_image( dummy_texture );
    gpu.destroy_image_view( dummy_texture_view );
    gpu.destroy_sampler( dummy_sampler );

    mesh_draws.shutdown();

    gpu.destroy_buffer( scene_buffer );
    gpu.destroy_shader_state( shader_state );
    gpu.destroy_pipeline( pipeline );
    gpu.destroy_descriptor_set_layout( dsl );
    gpu.destroy_pipeline_layout( pl );

    imgui->shutdown();

    gpu_profiler.shutdown();

    rm.shutdown();
    renderer.shutdown();

    samplers.shutdown();
    images.shutdown();
    buffers.shutdown();

    resource_name_buffer.shutdown();

    // NOTE(marco): we can't destroy this sooner as textures and buffers
    // hold a pointer to the names stored here
    cgltf_free( gltf_data );

    input_handler.shutdown();
    window.unregister_os_messages_callback( input_os_messages_callback );
    window.shutdown();

    scratch_allocator.shutdown();

    MemoryService::instance()->shutdown();

    return 0;
}
