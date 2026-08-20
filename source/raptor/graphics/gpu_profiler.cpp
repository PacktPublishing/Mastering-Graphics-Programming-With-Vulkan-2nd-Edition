#include "gpu_profiler.hpp"

#include "foundation/hash_map.hpp"
#include "foundation/numerics.hpp"
#include "foundation/color.hpp"

#include "graphics/command_buffer.hpp"
#include "graphics/raptor_imgui.hpp"

#include "external/imgui/imgui.h"
#include <cmath>
#include <algorithm>
#include <stdio.h>

namespace raptor {

// GPU task names to colors
raptor::FlatHashMap<u64, u32>   name_to_color;

static u32      initial_frames_paused = 15;
static i32      s_pinned_frame = -1;
static bool     s_isolate_focused_marker = false;
static bool     s_show_frame_timeline = true;
static f32      s_timeline_height = 220.0f;


static inline ImU32 apply_alpha( ImU32 color, f32 alpha01 ) {
    u32 a = (u32)( raptor::clamp( alpha01, 0.0f, 1.0f ) * 255u );
    return ( color & 0x00ffffffu ) | ( a << 24 );
}

void GpuVisualProfiler::init( Allocator* allocator_, f64 gpu_timestamp_frequency_, u32 max_frames_, u32 max_queries_per_frame_ ) {

    allocator = allocator_;
    max_frames = max_frames_;
    max_queries_per_frame = max_queries_per_frame_;

    timestamps.init( allocator, max_frames * max_queries_per_frame, max_frames * max_queries_per_frame );
    per_frame_active.init( allocator, max_frames, max_frames );

    max_duration = 16.666f;
    current_frame = 0;
    min_time = max_time = average_time = 0.f;
    paused = false;
    pipeline_statistics = nullptr;

    gpu_timestamp_frequency = gpu_timestamp_frequency_;

    memset( per_frame_active.data, 0, sizeof(u16) * max_frames );

    name_to_color.init( allocator, 16 );
    name_to_color.set_default_value( u32_max );

    reset_focused_marker_stats();
}

void GpuVisualProfiler::shutdown() {

    name_to_color.shutdown();

    timestamps.shutdown();
    per_frame_active.shutdown();
}

static f32 s_framebuffer_pixel_count = 0.f;

void GpuVisualProfiler::update( GpuDevice& gpu ) {

    gpu.set_gpu_timestamps_enable( !paused );

    if ( !gpu.gpu_profiler->resolved_frame_valid ) {
        return;
    }

    if ( initial_frames_paused ) {
        --initial_frames_paused;
        return;
    }

    if ( paused && !gpu.resized ) {
        return;
    }

    const u32 frame_to_resolve = gpu.gpu_profiler->resolved_frame_index;

    // Collect timestamps
    u32 active_timestamps = gpu.gpu_profiler->resolve( frame_to_resolve, &timestamps[ max_queries_per_frame * current_frame ] );
    per_frame_active[ current_frame ] = ( u16 )active_timestamps;

    // Collect pipeline statistics
    pipeline_statistics = &gpu.gpu_profiler->frame_pipeline_statistics;

    s_framebuffer_pixel_count = f32( gpu.swapchain_width * gpu.swapchain_height );

    // Get colors
    for ( u32 i = 0; i < active_timestamps; ++i ) {
        GPUTimeQuery& timestamp = timestamps[ max_queries_per_frame * current_frame + i ];

        const u64 hashed_name = raptor::hash_calculate( timestamp.name );
        u32 color_index = name_to_color.get( hashed_name );
        // No entry found, add new color
        if ( color_index == u32_max ) {

            color_index = ( u32 )name_to_color.size;
            name_to_color.insert( hashed_name, color_index );
        }

        if ( hashed_name == focused_marker_hash ) {
            update_focused_marker_stats( ( f32 )timestamp.elapsed_ms );
        }

        timestamp.color = raptor::Color::get_distinct_color( color_index );
    }

    current_frame = ( current_frame + 1 ) % max_frames;

    // Reset Min/Max/Average after few frames
    if ( current_frame == 0 ) {
        max_time = -FLT_MAX;
        min_time = FLT_MAX;
        average_time = 0.f;
    }
}

struct DrawEvent {
    bool begin;
    u16 id;
    u16 depth;
    u64 timestamp;
};

void GpuVisualProfiler::draw_vertical_graph( f64& new_average, i32& hovered_frame,
                                             const ImVec2& origin, const ImVec2& size,
                                             f32 row_h, f32 indent_px,
                                             bool isolate_focused, u64 focused_marker_hash,
                                             bool& out_clicked_frame, i32& out_clicked_frame_index ) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    // Reset outputs
    hovered_frame = -1;
    out_clicked_frame = false;

    const ImVec2 cursor_pos = origin;
    const ImVec2 canvas_size = size;

    // Padding based on style
    ImGuiStyle& style = ImGui::GetStyle();
    const f32 pad_x = style.FramePadding.x;
    const f32 pad_y = style.FramePadding.y;

    // Graph area inside the child
    const f32 widget_height = raptor::max( row_h * 6.0f, canvas_size.y - pad_y * 2.0f );

    // Compute graph width
    const f32 graph_width = raptor::max( 1.0f, canvas_size.x - pad_x * 2.0f );

    // Column width per frame (>= 1px)
    const u32 rect_width = raptor::max( 1u, ceilu32( graph_width / max_frames ) );
    i32 rect_x = ceili32( graph_width - rect_width );

    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse_pos = io.MousePos;

    static char buf[ 128 ];

    // Reference lines (top and mid)
    sprintf( buf, "%3.4fms", max_duration );
    draw_list->AddText( { cursor_pos.x + pad_x, cursor_pos.y + pad_y }, 0xff0000ff, buf );
    draw_list->AddLine( { cursor_pos.x + pad_x + rect_width, cursor_pos.y + pad_y },
                        { cursor_pos.x + pad_x + graph_width, cursor_pos.y + pad_y }, 0xff0000ff );

    sprintf( buf, "%3.4fms", max_duration * 0.5f );
    draw_list->AddText( { cursor_pos.x + pad_x, cursor_pos.y + pad_y + widget_height * 0.5f }, 0xff00ffff, buf );
    draw_list->AddLine( { cursor_pos.x + pad_x + rect_width, cursor_pos.y + pad_y + widget_height * 0.5f },
                        { cursor_pos.x + pad_x + graph_width, cursor_pos.y + pad_y + widget_height * 0.5f }, 0xff00ffff );

