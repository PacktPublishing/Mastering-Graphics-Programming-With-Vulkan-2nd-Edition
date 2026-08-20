
#include "raptor_imgui.hpp"

#include "foundation/hash_map.hpp"
#include "foundation/memory.hpp"

#include "graphics/gpu_resources.hpp"
#include "graphics/gpu_device.hpp"
#include "graphics/gpu_profiler.hpp"
#include "graphics/renderer.hpp"
#include "graphics/command_buffer.hpp"
#include "graphics/shader_compiler.hpp"

#include "external/imgui/imgui.h"
#include "external/imgui/imgui_impl_sdl3.h"

#include <SDL3/SDL.h>

#include <stdio.h>

namespace raptor {

// Graphics Data
static raptor::ImageHandle g_font_image;
static raptor::ImageViewHandle g_font_image_view;
static raptor::PipelineHandle g_imgui_pipeline;
static BufferHandle g_vb[ k_max_frames ];
static BufferHandle g_ib[ k_max_frames ];
static raptor::DescriptorSetLayoutHandle g_descriptor_set_layout;
static raptor::PipelineLayoutHandle g_pipeline_layout;
static raptor::DescriptorSetHandle g_ui_descriptor_set;  // Font descriptor set
static bool s_use_slang_shaders = true;

static uint32_t g_vb_size = rmega(1), g_ib_size = rmega(1);

raptor::FlatHashMap<raptor::ResourceHandle, raptor::DescriptorSetHandle> g_image_view_to_descriptor_set;

// Standard, non-bindless code ////////////////////////////////////////////
static const char* g_vertex_shader_code = {
    "#version 450\n"
    "layout( location = 0 ) in vec2 Position;\n"
    "layout( location = 1 ) in vec2 UV;\n"
    "layout( location = 2 ) in vec4 Color;\n"
    "layout( location = 0 ) out vec2 Frag_UV;\n"
    "layout( location = 1 ) out vec4 Frag_Color;\n"
    "layout( std140, binding = 0 ) uniform LocalConstants { mat4 ProjMtx; };\n"
    "void main()\n"
    "{\n"
    "    Frag_UV = UV;\n"
    "    Frag_Color = Color;\n"
    "    gl_Position = ProjMtx * vec4( Position.xy,0,1 );\n"
    "}\n"
};

static const char* g_fragment_shader_code = {
    "#version 450\n"
    "layout (location = 0) in vec2 Frag_UV;\n"
    "layout (location = 1) in vec4 Frag_Color;\n"
    "layout (location = 0) out vec4 Out_Color;\n"
    "layout (binding = 1) uniform sampler2D Texture;\n"
    "void main()\n"
    "{\n"
    "    Out_Color = Frag_Color * texture(Texture, Frag_UV.st);\n"
    "}\n"
};

// Bindless code //////////////////////////////////////////////////////////
static const char* g_vertex_shader_code_bindless = {
    "#version 450\n"
    "layout( location = 0 ) in vec2 Position;\n"
    "layout( location = 1 ) in vec2 UV;\n"
    "layout( location = 2 ) in vec4 Color;\n"
    "layout( location = 0 ) out vec2 Frag_UV;\n"
    "layout( location = 1 ) out vec4 Frag_Color;\n"
    "layout (location = 2) flat out uint texture_id;\n"
    "layout( std140, set = 1, binding = 0 ) uniform LocalConstants { mat4 ProjMtx; };\n"
    "void main()\n"
    "{\n"
    "    Frag_UV = UV;\n"
    "    Frag_Color = Color;\n"
    "    texture_id = gl_InstanceIndex;\n"
    "    gl_Position = ProjMtx * vec4( Position.xy,0,1 );\n"
    "}\n"
};

static const char* g_fragment_shader_code_bindless = {
    "#version 450\n"
    "#extension GL_EXT_nonuniform_qualifier : enable\n"
    "layout (location = 0) in vec2 Frag_UV;\n"
    "layout (location = 1) in vec4 Frag_Color;\n"
    "layout (location = 2) flat in uint texture_id;\n"
    "layout (location = 0) out vec4 Out_Color;\n"
    "layout (set = 0, binding = 10) uniform sampler2D textures[];\n"
    "void main()\n"
    "{\n"
    "    Out_Color = Frag_Color * texture(textures[nonuniformEXT(texture_id)], Frag_UV.st);\n"
    "}\n"
};

// Slang bindless code ////////////////////////////////////////////////////

// Slang shaders honor the 0 based instance index, thus we need to pass the instance ID as a push constant
static const char* g_vertex_shader_code_bindless_slang = {
    "cbuffer material_buffer : register(b0, space1) { float4x4 ProjectionMatrix; };\n"
    "struct VSInput {\n"
    "\n"
    "[[vk::location(0)]] float2 Position : POSITION;\n"
    "[[vk::location(1)]] float2 UV : TEXCOORD0;\n"
    "[[vk::location(2)]] float4 Color : COLOR0;};\n"
    "\n"
    "struct PSInput {\n"
    "float4 Position : SV_POSITION;\n"
    "[[vk::location(0)]] float2 UV : TEXCOORD0;\n"
    "[[vk::location(1)]] float4 Color : COLOR0;\n"
    "[[vk::location(2)]] nointerpolation uint TextureIndex : TEX_ID; };\n"
    "[shader(\"vertex\")]\n"
    "PSInput main(VSInput input, uint base_instance : SV_StartInstanceLocation) {\n"
    "PSInput output;\n"
    "output.Position = mul( ProjectionMatrix, float4( input.Position.xy, 0.f, 1.f ) );\n"
    "output.Color = input.Color;\n"
    "output.UV = input.UV;\n"
    "output.TextureIndex = base_instance;\n"
    "return output;\n"
    "}\n"
};

static const char* g_fragment_shader_code_bindless_slang = {
    "struct PSInput {\n"
    "float4 Position : SV_POSITION;\n"
    "[[vk::location(0)]] float2 UV : TEXCOORD0;\n"
    "[[vk::location(1)]] float4 Color : COLOR0;\n"
    "[[vk::location(2)]] nointerpolation uint TextureIndex : TEX_ID; };\n"
    "\n"
    "struct PSOutput {\n"
    "[[vk::location(0)]] float4 Color : SV_TARGET0; };\n"
    "\n"
    "Texture2D<float4> global_textures[] : register(t10, space0);\n"
    "SamplerState global_samplers[] : register(s12, space0);\n"
    "[shader(\"fragment\")]\n"
    "PSOutput main(PSInput input) {\n"
    "PSOutput output;\n"
    "output.Color = input.Color * global_textures[input.TextureIndex].Sample(global_samplers[0], input.UV);\n"
    "return output;\n"
    "}\n"
};

static ImGuiService s_imgui_service;

ImGuiService* ImGuiService::instance() {
    return &s_imgui_service;
}

//
//
void ImGuiService::init( void* configuration ) {

    ImGuiServiceConfiguration* imgui_config = ( ImGuiServiceConfiguration* )configuration;
    gpu = imgui_config->gpu;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    // Setup Platform/Renderer bindings
    ImGui_ImplSDL3_InitForVulkan( (SDL_Window*)imgui_config->window_handle );

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;         // Enable Docking
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;       // Enable Multi-Viewport / Platform Windows
    io.BackendRendererName = "Raptor_ImGui";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;

    // Setup scaling
    float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);        // Bake a fixed style scale. (until we have a solution for dynamic style scaling, changing this requires resetting Style + calling this again)
    style.FontScaleDpi = main_scale;        // Set initial font scale. (using io.ConfigDpiScaleFonts=true makes this unnecessary. We leave both here for documentation purpose)
    io.ConfigDpiScaleFonts = true;          // [Experimental] Automatically overwrite style.FontScaleDpi in Begin() when Monitor DPI changes. This will scale fonts but _NOT_ scale sizes/padding for now.
    io.ConfigDpiScaleViewports = true;      // [Experimental] Scale Dear ImGui and Platform Windows when Monitor DPI changes.