    // Draw frames
    for ( u32 i = 0; i < max_frames; ++i ) {
        const u32 frame_index = ( current_frame + max_frames - 1 - i ) % max_frames;

        const f32 frame_x = cursor_pos.x + pad_x + rect_x;

        GPUTimeQuery* frame_timestamps = &timestamps[ frame_index * max_queries_per_frame ];
        f32 frame_time = (f32)frame_timestamps[ 0 ].elapsed_ms;
        frame_time = raptor::clamp( frame_time, 0.00001f, 1000.f );

        new_average += frame_time;
        min_time = raptor::min( min_time, frame_time );
        max_time = raptor::max( max_time, frame_time );

        f32 current_height = cursor_pos.y + pad_y;

        // Stack: only depth 1 using margins scaled by DPI
        const f32 width_margin = raptor::max( 1.0f, style.ItemInnerSpacing.x * 0.5f );

        for ( u32 j = 0; j < per_frame_active[ frame_index ]; ++j ) {
            const GPUTimeQuery& timestamp = frame_timestamps[ j ];
            if ( timestamp.depth != 1 ) {
                continue;
            }

            // Focus filtering
            ImU32 color = timestamp.color;
            if ( focused_marker_hash != 0 ) {
                const u64 h = raptor::hash_calculate( timestamp.name );
                const bool is_focused = ( h == focused_marker_hash );

                if ( isolate_focused ) {
                    if ( !is_focused ) {
                        continue;
                    }
                } else {
                    if ( !is_focused ) {
                        color = apply_alpha( color, 0.15f );
                    }
                }
            }

            const f32 h = (f32)timestamp.elapsed_ms / max_duration * widget_height;
            const ImVec2 rect_min{ frame_x + width_margin, current_height + widget_height - h };
            const ImVec2 rect_max{ frame_x + rect_width - width_margin, current_height + widget_height };
            draw_list->AddRectFilled( rect_min, rect_max, color );

            current_height -= h;
        }

        // Hover selection
        if ( mouse_pos.x >= frame_x && mouse_pos.x < frame_x + rect_width &&
             mouse_pos.y >= cursor_pos.y + pad_y && mouse_pos.y < cursor_pos.y + pad_y + widget_height ) {

            draw_list->AddRectFilled( { frame_x, cursor_pos.y + pad_y + widget_height },
                                      { frame_x + rect_width, cursor_pos.y + pad_y }, 0x0fffffff );

            ImGui::SetTooltip( "(%u): %0.3f ms", frame_index, frame_time );
            hovered_frame = (i32)frame_index;

            if ( ImGui::IsMouseClicked( ImGuiMouseButton_Left ) ) {
                out_clicked_frame = true;
                out_clicked_frame_index = (i32)frame_index;
            }
        }

        // Column separator
        draw_list->AddLine( { frame_x, cursor_pos.y + pad_y + widget_height },
                            { frame_x, cursor_pos.y + pad_y }, 0x0fffffff );

        rect_x -= rect_width;
    }

    average_time = (f32)( new_average / (f64)max_frames );
}

static cstring gpu_queue_type_name( CommandQueueType queue_type ) {
    switch ( queue_type ) {
        case CommandQueueType::Graphics: return "Graphics";
        case CommandQueueType::Compute:  return "Compute";
        //case CommandQueueType::Transfer: return "Transfer";
        default:                         return "Unknown";
    }
}

static char gpu_queue_type_char( CommandQueueType queue_type ) {
    switch ( queue_type ) {
        case CommandQueueType::Graphics: return 'G';
        case CommandQueueType::Compute:  return 'C';
        //case CommandQueueType::Transfer: return 'T';
        default:                         return '?';
    }
}

static u32 gpu_queue_timeline_row( CommandQueueType queue_type ) {
    switch ( queue_type ) {
        case CommandQueueType::Graphics: return 0;
        case CommandQueueType::Compute:  return 1;
        //case CommandQueueType::Transfer: return 2;
        default:                         return 3;
    }
}

struct GpuTimelineBar {
    u16                         query_index;
    u16                         row;
    f32                         begin_ms;
    f32                         end_ms;
};

void GpuVisualProfiler::draw_horizontal_graph( i32 frame_index, const ImVec2& origin, const ImVec2& size, f32 row_h,
                                               u64 focused_marker_hash, bool isolate_focused ) {

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImGuiStyle& style = ImGui::GetStyle();

    const f32 pad_x = style.FramePadding.x;
    const f32 pad_y = style.FramePadding.y;
    const f32 top_gutter_h = row_h + style.ItemSpacing.y;

    const f32 content_x0 = origin.x + pad_x;
    const f32 content_y0 = origin.y + pad_y + top_gutter_h;
    const f32 content_w = raptor::max( 1.0f, size.x - pad_x * 2.0f );
    const f32 content_h = raptor::max( 1.0f, size.y - pad_y * 2.0f - top_gutter_h );

    GPUTimeQuery* frame_timestamps = &timestamps[ frame_index * max_queries_per_frame ];
    const u16 timestamp_count = per_frame_active[ frame_index ];

    if ( timestamp_count == 0 ) {
        return;
    }

    // Each queue gets its own local time origin.
    const u64 invalid_timestamp = ~0ull;

    u64 queue_begin[ k_command_queue_count ];
    u64 queue_end[ k_command_queue_count ];

    for ( u32 q = 0; q < k_command_queue_count; ++q ) {
        queue_begin[ q ] = invalid_timestamp;
        queue_end[ q ] = 0;
    }

    // Find the timestamp range of every queue. Depth 0 command-buffer markers
    // naturally contribute to this range but will not be drawn.
    for ( u32 i = 0; i < timestamp_count; ++i ) {
        const GPUTimeQuery& timestamp = frame_timestamps[ i ];
        const u32 queue_index = ( u32 )timestamp.queue_type;

        if ( queue_index >= k_command_queue_count || timestamp.end <= timestamp.begin ) {
            continue;
        }

        queue_begin[ queue_index ] = raptor::min( queue_begin[ queue_index ], timestamp.begin );
        queue_end[ queue_index ] = raptor::max( queue_end[ queue_index ], timestamp.end );
    }

    // Use the same duration scale for all queues.
    f32 max_timeline_ms = 0.0f;

    for ( u32 q = 0; q < k_command_queue_count; ++q ) {
        if ( queue_begin[ q ] == invalid_timestamp || queue_end[ q ] <= queue_begin[ q ] ) {
            continue;
        }

        const f32 queue_duration_ms = ( f32 )( queue_end[ q ] - queue_begin[ q ] ) * ( f32 )gpu_timestamp_frequency;
        max_timeline_ms = raptor::max( max_timeline_ms, queue_duration_ms );
    }

    if ( max_timeline_ms <= 0.0f ) {
        return;
    }

    const u32 visible_depth_count = raptor::max( 1u, max_visible_depth );
    const u32 total_rows = k_command_queue_count * visible_depth_count;

    const f32 label_w = raptor::max( 100.0f, ImGui::CalcTextSize( "Graphics D4" ).x + style.ItemSpacing.x * 2.0f );
    const f32 timeline_x0 = content_x0 + label_w;
    const f32 timeline_w = raptor::max( 1.0f, content_w - label_w );
    const f32 pixels_per_ms = timeline_w / raptor::max( max_timeline_ms, 0.0001f );

    // Compress rows if the timeline child is too short.
    const f32 lane_h = raptor::min( row_h, content_h / ( f32 )total_rows );
    const f32 lane_gap = style.ItemSpacing.y * 0.25f;
    const f32 rect_h = raptor::max( 1.0f, lane_h - lane_gap );

    const ImU32 separator_color = ImGui::GetColorU32( ImGuiCol_Separator );
    const ImU32 text_color = ImGui::GetColorU32( ImGuiCol_Text );
    const ImU32 subtle_color = ImGui::GetColorU32( ImGuiCol_TextDisabled );

    char text_buffer[ 128 ];

    // Time labels.
    draw_list->AddText( { timeline_x0, origin.y + pad_y }, text_color, "0 ms" );

    snprintf( text_buffer, sizeof( text_buffer ), "%.2f ms", max_timeline_ms * 0.5f );
    ImVec2 text_size = ImGui::CalcTextSize( text_buffer );
    draw_list->AddText( { timeline_x0 + timeline_w * 0.5f - text_size.x * 0.5f, origin.y + pad_y }, text_color, text_buffer );

    snprintf( text_buffer, sizeof( text_buffer ), "%.2f ms", max_timeline_ms );
    text_size = ImGui::CalcTextSize( text_buffer );
    draw_list->AddText( { timeline_x0 + timeline_w - text_size.x, origin.y + pad_y }, text_color, text_buffer );

    // Timeline reference lines.
    draw_list->AddLine( { timeline_x0, content_y0 }, { timeline_x0, content_y0 + content_h }, separator_color );
    draw_list->AddLine( { timeline_x0 + timeline_w * 0.5f, content_y0 },
                        { timeline_x0 + timeline_w * 0.5f, content_y0 + content_h }, separator_color );
    draw_list->AddLine( { timeline_x0 + timeline_w, content_y0 },
                        { timeline_x0 + timeline_w, content_y0 + content_h }, separator_color );

    // Queue/depth labels and row separators.
    for ( u32 q = 0; q < k_command_queue_count; ++q ) {
        const CommandQueueType queue_type = ( CommandQueueType )q;
        const u32 queue_row = gpu_queue_timeline_row( queue_type );

        for ( u32 depth = 1; depth <= visible_depth_count; ++depth ) {
            const u32 row = queue_row * visible_depth_count + depth - 1;
            const f32 row_y = content_y0 + ( f32 )row * lane_h;

            static char row_label[ 64 ];
            snprintf( row_label, sizeof( row_label ), "%s D%u", gpu_queue_type_name( queue_type ), depth );

            draw_list->AddText( { content_x0, row_y }, depth == 1 ? text_color : subtle_color, row_label );
            draw_list->AddLine( { timeline_x0, row_y + lane_h },
                                { timeline_x0 + timeline_w, row_y + lane_h }, subtle_color );
        }
    }

    const ImVec2 mouse_position = ImGui::GetIO().MousePos;

    // Draw all visible markers directly from the resolved timestamp array.
    for ( u32 i = 0; i < timestamp_count; ++i ) {
        const GPUTimeQuery& timestamp = frame_timestamps[ i ];

        // Depth 0 is the command-buffer marker: use it for the queue range,
        // but do not draw it as a normal profiler row.
        if ( timestamp.depth == 0 || timestamp.depth > visible_depth_count ) {
            continue;
        }

        if ( timestamp.end <= timestamp.begin || timestamp.elapsed_ms <= 0.0 ) {
            continue;
        }

        const u32 queue_index = ( u32 )timestamp.queue_type;

        if ( queue_index >= k_command_queue_count || queue_begin[ queue_index ] == invalid_timestamp ) {
            continue;
        }

        const u32 queue_row = gpu_queue_timeline_row( timestamp.queue_type );
        const u32 row = queue_row * visible_depth_count + timestamp.depth - 1;

        const f32 begin_ms = ( f32 )( timestamp.begin - queue_begin[ queue_index ] ) * ( f32 )gpu_timestamp_frequency;
        const f32 end_ms = ( f32 )( timestamp.end - queue_begin[ queue_index ] ) * ( f32 )gpu_timestamp_frequency;

        f32 graph_begin = raptor::clamp( begin_ms * pixels_per_ms, 0.0f, timeline_w );
        f32 graph_end = raptor::clamp( end_ms * pixels_per_ms, 0.0f, timeline_w );

        if ( graph_end <= graph_begin ) {
            graph_end = raptor::min( timeline_w, graph_begin + 1.0f );
        }

        const u64 marker_hash = raptor::hash_calculate( timestamp.name );
        ImU32 color = timestamp.color;

        if ( focused_marker_hash != 0 ) {
            const bool is_focused = marker_hash == focused_marker_hash;

            if ( isolate_focused && !is_focused ) {
                continue;
            }

            if ( !isolate_focused && !is_focused ) {
                color = apply_alpha( color, 0.15f );
            }
        }

        const f32 row_y = content_y0 + ( f32 )row * lane_h;
        const ImVec2 rect_min{ timeline_x0 + graph_begin, row_y };
        const ImVec2 rect_max{ timeline_x0 + graph_end, row_y + rect_h };

        draw_list->AddRectFilled( rect_min, rect_max, color );

        const bool is_hovered = mouse_position.x >= rect_min.x && mouse_position.x < rect_max.x &&
            mouse_position.y >= rect_min.y && mouse_position.y < rect_max.y;

        if ( is_hovered ) {
            ImGui::SetTooltip( "[%c] %s\nDuration: %.3f ms\nLocal: %.3f - %.3f ms\nDepth: %u",
                               gpu_queue_type_char( timestamp.queue_type ), timestamp.name, timestamp.elapsed_ms,
                               begin_ms, end_ms, timestamp.depth );

            if ( ImGui::IsMouseClicked( ImGuiMouseButton_Left ) ) {
                const bool already_selected = this->focused_marker_hash == marker_hash;

                this->focused_marker_hash = already_selected ? 0 : marker_hash;
                focused_marker_name = already_selected ? nullptr : timestamp.name;
                reset_focused_marker_stats();
            }
        }
    }

    // Preserve existing full-frame min/max statistics.
    for ( u32 i = 0; i < max_frames; ++i ) {
        const u32 history_frame_index = ( current_frame + max_frames - 1 - i ) % max_frames;
        GPUTimeQuery* history_frame_timestamps = &timestamps[ history_frame_index * max_queries_per_frame ];

        f32 frame_time = ( f32 )history_frame_timestamps[ 0 ].elapsed_ms;
        frame_time = raptor::clamp( frame_time, 0.00001f, 1000.0f );

        min_time = raptor::min( min_time, frame_time );
        max_time = raptor::max( max_time, frame_time );
    }
}