    // When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look identical to regular ones.
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    using namespace raptor;

    // Load font texture atlas //////////////////////////////////////////////////
    unsigned char* pixels;
    int width, height;
    // Load as RGBA 32-bits (75% of the memory is wasted, but default font is so small) because it is more likely to be
    // compatible with user's existing shaders. If your ImTextureId represent a higher-level concept than just a GL texture id,
    // consider calling GetTexDataAsAlpha8() instead to save on GPU memory.
    io.Fonts->GetTexDataAsRGBA32( &pixels, &width, &height );

    ImageCreation texture_creation;
    texture_creation.set_format_type( VK_FORMAT_R8G8B8A8_UNORM, TextureType::Texture2D ).set_data( pixels ).set_size( width, height, 1 ).set_name( "ImGui_Font" );
    g_font_image = gpu->create_image( texture_creation );

    g_font_image_view = gpu->create_image_view( {
        .parent_image = g_font_image,
        .view_type = VK_IMAGE_VIEW_TYPE_2D,
        .sub_resource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        .name = "IV_ImGui_Font" } );

    gpu->add_image_view_to_bindless( g_font_image_view );

    // Store our identifier
    io.Fonts->TexID = (ImTextureID)&g_font_image_view;

    // Manual code. Used to remove dependency from that.
    PipelineCreation pipeline_creation = {};

    ShaderCompilationStage vs_stage, ps_stage;
    bool slang_input = false;

    vs_stage.type = VK_SHADER_STAGE_VERTEX_BIT;
    ps_stage.type = VK_SHADER_STAGE_FRAGMENT_BIT;

    if ( gpu->bindless_supported ) {

        if ( s_use_slang_shaders ) {

            vs_stage.source_code = g_vertex_shader_code_bindless_slang;
            ps_stage.source_code = g_fragment_shader_code_bindless_slang;

            slang_input = true;
        }
        else {
            vs_stage.source_code = g_vertex_shader_code_bindless;
            ps_stage.source_code = g_fragment_shader_code_bindless;
        }
    }
    else {
        vs_stage.source_code = g_vertex_shader_code;
        ps_stage.source_code = g_fragment_shader_code;
    }

    ArenaAllocator* temp_allocator = MemoryService::instance()->get_thread_allocator();

    Span<const u32> vs, ps;

    bool shader_changed = false; // TODO(marco): ignored for now
    bool result = ShaderCompiler::compile_and_cache_shader( vs_stage, vs, nullptr, temp_allocator,
                                                            "imgui", "imgui", nullptr, nullptr, false,
                                                            slang_input, true, shader_changed );

    ShaderCompiler::compile_and_cache_shader( ps_stage, ps, nullptr, temp_allocator,
                                              "imgui", "imgui", nullptr, nullptr,
                                              false, slang_input, true, shader_changed );

    // Create pipeline
    ShaderStateCreation shader_creation;
    VkShaderModuleCreateInfo vs_info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    vs_info.codeSize = vs.size;
    vs_info.pCode = vs.data;

    VkShaderModuleCreateInfo ps_info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ps_info.codeSize = ps.size;
    ps_info.pCode = ps.data;

    shader_creation.add_stage( vs_info, VK_SHADER_STAGE_VERTEX_BIT )
        .add_stage( ps_info, VK_SHADER_STAGE_FRAGMENT_BIT ).set_name( "imgui" );

    ShaderStateHandle shader = gpu->create_shader_state( shader_creation );

    pipeline_creation.shader = shader;
    pipeline_creation.name = "Pipeline_ImGui";

    pipeline_creation.blend_state.blend_states = {
        {
            .source_color = VK_BLEND_FACTOR_SRC_ALPHA,
            .destination_color = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .color_operation = VK_BLEND_OP_ADD,
        }
    };

    pipeline_creation.vertex_input = {
        .bindings = {
            { 0, 20, VK_VERTEX_INPUT_RATE_VERTEX }
        },
        .attributes = {
            { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 },
            { 1, 0, VK_FORMAT_R32G32_SFLOAT, 8 },
            { 2, 0, VK_FORMAT_R8G8B8A8_UNORM, 16 }
        }
    };

    pipeline_creation.render_pass_output = gpu->get_swapchain_output();

    pipeline_creation.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    DescriptorSetLayoutCreation descriptor_set_layout_creation{};
    if ( !gpu->bindless_supported ) {
        g_descriptor_set_layout = gpu->create_descriptor_set_layout( {
            .bindings = {{ 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 },
                         { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1 }},
            .set_index = 0,
            .name = "DSL_ImGUI" } );

        g_pipeline_layout = gpu->create_pipeline_layout( {
            .layouts = { g_descriptor_set_layout },
            .name = "PL_ImGui" } );
    }
    else {
        g_descriptor_set_layout = gpu->create_descriptor_set_layout( {
            .bindings = {{ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1 }},
            .set_index = 1,
            .name = "DSL_ImGUI"});

        g_pipeline_layout = gpu->create_pipeline_layout( {
            .layouts = { gpu->bindless_descriptor_set_layout, g_descriptor_set_layout },
            .name = "PL_ImGui" } );
    }

    pipeline_creation.layout = g_pipeline_layout;

    StringBuffer pipeline_cache_path;
    pipeline_cache_path.init( 2048, temp_allocator );
    char* cache_path = pipeline_cache_path.append_use_f( "%s/%s.cache", imgui_config->renderer->resource_cache.binary_data_folder, pipeline_creation.name );
    g_imgui_pipeline = gpu->create_pipeline( pipeline_creation, cache_path );

    // Create descriptor set
    if ( gpu->bindless_supported ) {
        g_ui_descriptor_set = gpu->create_descriptor_set( {
            .dynamic_buffers = { { 0, 64 } },
            .layout = g_descriptor_set_layout,
            .name = "RL_ImGui" } );
    } else {
        g_ui_descriptor_set = gpu->create_descriptor_set( {
            .textures = { { g_font_image_view, 1 } },
            .dynamic_buffers = { { 0, 64 } },
            .layout = g_descriptor_set_layout,
            .name = "RL_ImGui" } );
    }

    // Add descriptor set to the map
    // Old Map
    g_image_view_to_descriptor_set.init( &MemoryService::instance()->system_allocator, 4 );
    g_image_view_to_descriptor_set.insert( g_font_image_view.index(), g_ui_descriptor_set );

    // Create vertex and index buffers //////////////////////////////////////////
    for ( u32 i = 0; i < k_max_frames; ++i ) {
        g_vb[ i ] = gpu->create_buffer( {
            .size = g_vb_size,
            .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
            .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .name = "VB_ImGui" } );

        g_ib[ i ] = gpu->create_buffer( {
            .size = g_ib_size,
            .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
            .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .name = "IB_ImGui" } );
    }
}