void GpuVisualProfiler::reset_focused_marker_stats() {

    focused_marker_times.clear();

    focused_marker_times_average = 0.f;
    focused_marker_times_write_index = 0;
    focused_marker_min_time = FLT_MAX;
    focused_marker_max_time = -FLT_MAX;
}

void GpuVisualProfiler::update_focused_marker_stats( f32 time_ms ) {

    if ( focused_marker_times.size < k_focused_marker_max_samples ) {
        focused_marker_times.push( time_ms );
    } else {
        focused_marker_times[ focused_marker_times_write_index ] = time_ms;
    }

    focused_marker_times_write_index = ( focused_marker_times_write_index + 1 ) % k_focused_marker_max_samples;

    // Compute statistics, loop is small so should be fine.
    focused_marker_times_average = 0.f;

    focused_marker_min_time = FLT_MAX;
    focused_marker_max_time = -FLT_MAX;

    for ( u32 i = 0; i < focused_marker_times.size; ++i ) {
        const f32 t = focused_marker_times[ i ];
        focused_marker_times_average += t;

        focused_marker_min_time = raptor::min( focused_marker_min_time, t );
        focused_marker_max_time = raptor::max( focused_marker_max_time, t );
    }

    focused_marker_times_average /= ( f32 )focused_marker_times.size;
   
}

void GpuVisualProfiler::imgui_draw() {
    if ( initial_frames_paused ) {
        return;
    }

    ImGuiStyle& style = ImGui::GetStyle();
    const f32 line_h = ImGui::GetTextLineHeightWithSpacing();

    ImGuiIO& io = ImGui::GetIO();

    const f32 splitter_h = raptor::max( 6.0f, style.FramePadding.y * 2.0f );

    // Clamp timeline height to something sane for current window size.
    const f32 timeline_min_h = line_h * 6.0f;
    const f32 timeline_max_h = raptor::max( timeline_min_h, ImGui::GetContentRegionAvail().y * 0.75f );

    // Footer height (DPI aware)
    // This is the "controls + stats + pipeline stats" region below the graph.
    // Tune footer_lines if you add/remove UI rows.
    const f32 footer_lines = 10.0f; // stats row + separators + pause + graph max + depth + pipeline stats + units
    f32 footer_h = footer_lines * line_h + style.ItemSpacing.y * 6.0f;

    // Reserve height for the overview area (graph + inspector)
    f32 avail_h = ImGui::GetContentRegionAvail().y;

    f32 timeline_h = s_show_frame_timeline ? s_timeline_height : 0.0f;
    f32 timeline_block_h = s_show_frame_timeline ? splitter_h + style.ItemSpacing.y + timeline_h + style.ItemSpacing.y : 0.0f;

    f32 overview_h = avail_h - footer_h - timeline_block_h;
    const f32 min_overview_h = line_h * 8.0f;
    if ( overview_h < min_overview_h ) {
        // If window is too small, steal from timeline.
        const f32 deficit = ( min_overview_h - overview_h );
        s_timeline_height = raptor::max( timeline_min_h, s_timeline_height - deficit );
        timeline_h = s_timeline_height;
        timeline_block_h = s_show_frame_timeline ? ( splitter_h + style.ItemSpacing.y + timeline_h + style.ItemSpacing.y ) : 0.0f;
        overview_h = avail_h - footer_h - timeline_block_h;
        overview_h = raptor::max( overview_h, min_overview_h );
    }

    // Layout: resizable split (graph | inspector)
    const ImGuiTableFlags table_flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV;

    // NOTE: could be wrapped in CollapsingHeader ?
    if ( ImGui::BeginTable( "gpu_profiler_overview", 2, table_flags ) ) {

        // Graph column
        ImGui::TableSetupColumn( "Graph", ImGuiTableColumnFlags_WidthStretch, 0.75f );
        // Inspector column
        ImGui::TableSetupColumn( "Inspector", ImGuiTableColumnFlags_WidthStretch, 0.25f );

        ImGui::TableNextRow();

        // Graph
        ImGui::TableSetColumnIndex( 0 );
        ImGui::BeginChild( "GpuProfiler_GraphRegion", ImVec2( 0.0f, overview_h ), true, ImGuiWindowFlags_NoScrollbar );

        f64 new_average = 0.0;

        // Draw inside this child rect
        ImVec2 graph_origin = ImGui::GetCursorScreenPos();
        ImVec2 graph_size = ImGui::GetContentRegionAvail();

        // Use DPI aware row height for all graph spacing decisions
        const f32 row_h = line_h;
        const f32 indent_px = raptor::max( 2.0f, style.IndentSpacing * 0.25f );

        i32 hovered_frame = -1;
        bool clicked_frame = false;
        i32 clicked_frame_index = -1;

        draw_vertical_graph( new_average, hovered_frame, graph_origin, graph_size, row_h, indent_px,
                             s_isolate_focused_marker, focused_marker_hash,
                             clicked_frame, clicked_frame_index );

        if ( clicked_frame ) {
            // Toggle pin if clicking same frame again
            if ( s_pinned_frame == clicked_frame_index ) {
                s_pinned_frame = -1;
            }
            else {
                s_pinned_frame = clicked_frame_index;
            }
        }

        const i32 last_frame = ( (i32)current_frame + (i32)max_frames - 1 ) % (i32)max_frames;

        i32 selected_frame = -1;
        if ( s_pinned_frame != -1 ) {
            selected_frame = s_pinned_frame;
        }
        else if ( hovered_frame != -1 ) {
            selected_frame = hovered_frame;
        }
        else {
            selected_frame = last_frame;
        }

        // Reserve the space so ImGui doesn't overlap subsequent items.
        ImGui::Dummy( graph_size );
        ImGui::EndChild();

        // Inspector
        ImGui::TableSetColumnIndex( 1 );
        ImGui::BeginChild( "GpuProfiler_InspectorRegion", ImVec2( 0.0f, overview_h ), true, ImGuiWindowFlags_NoScrollbar );

        // Default to last frame if nothing is selected by hover.
        selected_frame = selected_frame == -1 ? ( ( current_frame + max_frames - 1 ) % max_frames ) : selected_frame;

        if ( selected_frame >= 0 ) {
            ImGui::Text( "Frame %d%s", selected_frame, ( s_pinned_frame == selected_frame ? " (pinned)" : "" ) );
            ImGui::SameLine();
            if ( ImGui::SmallButton( "Unpin" ) ) s_pinned_frame = -1;

            ImGui::Separator();

            ImGui::Checkbox( "Timeline", &s_show_frame_timeline );
            ImGui::SameLine();
            ImGui::Checkbox( "Isolate Focus", &s_isolate_focused_marker );
            ImGui::SameLine();
            if ( ImGui::SmallButton( "Clear Focus" ) ) {
                focused_marker_hash = 0;
                focused_marker_name = nullptr;
                reset_focused_marker_stats();
            }

            ImGui::Separator();

            // Selected marker statistics
            if ( focused_marker_hash != 0 && focused_marker_times.size > 0 ) {

                ImGui::Text( "Selected: %s", focused_marker_name );

                if ( ImGui::BeginTable( "FocusedMarkerStatistics", 3,
                    ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV ) ) {

                    ImGui::TableNextColumn();
                    ImGui::TextDisabled( "Average" );
                    ImGui::Text( "%.3f ms", focused_marker_times_average );

                    ImGui::TableNextColumn();
                    ImGui::TextDisabled( "Min" );
                    ImGui::Text( "%.3f ms", focused_marker_min_time );

                    ImGui::TableNextColumn();
                    ImGui::TextDisabled( "Max" );
                    ImGui::Text( "%.3f ms", focused_marker_max_time );

                    ImGui::EndTable();
                }

                //ImGui::TextDisabled( "Samples %u / %u", s_focused_marker_stats.sample_count, s_statistics_sample_window );

                ImGui::Separator();
            }

            GPUTimeQuery* frame_timestamps = &timestamps[ selected_frame * max_queries_per_frame ];

            // Build a small list of depth==1 markers for selection.
            // (Later we can sort by elapsed_ms desc; keep simple first.)
            for ( u32 j = 0; j < per_frame_active[ selected_frame ]; ++j ) {
                const GPUTimeQuery& t = frame_timestamps[ j ];
                if ( t.depth != 1 ) {
                    continue;
                }

                const u64 h = raptor::hash_calculate( t.name );
                const bool is_selected = ( focused_marker_hash == h );

                // Small color swatch + selectable row
                ImGui::PushID( (int)h );
                ImGui::ColorButton( "##c", ImGui::ColorConvertU32ToFloat4( t.color ),
                                    ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                                    ImVec2( line_h * 0.6f, line_h * 0.6f ) );
                ImGui::SameLine();

                char label[ 256 ];
                // Use "###select" to make the label unique for ImGui but not visible.
                sprintf( label, "%6.3f ms  [%c]  %s###select", t.elapsed_ms,
                         ( t.queue_type == CommandQueueType::Graphics ? 'G' : ( t.queue_type == CommandQueueType::Compute ? 'C' : 'T' ) ),
                         t.name );

                if ( ImGui::Selectable( label, is_selected ) ) {
                    const u64 new_hash = is_selected ? 0 : h;

                    if ( new_hash != focused_marker_hash ) {
                        focused_marker_hash = new_hash;
                        focused_marker_name = t.name;
                        reset_focused_marker_stats();
                    }
                }
                ImGui::PopID();
            }

        }

        ImGui::EndChild();
        ImGui::EndTable();

        if ( s_show_frame_timeline ) {
            // Splitter bar (drag up/down to resize timeline)
            ImGui::Dummy( ImVec2( 0.0f, style.ItemSpacing.y * 0.5f ) );

            ImVec2 splitter_pos = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton( "GpuProfiler_TimelineSplitter", ImVec2( -1.0f, splitter_h ) );

            // Nice cursor when hovering
            if ( ImGui::IsItemHovered() || ImGui::IsItemActive() ) {
                ImGui::SetMouseCursor( ImGuiMouseCursor_ResizeNS );
            }

            // Draw a visible splitter line
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 col = ImGui::GetColorU32( ImGuiCol_Separator );
            dl->AddRectFilled( splitter_pos, ImVec2( splitter_pos.x + ImGui::GetContentRegionAvail().x, splitter_pos.y + splitter_h ), col );

            // Drag logic
            if ( ImGui::IsItemActive() ) {
                s_timeline_height = raptor::clamp( s_timeline_height - io.MouseDelta.y, timeline_min_h, timeline_max_h );
            }

            ImGui::Dummy( ImVec2( 0.0f, style.ItemSpacing.y * 0.5f ) );

            ImGui::BeginChild( "GpuProfiler_TimelineRegion", ImVec2( 0.0f, s_timeline_height ), true, ImGuiWindowFlags_NoScrollbar );

            ImVec2 tl_origin = ImGui::GetCursorScreenPos();
            ImVec2 tl_size = ImGui::GetContentRegionAvail();

            draw_horizontal_graph( selected_frame, tl_origin, tl_size, line_h,
                                   focused_marker_hash, s_isolate_focused_marker );

            ImGui::Dummy( tl_size );
            ImGui::EndChild();
        }

    }

    // Footer controls and stats
    ImGui::Separator();

    ImGui::SetNextItemWidth( 120.0f );
    ImGui::LabelText( "", "Max %3.4fms", max_time );
    ImGui::SameLine();
    ImGui::SetNextItemWidth( 120.0f );
    ImGui::LabelText( "", "Min %3.4fms", min_time );
    ImGui::SameLine();
    ImGui::SetNextItemWidth( 120.0f );
    ImGui::LabelText( "", "Ave %3.4fms", average_time );

    ImGui::Separator();
    ImGui::Checkbox( "Pause", &paused );

    static const char* items[] = { "200ms", "100ms", "66ms", "33ms", "16ms", "8ms", "4ms" };
    static const f32 max_durations[] = { 200.f, 100.f, 66.f, 33.f, 16.f, 8.f, 4.f };

    static int max_duration_index = 4;
    if ( ImGui::Combo( "Graph Max", &max_duration_index, items, IM_ARRAYSIZE( items ) ) ) {
        max_duration = max_durations[ max_duration_index ];
    }

    ImGui::SliderUint( "Max Depth", &max_visible_depth, 1, 4 );

    ImGui::Separator();

    static const char* stat_unit_names[] = { "Normal", "Kilo", "Mega" };
    static const char* stat_units[] = { "", "K", "M" };
    static const f32 stat_unit_multipliers[] = { 1.0f, 1000.f, 1000000.f };

    static int stat_unit_index = 1;
    const f32 stat_unit_multiplier = stat_unit_multipliers[ stat_unit_index ];
    cstring stat_unit_name = stat_units[ stat_unit_index ];

    if ( pipeline_statistics ) {
        f32 stat_values[ GpuPipelineStatistics::Count ];
        for ( u32 i = 0; i < GpuPipelineStatistics::Count; ++i ) {
            stat_values[ i ] = pipeline_statistics->statistics[ i ] / stat_unit_multiplier;
        }

        ImGui::Text( "Vertices %0.2f%s, Primitives %0.2f%s",
                     stat_values[ GpuPipelineStatistics::VerticesCount ], stat_unit_name,
                     stat_values[ GpuPipelineStatistics::PrimitiveCount ], stat_unit_name );

        ImGui::Text( "Clipping: Invocations %0.2f%s, Visible Primitives %0.2f%s, Visible Perc %3.1f",
                     stat_values[ GpuPipelineStatistics::ClippingInvocations ], stat_unit_name,
                     stat_values[ GpuPipelineStatistics::ClippingPrimitives ], stat_unit_name,
                     stat_values[ GpuPipelineStatistics::ClippingInvocations ] > 0.0f ?
                     ( stat_values[ GpuPipelineStatistics::ClippingPrimitives ] / stat_values[ GpuPipelineStatistics::ClippingInvocations ] * 100.0f ) : 0.0f );

        ImGui::Text( "Invocations: VS %0.2f%s, FS %0.2f%s, CS %0.2f%s",
                     stat_values[ GpuPipelineStatistics::VertexShaderInvocations ], stat_unit_name,
                     stat_values[ GpuPipelineStatistics::FragmentShaderInvocations ], stat_unit_name,
                     stat_values[ GpuPipelineStatistics::ComputeShaderInvocations ], stat_unit_name );

        ImGui::Text( "Invocations divided by full screen pixels:" );
        ImGui::Text( "VS %0.2f, FS %0.2f, CS %0.2f",
                     s_framebuffer_pixel_count > 0.0f ? ( pipeline_statistics->statistics[ GpuPipelineStatistics::VertexShaderInvocations ] / s_framebuffer_pixel_count ) : 0.0f,
                     s_framebuffer_pixel_count > 0.0f ? ( pipeline_statistics->statistics[ GpuPipelineStatistics::FragmentShaderInvocations ] / s_framebuffer_pixel_count ) : 0.0f,
                     s_framebuffer_pixel_count > 0.0f ? ( pipeline_statistics->statistics[ GpuPipelineStatistics::ComputeShaderInvocations ] / s_framebuffer_pixel_count ) : 0.0f );
    }

    ImGui::Combo( "Stat Units", &stat_unit_index, stat_unit_names, IM_ARRAYSIZE( stat_unit_names ) );
}