void ImGuiService::shutdown() {

    FlatHashMapIterator it = g_image_view_to_descriptor_set.iterator_begin();
    while ( it.is_valid() ) {
        raptor::DescriptorSetHandle handle = g_image_view_to_descriptor_set.get( it );
        gpu->destroy_descriptor_set( handle );

        g_image_view_to_descriptor_set.iterator_advance( it );
    }

    g_image_view_to_descriptor_set.shutdown();

    for ( u32 i = 0; i < k_max_frames; ++i ) {
        gpu->destroy_buffer( g_vb[ i ] );
        gpu->destroy_buffer( g_ib[ i ]);
    }

    // NOTE: this is destroyed by the pipeline.
    gpu->destroy_pipeline_layout( g_pipeline_layout );
    gpu->destroy_descriptor_set_layout( g_descriptor_set_layout );

    gpu->destroy_image_view( g_font_image_view );
    gpu->destroy_pipeline( g_imgui_pipeline );
    gpu->destroy_image( g_font_image );

    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void ImGuiService::new_frame() {
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    // Reset draw data
    frame_draw_data = nullptr;
}

void ImGuiService::finalize_draw_data() {

    ImGui::Render();

    frame_draw_data = ImGui::GetDrawData();
}

void ImGuiService::render( raptor::CommandBuffer& commands, bool use_secondary ) {

    RASSERTM( frame_draw_data, "ImGui draw data not finalized! Call finalize_draw_data() after new_frame() and before render() IN THE MAIN THREAD." );
    ImDrawData* draw_data = frame_draw_data;

    // Avoid rendering when minimized, scale coordinates for retina displays (screen coordinates != framebuffer coordinates)
    int fb_width = (int)( draw_data->DisplaySize.x * draw_data->FramebufferScale.x );
    int fb_height = (int)( draw_data->DisplaySize.y * draw_data->FramebufferScale.y );
    if ( fb_width <= 0 || fb_height <= 0 )
        return;

    // Vulkan backend has a different origin than OpenGL.
    bool clip_origin_lower_left = false;

#if defined(GL_CLIP_ORIGIN) && !defined(__APPLE__)
    GLenum last_clip_origin = 0; glGetIntegerv( GL_CLIP_ORIGIN, (GLint*)&last_clip_origin ); // Support for GL 4.5's glClipControl(GL_UPPER_LEFT)
    if ( last_clip_origin == GL_UPPER_LEFT )
        clip_origin_lower_left = false;
#endif
    size_t vertex_size = draw_data->TotalVtxCount * sizeof( ImDrawVert );
    size_t index_size = draw_data->TotalIdxCount * sizeof( ImDrawIdx );

    if ( vertex_size >= g_vb_size || index_size >= g_ib_size ) {
        rprint( "ImGui Backend Error: vertex/index overflow!\n" );
        return;
    }

    if ( vertex_size == 0 && index_size == 0 ) {
        return;
    }

    using namespace raptor;

    // Upload data
    const u32 frame_index = gpu->current_frame;

    BufferHandle vertex_buffer_handle = g_vb[ frame_index ];
    BufferHandle index_buffer_handle = g_ib[ frame_index ];

    Buffer* vertex_buffer = gpu->get_buffer( vertex_buffer_handle );
    Buffer* index_buffer = gpu->get_buffer( index_buffer_handle );

    RASSERT( vertex_buffer && vertex_buffer->mapped_data );
    RASSERT( index_buffer && index_buffer->mapped_data );

    ImDrawVert* vertex_data = ( ImDrawVert* )vertex_buffer->mapped_data;
    ImDrawIdx* index_data = ( ImDrawIdx* )index_buffer->mapped_data;

    for ( i32 list_index = 0; list_index < draw_data->CmdListsCount; ++list_index ) {
        const ImDrawList* command_list = draw_data->CmdLists[ list_index ];

        const VkDeviceSize vertex_size = command_list->VtxBuffer.Size * sizeof( ImDrawVert );
        const VkDeviceSize index_size = command_list->IdxBuffer.Size * sizeof( ImDrawIdx );

        memcpy( vertex_data, command_list->VtxBuffer.Data, vertex_size );
        memcpy( index_data, command_list->IdxBuffer.Data, index_size );

        vertex_data += command_list->VtxBuffer.Size;
        index_data += command_list->IdxBuffer.Size;
    }

    const VkDeviceSize total_vertex_size = draw_data->TotalVtxCount * sizeof( ImDrawVert );
    const VkDeviceSize total_index_size = draw_data->TotalIdxCount * sizeof( ImDrawIdx );

    gpu->flush_buffer( vertex_buffer_handle, 0, total_vertex_size );
    gpu->flush_buffer( index_buffer_handle, 0, total_index_size );

    // TODO_KS: Add the sorting.
    commands.push_marker( "ImGUI" );

    commands.bind_pipeline( g_imgui_pipeline );
    commands.bind_vertex_buffer( vertex_buffer_handle, 0, 0 );
    commands.bind_index_buffer( index_buffer_handle, 0, VK_INDEX_TYPE_UINT16 );

    //const Viewport viewport = { 0, 0, (u16)fb_width, (u16)fb_height, 0.0f, 1.0f };
    //commands.set_viewport( &viewport );
    commands.set_fullscreen_viewport();

    // Setup viewport, orthographic projection matrix
    // Our visible imgui space lies from draw_data->DisplayPos (top left) to draw_data->DisplayPos+data_data->DisplaySize (bottom right). DisplayMin is typically (0,0) for single viewport apps.
    float L = draw_data->DisplayPos.x;
    float R = draw_data->DisplayPos.x + draw_data->DisplaySize.x;
    float T = draw_data->DisplayPos.y;
    float B = draw_data->DisplayPos.y + draw_data->DisplaySize.y;
    const float ortho_projection[4][4] =
    {
        { 2.0f / ( R - L ),   0.0f,         0.0f,   0.0f },
        { 0.0f,         2.0f / ( T - B ),   0.0f,   0.0f },
        { 0.0f,         0.0f,        -1.0f,   0.0f },
        { ( R + L ) / ( L - R ),  ( T + B ) / ( B - T ),  0.0f,   1.0f },
    };

    u32 cb_offset;
    float* cb_data = ( float* )gpu->dynamic_buffer_allocate( 64, alignof( float ), &cb_offset );
    if ( cb_data ) {
        memcpy( cb_data, &ortho_projection[ 0 ][ 0 ], 64 );
    }

    // Will project scissor/clipping rectangles into framebuffer space
    ImVec2 clip_off = draw_data->DisplayPos;         // (0,0) unless using multi-viewports
    ImVec2 clip_scale = draw_data->FramebufferScale; // (1,1) unless using retina display which are often (2,2)

    // Render command lists
    //
    int counts = draw_data->CmdListsCount;

    ImageViewHandle last_image_view = g_font_image_view;
    // todo:map
    DescriptorSetHandle last_descriptor_set = { g_image_view_to_descriptor_set.get( last_image_view.index() ) };

    if ( gpu->bindless_supported ) {
        commands.bind_descriptor_set( { gpu->bindless_descriptor_set, last_descriptor_set }, { cb_offset } );
    } else {
        commands.bind_descriptor_set( { last_descriptor_set }, { cb_offset } );
    }

    uint32_t vtx_buffer_offset = 0, index_buffer_offset = 0;
    for ( int n = 0; n < counts; n++ )
    {
        const ImDrawList* cmd_list = draw_data->CmdLists[n];

        for ( int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++ )
        {
            const ImDrawCmd* pcmd = &cmd_list->CmdBuffer[cmd_i];
            if ( pcmd->UserCallback )
            {
                // User callback (registered via ImDrawList::AddCallback)
                pcmd->UserCallback( cmd_list, pcmd );
            }
            else
            {
                // Project scissor/clipping rectangles into framebuffer space
                ImVec4 clip_rect;
                clip_rect.x = ( pcmd->ClipRect.x - clip_off.x ) * clip_scale.x;
                clip_rect.y = ( pcmd->ClipRect.y - clip_off.y ) * clip_scale.y;
                clip_rect.z = ( pcmd->ClipRect.z - clip_off.x ) * clip_scale.x;
                clip_rect.w = ( pcmd->ClipRect.w - clip_off.y ) * clip_scale.y;

                if ( clip_rect.x < fb_width && clip_rect.y < fb_height && clip_rect.z >= 0.0f && clip_rect.w >= 0.0f )
                {
                    // Apply scissor/clipping rectangle
                    /*if ( clip_origin_lower_left ) {
                        Rect2DInt scissor_rect = { (int16_t)clip_rect.x, (int16_t)( fb_height - clip_rect.w ), (uint16_t)( clip_rect.z - clip_rect.x ), (uint16_t)( clip_rect.w - clip_rect.y ) };
                        commands.set_scissor( &scissor_rect );
                    }
                    else {
                        Rect2DInt scissor_rect = { (int16_t)clip_rect.x, (int16_t)clip_rect.y,                 (uint16_t)( clip_rect.z - clip_rect.x ), (uint16_t)( clip_rect.w - clip_rect.y ) };
                        commands.set_scissor( &scissor_rect );
                    }*/
                    VkRect2D scissor;
                    scissor.offset.x = (i32)clip_rect.x;
                    scissor.offset.y = clip_origin_lower_left ? (i32)(fb_height - clip_rect.w) : (i32)clip_rect.y;
                    scissor.extent.width = (u32)( clip_rect.z - clip_rect.x );
                    scissor.extent.height = (u32)( clip_rect.w - clip_rect.y );

                    commands.set_scissor( scissor );

                    // Retrieve
                    ImageViewHandle new_image_view = *(ImageViewHandle*)( pcmd->GetTexID() );
                    if ( !gpu->bindless_supported ) {
                        if ( new_image_view != last_image_view && new_image_view.is_valid() ) {
                            last_image_view = new_image_view;
                            FlatHashMapIterator it = g_image_view_to_descriptor_set.find( last_image_view.index() );

                            // TODO: invalidate handles and update descriptor set when needed ?
                            // Found this problem when reusing the handle from a previous
                            // If not present
                            if ( it.is_invalid() ) {
                                //ds_creation.set_layout( g_descriptor_set_layout ).buffer( g_ui_cb, 0 ).texture( last_texture, 1 ).set_name( "RL_Dynamic_ImGUI" );
                                last_descriptor_set = gpu->create_descriptor_set( {
                                    .textures = { { last_image_view, 1 } },
                                    .dynamic_buffers = { { 0, 64 } },
                                    .layout = g_descriptor_set_layout,
                                    .name = "RL_Dynamic_ImGUI" } );

                                g_image_view_to_descriptor_set.insert( new_image_view.index(), last_descriptor_set );
                            } else {
                                last_descriptor_set = { g_image_view_to_descriptor_set.get( it ) };
                            }
                            commands.bind_descriptor_set( { last_descriptor_set }, { cb_offset } );
                        }
                    }

                    commands.draw_indexed( raptor::TopologyType::Triangle, pcmd->ElemCount, 1, index_buffer_offset + pcmd->IdxOffset, vtx_buffer_offset + pcmd->VtxOffset, new_image_view.index() );
                }
            }

        }
        index_buffer_offset += cmd_list->IdxBuffer.Size;
        vtx_buffer_offset += cmd_list->VtxBuffer.Size;
    }

    commands.pop_marker();
}

static void set_style_dark_gold();
static void set_style_green_blue();
static void set_style_dark_red();

void ImGuiService::set_style( ImGuiStyles style ) {

    switch ( style ) {
        case GreenBlue:
        {
            set_style_green_blue();
            break;
        }

        case DarkRed:
        {
            set_style_dark_red();
            break;
        }

        case DarkGold:
        {
            set_style_dark_gold();
            break;
        }

        default:
        case Default:
        {
            ImGui::StyleColorsDark();
            break;
        }
    }
}

//void ImGuiService::remove_cached_image_view( ImageViewHandle& image_view ) {
//    FlatHashMapIterator it = g_image_view_to_descriptor_set.find( image_view.index );
//    if ( it.is_valid() ) {
//
//        // Destroy descriptor set
//        raptor::DescriptorSetHandle descriptor_set{ g_image_view_to_descriptor_set.get(it) };
//        gpu->destroy_descriptor_set( descriptor_set );
//
//        // Remove from cache
//        g_image_view_to_descriptor_set.remove( image_view.index );
//    }
//
//}

void set_style_dark_red() {
    ImVec4* colors = ImGui::GetStyle().Colors;
    colors[ ImGuiCol_Text ] = ImVec4( 0.75f, 0.75f, 0.75f, 1.00f );
    colors[ ImGuiCol_TextDisabled ] = ImVec4( 0.35f, 0.35f, 0.35f, 1.00f );
    colors[ ImGuiCol_WindowBg ] = ImVec4( 0.00f, 0.00f, 0.00f, 0.94f );
    colors[ ImGuiCol_ChildBg ] = ImVec4( 0.00f, 0.00f, 0.00f, 0.00f );
    colors[ ImGuiCol_PopupBg ] = ImVec4( 0.08f, 0.08f, 0.08f, 0.94f );
    colors[ ImGuiCol_Border ] = ImVec4( 0.00f, 0.00f, 0.00f, 0.50f );
    colors[ ImGuiCol_BorderShadow ] = ImVec4( 0.00f, 0.00f, 0.00f, 0.00f );
    colors[ ImGuiCol_FrameBg ] = ImVec4( 0.00f, 0.00f, 0.00f, 0.54f );
    colors[ ImGuiCol_FrameBgHovered ] = ImVec4( 0.37f, 0.14f, 0.14f, 0.67f );
    colors[ ImGuiCol_FrameBgActive ] = ImVec4( 0.39f, 0.20f, 0.20f, 0.67f );
    colors[ ImGuiCol_TitleBg ] = ImVec4( 0.04f, 0.04f, 0.04f, 1.00f );
    colors[ ImGuiCol_TitleBgActive ] = ImVec4( 0.48f, 0.16f, 0.16f, 1.00f );
    colors[ ImGuiCol_TitleBgCollapsed ] = ImVec4( 0.48f, 0.16f, 0.16f, 1.00f );
    colors[ ImGuiCol_MenuBarBg ] = ImVec4( 0.14f, 0.14f, 0.14f, 1.00f );
    colors[ ImGuiCol_ScrollbarBg ] = ImVec4( 0.02f, 0.02f, 0.02f, 0.53f );
    colors[ ImGuiCol_ScrollbarGrab ] = ImVec4( 0.31f, 0.31f, 0.31f, 1.00f );
    colors[ ImGuiCol_ScrollbarGrabHovered ] = ImVec4( 0.41f, 0.41f, 0.41f, 1.00f );
    colors[ ImGuiCol_ScrollbarGrabActive ] = ImVec4( 0.51f, 0.51f, 0.51f, 1.00f );
    colors[ ImGuiCol_CheckMark ] = ImVec4( 0.56f, 0.10f, 0.10f, 1.00f );
    colors[ ImGuiCol_SliderGrab ] = ImVec4( 1.00f, 0.19f, 0.19f, 0.40f );
    colors[ ImGuiCol_SliderGrabActive ] = ImVec4( 0.89f, 0.00f, 0.19f, 1.00f );
    colors[ ImGuiCol_Button ] = ImVec4( 1.00f, 0.19f, 0.19f, 0.40f );
    colors[ ImGuiCol_ButtonHovered ] = ImVec4( 0.80f, 0.17f, 0.00f, 1.00f );
    colors[ ImGuiCol_ButtonActive ] = ImVec4( 0.89f, 0.00f, 0.19f, 1.00f );
    colors[ ImGuiCol_Header ] = ImVec4( 0.33f, 0.35f, 0.36f, 0.53f );
    colors[ ImGuiCol_HeaderHovered ] = ImVec4( 0.76f, 0.28f, 0.44f, 0.67f );
    colors[ ImGuiCol_HeaderActive ] = ImVec4( 0.47f, 0.47f, 0.47f, 0.67f );
    colors[ ImGuiCol_Separator ] = ImVec4( 0.32f, 0.32f, 0.32f, 1.00f );
    colors[ ImGuiCol_SeparatorHovered ] = ImVec4( 0.32f, 0.32f, 0.32f, 1.00f );
    colors[ ImGuiCol_SeparatorActive ] = ImVec4( 0.32f, 0.32f, 0.32f, 1.00f );
    colors[ ImGuiCol_ResizeGrip ] = ImVec4( 1.00f, 1.00f, 1.00f, 0.85f );
    colors[ ImGuiCol_ResizeGripHovered ] = ImVec4( 1.00f, 1.00f, 1.00f, 0.60f );
    colors[ ImGuiCol_ResizeGripActive ] = ImVec4( 1.00f, 1.00f, 1.00f, 0.90f );
    colors[ ImGuiCol_Tab ] = ImVec4( 0.07f, 0.07f, 0.07f, 0.51f );
    colors[ ImGuiCol_TabHovered ] = ImVec4( 0.86f, 0.23f, 0.43f, 0.67f );
    colors[ ImGuiCol_TabActive ] = ImVec4( 0.19f, 0.19f, 0.19f, 0.57f );
    colors[ ImGuiCol_TabUnfocused ] = ImVec4( 0.05f, 0.05f, 0.05f, 0.90f );
    colors[ ImGuiCol_TabUnfocusedActive ] = ImVec4( 0.13f, 0.13f, 0.13f, 0.74f );
#if defined(IMGUI_HAS_DOCK)
    colors[ ImGuiCol_DockingPreview ] = ImVec4( 0.47f, 0.47f, 0.47f, 0.47f );
    colors[ ImGuiCol_DockingEmptyBg ] = ImVec4( 0.20f, 0.20f, 0.20f, 1.00f );
#endif // IMGUI_HAS_DOCK
    colors[ ImGuiCol_PlotLines ] = ImVec4( 0.61f, 0.61f, 0.61f, 1.00f );
    colors[ ImGuiCol_PlotLinesHovered ] = ImVec4( 1.00f, 0.43f, 0.35f, 1.00f );
    colors[ ImGuiCol_PlotHistogram ] = ImVec4( 0.90f, 0.70f, 0.00f, 1.00f );
    colors[ ImGuiCol_PlotHistogramHovered ] = ImVec4( 1.00f, 0.60f, 0.00f, 1.00f );
#if defined(IMGUI_HAS_TABLE)
    colors[ ImGuiCol_TableHeaderBg ] = ImVec4( 0.19f, 0.19f, 0.20f, 1.00f );
    colors[ ImGuiCol_TableBorderStrong ] = ImVec4( 0.31f, 0.31f, 0.35f, 1.00f );
    colors[ ImGuiCol_TableBorderLight ] = ImVec4( 0.23f, 0.23f, 0.25f, 1.00f );
    colors[ ImGuiCol_TableRowBg ] = ImVec4( 0.00f, 0.00f, 0.00f, 0.00f );
    colors[ ImGuiCol_TableRowBgAlt ] = ImVec4( 1.00f, 1.00f, 1.00f, 0.07f );
#endif // IMGUI_HAS_TABLE
    colors[ ImGuiCol_TextSelectedBg ] = ImVec4( 0.26f, 0.59f, 0.98f, 0.35f );
    colors[ ImGuiCol_DragDropTarget ] = ImVec4( 1.00f, 1.00f, 0.00f, 0.90f );
    colors[ ImGuiCol_NavHighlight ] = ImVec4( 0.26f, 0.59f, 0.98f, 1.00f );
    colors[ ImGuiCol_NavWindowingHighlight ] = ImVec4( 1.00f, 1.00f, 1.00f, 0.70f );
    colors[ ImGuiCol_NavWindowingDimBg ] = ImVec4( 0.80f, 0.80f, 0.80f, 0.20f );
    colors[ ImGuiCol_ModalWindowDimBg ] = ImVec4( 0.80f, 0.80f, 0.80f, 0.35f );
}


void set_style_green_blue() {
    ImVec4* colors = ImGui::GetStyle().Colors;
    colors[ ImGuiCol_Text ] = ImVec4( 1.00f, 1.00f, 1.00f, 1.00f );
    colors[ ImGuiCol_TextDisabled ] = ImVec4( 0.50f, 0.50f, 0.50f, 1.00f );
    colors[ ImGuiCol_WindowBg ] = ImVec4( 0.06f, 0.06f, 0.06f, 0.94f );
    colors[ ImGuiCol_ChildBg ] = ImVec4( 0.00f, 0.00f, 0.00f, 0.00f );
    colors[ ImGuiCol_PopupBg ] = ImVec4( 0.08f, 0.08f, 0.08f, 0.94f );
    colors[ ImGuiCol_Border ] = ImVec4( 0.43f, 0.43f, 0.50f, 0.50f );
    colors[ ImGuiCol_BorderShadow ] = ImVec4( 0.00f, 0.00f, 0.00f, 0.00f );
    colors[ ImGuiCol_FrameBg ] = ImVec4( 0.44f, 0.44f, 0.44f, 0.60f );
    colors[ ImGuiCol_FrameBgHovered ] = ImVec4( 0.57f, 0.57f, 0.57f, 0.70f );
    colors[ ImGuiCol_FrameBgActive ] = ImVec4( 0.76f, 0.76f, 0.76f, 0.80f );
    colors[ ImGuiCol_TitleBg ] = ImVec4( 0.04f, 0.04f, 0.04f, 1.00f );
    colors[ ImGuiCol_TitleBgActive ] = ImVec4( 0.16f, 0.16f, 0.16f, 1.00f );
    colors[ ImGuiCol_TitleBgCollapsed ] = ImVec4( 0.00f, 0.00f, 0.00f, 0.60f );
    colors[ ImGuiCol_MenuBarBg ] = ImVec4( 0.14f, 0.14f, 0.14f, 1.00f );
    colors[ ImGuiCol_ScrollbarBg ] = ImVec4( 0.02f, 0.02f, 0.02f, 0.53f );
    colors[ ImGuiCol_ScrollbarGrab ] = ImVec4( 0.31f, 0.31f, 0.31f, 1.00f );
    colors[ ImGuiCol_ScrollbarGrabHovered ] = ImVec4( 0.41f, 0.41f, 0.41f, 1.00f );
    colors[ ImGuiCol_ScrollbarGrabActive ] = ImVec4( 0.51f, 0.51f, 0.51f, 1.00f );
    colors[ ImGuiCol_CheckMark ] = ImVec4( 0.13f, 0.75f, 0.55f, 0.80f );
    colors[ ImGuiCol_SliderGrab ] = ImVec4( 0.13f, 0.75f, 0.75f, 0.80f );
    colors[ ImGuiCol_SliderGrabActive ] = ImVec4( 0.13f, 0.75f, 1.00f, 0.80f );
    colors[ ImGuiCol_Button ] = ImVec4( 0.13f, 0.75f, 0.55f, 0.40f );
    colors[ ImGuiCol_ButtonHovered ] = ImVec4( 0.13f, 0.75f, 0.75f, 0.60f );
    colors[ ImGuiCol_ButtonActive ] = ImVec4( 0.13f, 0.75f, 1.00f, 0.80f );
    colors[ ImGuiCol_Header ] = ImVec4( 0.13f, 0.75f, 0.55f, 0.40f );
    colors[ ImGuiCol_HeaderHovered ] = ImVec4( 0.13f, 0.75f, 0.75f, 0.60f );
    colors[ ImGuiCol_HeaderActive ] = ImVec4( 0.13f, 0.75f, 1.00f, 0.80f );
    colors[ ImGuiCol_Separator ] = ImVec4( 0.13f, 0.75f, 0.55f, 0.40f );
    colors[ ImGuiCol_SeparatorHovered ] = ImVec4( 0.13f, 0.75f, 0.75f, 0.60f );
    colors[ ImGuiCol_SeparatorActive ] = ImVec4( 0.13f, 0.75f, 1.00f, 0.80f );
    colors[ ImGuiCol_ResizeGrip ] = ImVec4( 0.13f, 0.75f, 0.55f, 0.40f );
    colors[ ImGuiCol_ResizeGripHovered ] = ImVec4( 0.13f, 0.75f, 0.75f, 0.60f );
    colors[ ImGuiCol_ResizeGripActive ] = ImVec4( 0.13f, 0.75f, 1.00f, 0.80f );
    colors[ ImGuiCol_Tab ] = ImVec4( 0.13f, 0.75f, 0.55f, 0.80f );
    colors[ ImGuiCol_TabHovered ] = ImVec4( 0.13f, 0.75f, 0.75f, 0.80f );
    colors[ ImGuiCol_TabActive ] = ImVec4( 0.13f, 0.75f, 1.00f, 0.80f );
    colors[ ImGuiCol_TabUnfocused ] = ImVec4( 0.18f, 0.18f, 0.18f, 1.00f );
    colors[ ImGuiCol_TabUnfocusedActive ] = ImVec4( 0.36f, 0.36f, 0.36f, 0.54f );
#if defined(IMGUI_HAS_DOCK)
    colors[ ImGuiCol_DockingPreview ] = ImVec4( 0.13f, 0.75f, 0.55f, 0.80f );
    colors[ ImGuiCol_DockingEmptyBg ] = ImVec4( 0.13f, 0.13f, 0.13f, 0.80f );
#endif // IMGUI_HAS_DOCK
    colors[ ImGuiCol_PlotLines ] = ImVec4( 0.61f, 0.61f, 0.61f, 1.00f );
    colors[ ImGuiCol_PlotLinesHovered ] = ImVec4( 1.00f, 0.43f, 0.35f, 1.00f );
    colors[ ImGuiCol_PlotHistogram ] = ImVec4( 0.90f, 0.70f, 0.00f, 1.00f );
    colors[ ImGuiCol_PlotHistogramHovered ] = ImVec4( 1.00f, 0.60f, 0.00f, 1.00f );
#if defined (IMGUI_HAS_TABLE)
    colors[ ImGuiCol_TableHeaderBg ] = ImVec4( 0.19f, 0.19f, 0.20f, 1.00f );
    colors[ ImGuiCol_TableBorderStrong ] = ImVec4( 0.31f, 0.31f, 0.35f, 1.00f );
    colors[ ImGuiCol_TableBorderLight ] = ImVec4( 0.23f, 0.23f, 0.25f, 1.00f );
    colors[ ImGuiCol_TableRowBg ] = ImVec4( 0.00f, 0.00f, 0.00f, 0.00f );
    colors[ ImGuiCol_TableRowBgAlt ] = ImVec4( 1.00f, 1.00f, 1.00f, 0.07f );
#endif // IMGUI_HAS_TABLE
    colors[ ImGuiCol_TextSelectedBg ] = ImVec4( 0.26f, 0.59f, 0.98f, 0.35f );
    colors[ ImGuiCol_DragDropTarget ] = ImVec4( 1.00f, 1.00f, 0.00f, 0.90f );
    colors[ ImGuiCol_NavHighlight ] = ImVec4( 0.26f, 0.59f, 0.98f, 1.00f );
    colors[ ImGuiCol_NavWindowingHighlight ] = ImVec4( 1.00f, 1.00f, 1.00f, 0.70f );
    colors[ ImGuiCol_NavWindowingDimBg ] = ImVec4( 0.80f, 0.80f, 0.80f, 0.20f );
    colors[ ImGuiCol_ModalWindowDimBg ] = ImVec4( 0.80f, 0.80f, 0.80f, 0.35f );
}

static void set_style_dark_gold() {
    ImGuiStyle* style = &ImGui::GetStyle();
    ImVec4* colors = style->Colors;

    colors[ ImGuiCol_Text ] = ImVec4( 0.92f, 0.92f, 0.92f, 1.00f );
    colors[ ImGuiCol_TextDisabled ] = ImVec4( 0.44f, 0.44f, 0.44f, 1.00f );
    colors[ ImGuiCol_WindowBg ] = ImVec4( 0.06f, 0.06f, 0.06f, 1.00f );
    colors[ ImGuiCol_ChildBg ] = ImVec4( 0.00f, 0.00f, 0.00f, 0.00f );
    colors[ ImGuiCol_PopupBg ] = ImVec4( 0.08f, 0.08f, 0.08f, 0.94f );
    colors[ ImGuiCol_Border ] = ImVec4( 0.51f, 0.36f, 0.15f, 1.00f );
    colors[ ImGuiCol_BorderShadow ] = ImVec4( 0.00f, 0.00f, 0.00f, 0.00f );
    colors[ ImGuiCol_FrameBg ] = ImVec4( 0.11f, 0.11f, 0.11f, 1.00f );
    colors[ ImGuiCol_FrameBgHovered ] = ImVec4( 0.51f, 0.36f, 0.15f, 1.00f );
    colors[ ImGuiCol_FrameBgActive ] = ImVec4( 0.78f, 0.55f, 0.21f, 1.00f );
    colors[ ImGuiCol_TitleBg ] = ImVec4( 0.51f, 0.36f, 0.15f, 1.00f );
    colors[ ImGuiCol_TitleBgActive ] = ImVec4( 0.91f, 0.64f, 0.13f, 1.00f );
    colors[ ImGuiCol_TitleBgCollapsed ] = ImVec4( 0.00f, 0.00f, 0.00f, 0.51f );
    colors[ ImGuiCol_MenuBarBg ] = ImVec4( 0.11f, 0.11f, 0.11f, 1.00f );
    colors[ ImGuiCol_ScrollbarBg ] = ImVec4( 0.06f, 0.06f, 0.06f, 0.53f );
    colors[ ImGuiCol_ScrollbarGrab ] = ImVec4( 0.21f, 0.21f, 0.21f, 1.00f );
    colors[ ImGuiCol_ScrollbarGrabHovered ] = ImVec4( 0.47f, 0.47f, 0.47f, 1.00f );
    colors[ ImGuiCol_ScrollbarGrabActive ] = ImVec4( 0.81f, 0.83f, 0.81f, 1.00f );
    colors[ ImGuiCol_CheckMark ] = ImVec4( 0.78f, 0.55f, 0.21f, 1.00f );
    colors[ ImGuiCol_SliderGrab ] = ImVec4( 0.91f, 0.64f, 0.13f, 1.00f );
    colors[ ImGuiCol_SliderGrabActive ] = ImVec4( 0.91f, 0.64f, 0.13f, 1.00f );
    colors[ ImGuiCol_Button ] = ImVec4( 0.51f, 0.36f, 0.15f, 1.00f );
    colors[ ImGuiCol_ButtonHovered ] = ImVec4( 0.91f, 0.64f, 0.13f, 1.00f );
    colors[ ImGuiCol_ButtonActive ] = ImVec4( 0.78f, 0.55f, 0.21f, 1.00f );
    colors[ ImGuiCol_Header ] = ImVec4( 0.51f, 0.36f, 0.15f, 1.00f );
    colors[ ImGuiCol_HeaderHovered ] = ImVec4( 0.91f, 0.64f, 0.13f, 1.00f );
    colors[ ImGuiCol_HeaderActive ] = ImVec4( 0.93f, 0.65f, 0.14f, 1.00f );
    colors[ ImGuiCol_Separator ] = ImVec4( 0.21f, 0.21f, 0.21f, 1.00f );
    colors[ ImGuiCol_SeparatorHovered ] = ImVec4( 0.91f, 0.64f, 0.13f, 1.00f );
    colors[ ImGuiCol_SeparatorActive ] = ImVec4( 0.78f, 0.55f, 0.21f, 1.00f );
    colors[ ImGuiCol_ResizeGrip ] = ImVec4( 0.21f, 0.21f, 0.21f, 1.00f );
    colors[ ImGuiCol_ResizeGripHovered ] = ImVec4( 0.91f, 0.64f, 0.13f, 1.00f );
    colors[ ImGuiCol_ResizeGripActive ] = ImVec4( 0.78f, 0.55f, 0.21f, 1.00f );
    colors[ ImGuiCol_Tab ] = ImVec4( 0.51f, 0.36f, 0.15f, 1.00f );
    colors[ ImGuiCol_TabHovered ] = ImVec4( 0.91f, 0.64f, 0.13f, 1.00f );
    colors[ ImGuiCol_TabActive ] = ImVec4( 0.78f, 0.55f, 0.21f, 1.00f );
    colors[ ImGuiCol_TabUnfocused ] = ImVec4( 0.07f, 0.10f, 0.15f, 0.97f );
    colors[ ImGuiCol_TabUnfocusedActive ] = ImVec4( 0.14f, 0.26f, 0.42f, 1.00f );
    colors[ ImGuiCol_PlotLines ] = ImVec4( 0.61f, 0.61f, 0.61f, 1.00f );
    colors[ ImGuiCol_PlotLinesHovered ] = ImVec4( 1.00f, 0.43f, 0.35f, 1.00f );
    colors[ ImGuiCol_PlotHistogram ] = ImVec4( 0.90f, 0.70f, 0.00f, 1.00f );
    colors[ ImGuiCol_PlotHistogramHovered ] = ImVec4( 1.00f, 0.60f, 0.00f, 1.00f );
    colors[ ImGuiCol_TextSelectedBg ] = ImVec4( 0.26f, 0.59f, 0.98f, 0.35f );
    colors[ ImGuiCol_DragDropTarget ] = ImVec4( 1.00f, 1.00f, 0.00f, 0.90f );
    colors[ ImGuiCol_NavHighlight ] = ImVec4( 0.26f, 0.59f, 0.98f, 1.00f );
    colors[ ImGuiCol_NavWindowingHighlight ] = ImVec4( 1.00f, 1.00f, 1.00f, 0.70f );
    colors[ ImGuiCol_NavWindowingDimBg ] = ImVec4( 0.80f, 0.80f, 0.80f, 0.20f );
    colors[ ImGuiCol_ModalWindowDimBg ] = ImVec4( 0.80f, 0.80f, 0.80f, 0.35f );

    style->FramePadding = ImVec2( 4, 2 );
    style->ItemSpacing = ImVec2( 10, 2 );
    style->IndentSpacing = 12;
    style->ScrollbarSize = 10;

    style->WindowRounding = 4;
    style->FrameRounding = 4;
    style->PopupRounding = 4;
    style->ScrollbarRounding = 6;
    style->GrabRounding = 4;
    style->TabRounding = 4;

    style->WindowTitleAlign = ImVec2( 1.0f, 0.5f );
    style->WindowMenuButtonPosition = ImGuiDir_Right;

    style->DisplaySafeAreaPadding = ImVec2( 4, 4 );
}

// File Dialog //////////////////////////////////////////////////////////////////
/*

//
//
struct FileDialogOpenMap {

    char*                           key;
    bool                            value;

}; // struct FileDialogOpenMap

static string_hash( FileDialogOpenMap ) file_dialog_open_map = nullptr;

static raptor::Directory             directory;
static char                         filename[MAX_PATH];
static char                         last_path[MAX_PATH];
static char                         last_extension[16];
static bool                         scan_folder             = true;
static bool                         init                    = false;

static raptor::StringArray           files;
static raptor::StringArray           directories;

bool imgui_file_dialog_open( const char* button_name, const char* path, const char* extension ) {

    bool opened = string_hash_get( file_dialog_open_map, button_name );
    if ( ImGui::Button( button_name ) ) {
        opened = true;
    }

    bool selected = false;

    if ( opened && ImGui::Begin( "raptor_imgui_file_dialog", &opened, ImGuiWindowFlags_AlwaysAutoResize ) ) {

        ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 20, 20 ) );
        ImGui::Text( directory.path );
        ImGui::PopStyleVar();

        ImGui::Separator();

        ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 20, 4 ) );

        raptor::MemoryAllocator* allocator = raptor::memory_get_system_allocator();

        if ( !init ) {
            init = true;

            files.init( 10000, allocator );
            directories.init( 10000, allocator );

            string_hash_init_dynamic( file_dialog_open_map );

            filename[0] = 0;
            last_path[0] = 0;
            last_extension[0] = 0;
        }

        if ( strcmp( path, last_path ) != 0 ) {
            strcpy( last_path, path );

            raptor::file_open_directory( path, &directory );

            scan_folder = true;
        }

        if ( strcmp( extension, last_extension ) != 0 ) {
            strcpy( last_extension, extension );

            scan_folder = true;
        }

        // Search files
        if ( scan_folder ) {
            scan_folder = false;

            raptor::file_find_files_in_path( extension, directory.path, files, directories );
        }

        for ( size_t d = 0; d < directories.get_string_count(); ++d ) {

            const char* directory_name = directories.get_string( d );
            if ( ImGui::Selectable( directory_name, selected, ImGuiSelectableFlags_AllowDoubleClick ) ) {

                if ( strcmp( directory_name, ".." ) == 0 ) {
                    raptor::file_parent_directory( &directory );
                } else {
                    raptor::file_sub_directory( &directory, directory_name );
                }

                scan_folder = true;
            }
        }

        for ( size_t f = 0; f < files.get_string_count(); ++f ) {
            const char* file_name = files.get_string( f );
            if ( ImGui::Selectable( file_name, selected, ImGuiSelectableFlags_AllowDoubleClick ) ) {

                strcpy( filename, directory.path );
                filename[strlen( filename ) - 1] = 0;
                strcat( filename, file_name );

                selected = true;
                opened = false;
            }
        }

        ImGui::PopStyleVar();

        ImGui::End();
    }

    // Update opened map
    string_hash_put( file_dialog_open_map, button_name, opened );

    return selected;
}

const char * imgui_file_dialog_get_filename() {
    return filename;
}

bool imgui_path_dialog_open( const char* button_name, const char* path ) {
    bool opened = string_hash_get( file_dialog_open_map, button_name );
    if ( ImGui::Button( button_name ) ) {
        opened = true;
    }

    bool selected = false;

    if ( opened && ImGui::Begin( "raptor_imgui_file_dialog", &opened, ImGuiWindowFlags_AlwaysAutoResize ) ) {

        ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 20, 20 ) );
        ImGui::Text( directory.path );
        ImGui::PopStyleVar();

        ImGui::Separator();

        ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 20, 4 ) );

        raptor::MemoryAllocator* allocator = raptor::memory_get_system_allocator();

        if ( !init ) {
            init = true;

            files.init( 10000, allocator );
            directories.init( 10000, allocator );

            string_hash_init_dynamic( file_dialog_open_map );

            filename[0] = 0;
            last_path[0] = 0;
            last_extension[0] = 0;
        }

        if ( strcmp( path, last_path ) != 0 ) {
            strcpy( last_path, path );

            raptor::file_open_directory( path, &directory );

            scan_folder = true;
        }

        // Search files
        if ( scan_folder ) {
            scan_folder = false;

            raptor::file_find_files_in_path( ".", directory.path, files, directories );
        }

        for ( size_t d = 0; d < directories.get_string_count(); ++d ) {

            const char* directory_name = directories.get_string( d );
            if ( ImGui::Selectable( directory_name, selected, ImGuiSelectableFlags_AllowDoubleClick ) ) {

                if ( strcmp( directory_name, ".." ) == 0 ) {
                    raptor::file_parent_directory( &directory );
                } else {
                    raptor::file_sub_directory( &directory, directory_name );
                }

                scan_folder = true;
            }
        }

        if ( ImGui::Button( "Choose Current Folder" ) ) {
            strcpy( last_path, directory.path );
            // Remove the asterisk
            last_path[strlen( last_path ) - 1] = 0;

            selected = true;
            opened = false;
        }
        ImGui::SameLine();
        if ( ImGui::Button( "Cancel" ) ) {
            opened = false;
        }

        ImGui::PopStyleVar();

        ImGui::End();
    }

    // Update opened map
    string_hash_put( file_dialog_open_map, button_name, opened );

    return selected;
}

const char* imgui_path_dialog_get_path() {
    return last_path;
}

////////////////////////////////////////////////////////////
*/
// Usage:
//  static ExampleAppLog my_log;
//  my_log.AddLog("Hello %d world\n", 123);
//  my_log.Draw("title");
struct ExampleAppLog {
    ImGuiTextBuffer     Buf;
    ImGuiTextFilter     Filter;
    ImVector<int>       LineOffsets;        // Index to lines offset. We maintain this with AddLog() calls, allowing us to have a random access on lines
    bool                AutoScroll;     // Keep scrolling if already at the bottom

    ExampleAppLog() {
        AutoScroll = true;
        Clear();
    }

    void    Clear() {
        Buf.clear();
        LineOffsets.clear();
        LineOffsets.push_back( 0 );
    }

    void    AddLog( const char* fmt, ... ) IM_FMTARGS( 2 ) {
        int old_size = Buf.size();
        va_list args;
        va_start( args, fmt );
        Buf.appendfv( fmt, args );
        va_end( args );
        for ( int new_size = Buf.size(); old_size < new_size; old_size++ )
            if ( Buf[ old_size ] == '\n' )
                LineOffsets.push_back( old_size + 1 );
    }

    void    Draw( const char* title, bool* p_open = NULL ) {
        if ( !ImGui::Begin( title, p_open ) ) {
            ImGui::End();
            return;
        }

        // Options menu
        if ( ImGui::BeginPopup( "Options" ) ) {
            ImGui::Checkbox( "Auto-scroll", &AutoScroll );
            ImGui::EndPopup();
        }

        // Main window
        if ( ImGui::Button( "Options" ) )
            ImGui::OpenPopup( "Options" );
        ImGui::SameLine();
        bool clear = ImGui::Button( "Clear" );
        ImGui::SameLine();
        bool copy = ImGui::Button( "Copy" );
        ImGui::SameLine();
        Filter.Draw( "Filter", -100.0f );

        ImGui::Separator();
        ImGui::BeginChild( "scrolling", ImVec2( 0, 0 ), false, ImGuiWindowFlags_HorizontalScrollbar );

        if ( clear )
            Clear();
        if ( copy )
            ImGui::LogToClipboard();

        ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 0, 0 ) );
        const char* buf = Buf.begin();
        const char* buf_end = Buf.end();
        if ( Filter.IsActive() ) {
            // In this example we don't use the clipper when Filter is enabled.
            // This is because we don't have a random access on the result on our filter.
            // A real application processing logs with ten of thousands of entries may want to store the result of search/filter.
            // especially if the filtering function is not trivial (e.g. reg-exp).
            for ( int line_no = 0; line_no < LineOffsets.Size; line_no++ ) {
                const char* line_start = buf + LineOffsets[ line_no ];
                const char* line_end = ( line_no + 1 < LineOffsets.Size ) ? ( buf + LineOffsets[ line_no + 1 ] - 1 ) : buf_end;
                if ( Filter.PassFilter( line_start, line_end ) )
                    ImGui::TextUnformatted( line_start, line_end );
            }
        } else {
            // The simplest and easy way to display the entire buffer:
            //   ImGui::TextUnformatted(buf_begin, buf_end);
            // And it'll just work. TextUnformatted() has specialization for large blob of text and will fast-forward to skip non-visible lines.
            // Here we instead demonstrate using the clipper to only process lines that are within the visible area.
            // If you have tens of thousands of items and their processing cost is non-negligible, coarse clipping them on your side is recommended.
            // Using ImGuiListClipper requires A) random access into your data, and B) items all being the  same height,
            // both of which we can handle since we an array pointing to the beginning of each line of text.
            // When using the filter (in the block of code above) we don't have random access into the data to display anymore, which is why we don't use the clipper.
            // Storing or skimming through the search result would make it possible (and would be recommended if you want to search through tens of thousands of entries)
            ImGuiListClipper clipper;
            clipper.Begin( LineOffsets.Size );
            while ( clipper.Step() ) {
                for ( int line_no = clipper.DisplayStart; line_no < clipper.DisplayEnd; line_no++ ) {
                    const char* line_start = buf + LineOffsets[ line_no ];
                    const char* line_end = ( line_no + 1 < LineOffsets.Size ) ? ( buf + LineOffsets[ line_no + 1 ] - 1 ) : buf_end;
                    ImGui::TextUnformatted( line_start, line_end );
                }
            }
            clipper.End();
        }
        ImGui::PopStyleVar();

        if ( AutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() )
            ImGui::SetScrollHereY( 1.0f );

        ImGui::EndChild();
        ImGui::End();
    }
}; // struct ExampleAppLog