// GpuProfiler //////////////////////////////////////////////////
void GpuProfiler::init( GpuDevice* gpu_device_, Allocator* allocator_,
                        u16 queries_per_thread_, u16 num_threads_, u16 max_frames ) {

    gpu_device = gpu_device_;
    allocator = allocator_;
    num_threads = num_threads_;
    // Queries per thread per cb
    queries_per_thread = queries_per_thread_;
    queries_per_frame = queries_per_thread_ * num_threads_;

    const u32 num_pools = num_threads * max_frames * k_command_queue_count;
    const u32 total_time_queries = queries_per_thread * num_pools * CommandBufferManager::k_num_cbs_per_thread;
    //const sizet allocated_size = sizeof( GPUTimeQuery ) * total_time_queries;
    //u8* memory = rallocam( allocated_size, allocator );

    timestamps.init( allocator, total_time_queries, total_time_queries );
   // memset( timestamps.data, 0, sizeof( GPUTimeQuery ) * total_time_queries );

    const u32 num_trees = num_pools * CommandBufferManager::k_num_cbs_per_thread;
    query_trees.init( allocator, num_trees, num_trees );

    thread_frame_query_pools.init( allocator, num_pools, num_pools );

    for ( u32 i = 0; i < num_pools; ++i ) {

        GpuThreadFrameQueryPools& pool = thread_frame_query_pools[ i ];
        pool.time_queries.size = CommandBufferManager::k_num_cbs_per_thread;

        for ( u32 cb_i = 0; cb_i < CommandBufferManager::k_num_cbs_per_thread; ++cb_i ) {

            const u32 tree_index = i * CommandBufferManager::k_num_cbs_per_thread + cb_i;
            GpuTimeQueryTree* query_tree = &query_trees[ tree_index ];

            // Each CB gets its own CPU-side query array slice.
            query_tree->set_queries( &timestamps[ tree_index * queries_per_thread ], queries_per_thread );
            pool.time_queries[ cb_i ] = query_tree;
        }

        // Create timestamp query pool used for GPU timings.
        VkQueryPoolCreateInfo timestamp_pool_info{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0, VK_QUERY_TYPE_TIMESTAMP,
                                                   queries_per_thread * 2u * CommandBufferManager::k_num_cbs_per_thread, 0 };
        vkCreateQueryPool( gpu_device->vulkan_device, &timestamp_pool_info, gpu_device->vulkan_allocation_callbacks, &pool.vulkan_timestamp_query_pool );

        const CommandQueueType queue_type = (CommandQueueType)( i % k_command_queue_count );
        if ( queue_type == CommandQueueType::Graphics ) {
            // Create pipeline statistics query pool
            VkQueryPoolCreateInfo statistics_pool_info{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0, VK_QUERY_TYPE_PIPELINE_STATISTICS, 7, 0 };
            statistics_pool_info.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT |
                VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
                VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
                VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT |
                VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
                VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT |
                VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT;
            vkCreateQueryPool( gpu_device->vulkan_device, &statistics_pool_info, gpu_device->vulkan_allocation_callbacks, &pool.vulkan_pipeline_stats_query_pool );
        }
        else {
            pool.vulkan_pipeline_stats_query_pool = VK_NULL_HANDLE;
        }
    }

    reset();
}