static ExampleAppLog        s_imgui_log;
static bool                 s_imgui_log_open = true;

static void imgui_print( const char* text ) {
    s_imgui_log.AddLog( "%s", text );
}

void imgui_log_init() {

    LogService::instance()->set_callback( &imgui_print );
}

void imgui_log_shutdown() {

    LogService::instance()->set_callback( nullptr );
}

void imgui_log_draw() {
    s_imgui_log.Draw( "Log", &s_imgui_log_open );
}


// Plot with ringbuffer

// https://github.com/leiradel/ImGuiAl
template<typename T, size_t L>
class Sparkline {
public:
    Sparkline() {
        setLimits( 0, 1 );
        clear();
    }

    void setLimits( T const min, T const max ) {
        _min = static_cast< float >( min );
        _max = static_cast< float >( max );
    }

    void add( T const value ) {
        _offset = ( _offset + 1 ) % L;
        _values[ _offset ] = value;
    }

    void clear() {
        memset( _values, 0, L * sizeof( T ) );
        _offset = L - 1;
    }

    void draw( char const* const label = "", ImVec2 const size = ImVec2() ) const {
        char overlay[ 32 ];
        print( overlay, sizeof( overlay ), _values[ _offset ] );

        ImGui::PlotLines( label, getValue, const_cast< Sparkline* >( this ), L, 0, overlay, _min, _max, size );
    }

protected:
    float _min, _max;
    T _values[ L ];
    size_t _offset;