void GpuProfiler::shutdown() {
    query_trees.shutdown();

    for ( u32 i = 0; i < thread_frame_query_pools.size; ++i ) {
        GpuThreadFrameQueryPools& pool = thread_frame_query_pools[ i ];
        vkDestroyQueryPool( gpu_device->vulkan_device, pool.vulkan_timestamp_query_pool, gpu_device->vulkan_allocation_callbacks );
        vkDestroyQueryPool( gpu_device->vulkan_device, pool.vulkan_pipeline_stats_query_pool, gpu_device->vulkan_allocation_callbacks );
    }

    thread_frame_query_pools.shutdown();
    timestamps.shutdown();
}

u32 timestamp_base_offset( const CommandBuffer* cb, u32 queries_per_thread ) {
    return (u32)cb->cb_used_index * ( queries_per_thread * 2u );
}

void GpuProfiler::begin_command_buffer( CommandBuffer* cb ) {

    // Timestamp queries
    GpuThreadFrameQueryPools* thread_frame_pool = get_thread_frame_pools( cb->frame_index, cb->thread_index, cb->queue_type );

    // Reset query tree
    GpuTimeQueryTree* tree = thread_frame_pool->time_queries[ cb->cb_used_index ];
    tree->reset();

    const u32 base_query_index = timestamp_base_offset( cb, queries_per_thread );

    // Reset timestamp queries range
    vkCmdResetQueryPool( cb->vk_command_buffer, thread_frame_pool->vulkan_timestamp_query_pool,
                         base_query_index, queries_per_thread * 2 );

    // Pipeline statistics only if graphics queue (only on CB 0 to simplify)
    if ( thread_frame_pool->vulkan_pipeline_stats_query_pool != VK_NULL_HANDLE && cb->cb_used_index == 0 ) {
        vkCmdResetQueryPool( cb->vk_command_buffer, thread_frame_pool->vulkan_pipeline_stats_query_pool, 0, GpuPipelineStatistics::Count );
        vkCmdBeginQuery( cb->vk_command_buffer, thread_frame_pool->vulkan_pipeline_stats_query_pool, 0, 0 );
    }
}

void GpuProfiler::end_command_buffer( CommandBuffer* cb ) {

    GpuThreadFrameQueryPools* thread_frame_pool = get_thread_frame_pools( cb->frame_index, cb->thread_index, cb->queue_type );

    // Pipeline statistics only if graphics queue and cb0
    if ( thread_frame_pool->vulkan_pipeline_stats_query_pool != VK_NULL_HANDLE && cb->cb_used_index == 0 ) {
        vkCmdEndQuery( cb->vk_command_buffer, thread_frame_pool->vulkan_pipeline_stats_query_pool, 0 );
    }
}

void GpuProfiler::push_timestamp( CommandBuffer* cb, cstring name ) {
    GpuThreadFrameQueryPools* thread_frame_pool = get_thread_frame_pools( cb->frame_index, cb->thread_index, cb->queue_type );
    GpuTimeQueryTree* tree = thread_frame_pool->time_queries[ cb->cb_used_index ];
    GPUTimeQuery* time_query = tree->push( name );

    const u32 base_query_index = timestamp_base_offset( cb, queries_per_thread );

    vkCmdWriteTimestamp( cb->vk_command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, thread_frame_pool->vulkan_timestamp_query_pool,
                         base_query_index + time_query->start_query_index );
}

void GpuProfiler::pop_timestamp( CommandBuffer* cb ) {
    GpuThreadFrameQueryPools* thread_frame_pool = get_thread_frame_pools( cb->frame_index, cb->thread_index, cb->queue_type );
    GpuTimeQueryTree* tree = thread_frame_pool->time_queries[ cb->cb_used_index ];
    GPUTimeQuery* time_query = tree->pop();

    const u32 base_query_index = timestamp_base_offset( cb, queries_per_thread );

    vkCmdWriteTimestamp( cb->vk_command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, thread_frame_pool->vulkan_timestamp_query_pool,
                         base_query_index + time_query->end_query_index );
}

GpuThreadFrameQueryPools* GpuProfiler::get_thread_frame_pools( u32 frame_index, u32 thread_index, CommandQueueType queue_type ) {
    const u32 pool_index = pool_from_indices( frame_index, thread_index, queue_type );
    return &thread_frame_query_pools[ pool_index ];
}

u32 GpuProfiler::pool_from_indices( u32 frame_index, u32 thread_index, CommandQueueType queue_type ) {
    return ( ( frame_index * num_threads ) + thread_index ) * k_command_queue_count + (u32)queue_type;
}

void GpuProfiler::reset_pools( u32 frame_index ) {
    for ( u32 t = 0; t < num_threads; ++t ) {
        for ( u32 q = 0; q < k_command_queue_count; ++q ) {
            u32 pool_index = pool_from_indices( frame_index, t, (CommandQueueType)q );
            for ( u32 cb_i = 0; cb_i < CommandBufferManager::k_num_cbs_per_thread; ++cb_i ) {
                thread_frame_query_pools[ pool_index ].time_queries[ cb_i ]->reset();
            }
        }
    }
}

void GpuProfiler::reset() {
    current_frame_resolved = false;
}

void GpuProfiler::get_query_pool_results( ArenaAllocator* temp_allocator ) {

    frame_pipeline_statistics.reset();
    temp_allocator->clear();

    const u32 frame_to_query = gpu_device->previous_frame;

    resolved_frame_index = frame_to_query;
    resolved_frame_valid = true;

    for ( u32 t = 0; t < num_threads; ++t ) {
        for ( u32 q = 0; q < k_command_queue_count; ++q ) {

            const CommandQueueType queue_type = (CommandQueueType)q;
            const u32 pool_index = pool_from_indices( frame_to_query, t, queue_type );

            GpuThreadFrameQueryPools& pool = thread_frame_query_pools[ pool_index ];

            for ( u32 cb_i = 0; cb_i < CommandBufferManager::k_num_cbs_per_thread; cb_i++ ) {
                GpuTimeQueryTree* time_query = pool.time_queries[ cb_i ];
                if ( !time_query || time_query->allocated_time_query == 0 ) {
                    continue;
                }

                const u32 query_count = time_query->allocated_time_query;
                // GPU read starts at this CB slice base.
                const u32 query_base_index = cb_i * ( queries_per_thread * 2u );

                u64* timestamps_data = (u64*)ralloca( query_count * 2 * sizeof( u64 ), temp_allocator );

                vkGetQueryPoolResults( gpu_device->vulkan_device,
                                       pool.vulkan_timestamp_query_pool,
                                       query_base_index,
                                       query_count * 2,
                                       sizeof( u64 ) * query_count * 2,
                                       timestamps_data,
                                       sizeof( u64 ),
                                       VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT );


                // Base offset into the CPU-side timestamps array for this (frame,thread,queue) pool.
                //const u32 query_offset = pool_index * queries_per_thread;
                const u32 tree_index = pool_index * CommandBufferManager::k_num_cbs_per_thread + cb_i;
                const u32 query_offset = tree_index * queries_per_thread;

                for ( u32 i = 0; i < query_count; ++i ) {
                    const u32 index = query_offset + i;
                    GPUTimeQuery& ts = timestamps[ index ];

                    const u64 start = timestamps_data[ i * 2 + 0 ];
                    const u64 end = timestamps_data[ i * 2 + 1 ];

                    // Guard against wrong values if something went wrong.
                    const u64 range = ( end >= start ) ? ( end - start ) : 0;

                    const double elapsed_time = range * gpu_device->gpu_timestamp_frequency;

                    ts.begin = start;
                    ts.end = end;
                    ts.elapsed_ms = elapsed_time;
                    ts.frame_index = gpu_device->absolute_frame - 1;
                    ts.queue_type = queue_type;
                }

                // Pipeline statistics only if this pool has them (graphics only).
                if ( pool.vulkan_pipeline_stats_query_pool != VK_NULL_HANDLE ) {

                    u64* pipeline_statistics_data =
                        (u64*)ralloca( GpuPipelineStatistics::Count * sizeof( u64 ), temp_allocator );

                    vkGetQueryPoolResults( gpu_device->vulkan_device,
                                           pool.vulkan_pipeline_stats_query_pool,
                                           0,
                                           1,
                                           GpuPipelineStatistics::Count * sizeof( u64 ),
                                           pipeline_statistics_data,
                                           sizeof( u64 ),
                                           VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT );

                    for ( u32 i = 0; i < GpuPipelineStatistics::Count; ++i ) {
                        frame_pipeline_statistics.statistics[ i ] += pipeline_statistics_data[ i ];
                    }
                }
            }

            temp_allocator->clear();
        }
    }
}


u32 GpuProfiler::resolve( u32 current_frame, GPUTimeQuery* timestamps_to_fill ) {
    u32 copied_timestamps = 0;

    for ( u32 t = 0; t < num_threads; ++t ) {
        for ( u32 q = 0; q < k_command_queue_count; ++q ) {

            const u32 pool_index = pool_from_indices( current_frame, t, (CommandQueueType)q );
            GpuThreadFrameQueryPools& thread_pools = thread_frame_query_pools[ pool_index ];

            for ( u32 cb_i = 0; cb_i < CommandBufferManager::k_num_cbs_per_thread; ++cb_i ) {

                GpuTimeQueryTree* time_query = thread_pools.time_queries[ cb_i ];
                if ( !time_query || time_query->allocated_time_query == 0 ) {
                    continue;
                }

                const u32 tree_index = pool_index * CommandBufferManager::k_num_cbs_per_thread + cb_i;

                raptor::memory_copy(
                    timestamps_to_fill + copied_timestamps,
                    &timestamps[ tree_index * queries_per_thread ],
                    sizeof( GPUTimeQuery ) * time_query->allocated_time_query );

                copied_timestamps += time_query->allocated_time_query;
            }
        }
    }

    return copied_timestamps;
}

// GpuTimeQueryTree ///////////////////////////////////////////////////////
void GpuTimeQueryTree::reset() {
    current_time_query = 0;
    allocated_time_query = 0;
    depth = 0;
}

void GpuTimeQueryTree::set_queries( GPUTimeQuery* time_queries_, u32 count ) {
    time_queries = { time_queries_, count };

    reset();
}

GPUTimeQuery* GpuTimeQueryTree::push( cstring name ) {

    GPUTimeQuery& time_query = time_queries[ allocated_time_query ];
    time_query.start_query_index = allocated_time_query * 2;
    time_query.end_query_index = time_query.start_query_index + 1;
    time_query.depth = depth++;
    time_query.name = name;
    time_query.parent_index = current_time_query;

    current_time_query = allocated_time_query;
    ++allocated_time_query;

    return &time_query;
}

GPUTimeQuery* GpuTimeQueryTree::pop() {
    GPUTimeQuery& time_query = time_queries[ current_time_query ];
    current_time_query = time_query.parent_index;

    depth--;

    return &time_query;
}

// GpuPipelineStatistics //////////////////////////////////////////////////
void GpuPipelineStatistics::reset() {
    for ( u32 i = 0; i < Count; ++i ) {
        statistics[ i ] = 0;
    }
}

} // namespace raptor