    static float getValue( void* const data, int const idx ) {
        Sparkline const* const self = static_cast< Sparkline* >( data );
        size_t const index = ( idx + self->_offset + 1 ) % L;
        return static_cast< float >( self->_values[ index ] );
    }

    static void print( char* const buffer, size_t const bufferLen, int const value ) {
        snprintf( buffer, bufferLen, "%d", value );
    }

    static void print( char* const buffer, size_t const bufferLen, double const value ) {
        snprintf( buffer, bufferLen, "%f", value );
    }
};

static Sparkline<f32, 100> s_fps_line;

void imgui_fps_init() {
    s_fps_line.clear();
    s_fps_line.setLimits( 0.0f, 33.f );
}

void imgui_fps_shutdown() {
}

void imgui_fps_add( f32 dt ) {
    s_fps_line.add( dt );
}

void imgui_fps_draw() {
    s_fps_line.draw( "FPS", { 0,100 } );
}
} // namespace raptor


// Imgui helpers //////////////////////////////////////////////////////////

namespace ImGui {

bool SliderUint( const char* label, u32* v, u32 v_min, u32 v_max, const char* format, ImGuiSliderFlags flags ) {
    return SliderScalar( label, ImGuiDataType_U32, v, &v_min, &v_max, format, flags );
}

bool SliderUint2( const char* label, u32 v[ 2 ], u32 v_min, u32 v_max, const char* format, ImGuiSliderFlags flags ) {
    return SliderScalarN( label, ImGuiDataType_U32, v, 2, &v_min, &v_max, format, flags );
}

bool SliderUint3( const char* label, u32 v[ 3 ], u32 v_min, u32 v_max, const char* format, ImGuiSliderFlags flags ) {
    return SliderScalarN( label, ImGuiDataType_U32, v, 3, &v_min, &v_max, format, flags );
}

bool SliderUint4( const char* label, u32 v[ 4 ], u32 v_min, u32 v_max, const char* format, ImGuiSliderFlags flags ) {
    return SliderScalarN( label, ImGuiDataType_U32, v, 4, &v_min, &v_max, format, flags );
}

} // namespace ImGui
