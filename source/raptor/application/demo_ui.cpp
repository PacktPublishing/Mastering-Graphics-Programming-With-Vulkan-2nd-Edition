#include "application/demo_ui.hpp"

#include "graphics/gpu_profiler.hpp"
#include "graphics/renderer.hpp"
#include "graphics/frame_graph.hpp"
#include "external/imgui/imgui.h"

namespace raptor {

static constexpr const char* k_tab_names[] = {
    "Chapter", "Scene", "Renderer", "GPU Profiler",
    "Frame Graph", "Image Viewer", "Memory"
};

static_assert( sizeof( k_tab_names ) / sizeof( k_tab_names[ 0 ] ) ==
               static_cast< int >( DemoUiTab::Count ) );

[[maybe_unused]] static bool valid_tab( DemoUiTab tab ) {
    return static_cast< int >( tab ) >= 0 && tab < DemoUiTab::Count;
}

static bool tool_tab( DemoUiTab tab ) {
    return tab >= DemoUiTab::GpuProfiler;
}

bool DemoUi::begin( const char* chapter_title, const char* status_text ) {
    IM_ASSERT( active_window == Window::None );
    active_window = Window::Raptor;
    window_begun = true;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos( ImVec2( viewport->WorkPos.x + 16.f,
                                     viewport->WorkPos.y + 16.f ),
                             ImGuiCond_FirstUseEver );
    ImGui::SetNextWindowSize( ImVec2( 560.f, 680.f ), ImGuiCond_FirstUseEver );

    // Keep a stable window ID so position and size survive title changes.
    if ( !ImGui::Begin( "Raptor", nullptr, ImGuiWindowFlags_MenuBar ) ) {
        return false;
    }

    if ( ImGui::BeginMenuBar() ) {
        if ( ImGui::BeginMenu( "Tools" ) ) {
            // Reopen the whole window without changing its selected tool.
            if ( ImGui::MenuItem( "Outputs window", nullptr, &show_outputs ) ) {
                focus_outputs = show_outputs;
            }
            ImGui::Separator();

            for ( int i = static_cast< int >( DemoUiTab::GpuProfiler ); i < k_tab_count; ++i ) {
                if ( !tab_available[ i ] ) {
                    continue;
                }

                // The bool overload marks open tools without toggling them.
                // Clicking an existing tool selects it; its tab X closes it.
                if ( ImGui::MenuItem( k_tab_names[ i ], nullptr, tab_open[ i ] ) ) {
                    open_tab( static_cast< DemoUiTab >( i ) );
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    if ( chapter_title && chapter_title[ 0 ] ) {
        ImGui::PushTextWrapPos( 0.f );
        ImGui::TextUnformatted( chapter_title );
        ImGui::PopTextWrapPos();
    }
    if ( status_text && status_text[ 0 ] ) {
        ImGui::TextDisabled( "%s", status_text );
    }
    ImGui::Separator();

    const ImGuiTabBarFlags flags = ImGuiTabBarFlags_Reorderable |
        ImGuiTabBarFlags_TabListPopupButton |
        ImGuiTabBarFlags_FittingPolicyScroll;
    tab_bar_begun = ImGui::BeginTabBar( "RaptorTabs", flags );
    return tab_bar_begun;
}

void DemoUi::end() {
    IM_ASSERT( active_window == Window::Raptor );
    end_window();
}

bool DemoUi::begin_outputs() {
    IM_ASSERT( active_window == Window::None );
    active_window = Window::Outputs;

    // No ImGui window is submitted while hidden. end_outputs() still pairs
    // with this call and knows whether an ImGui::End() is needed.
    if ( !show_outputs ) {
        return false;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos( ImVec2( viewport->WorkPos.x + viewport->WorkSize.x - 16.f,
                                     viewport->WorkPos.y + 16.f ),
                             ImGuiCond_FirstUseEver, ImVec2( 1.f, 0.f ) );
    ImGui::SetNextWindowSize( ImVec2( 900.f, 680.f ), ImGuiCond_FirstUseEver );

    if ( focus_outputs ) {
        ImGui::SetNextWindowCollapsed( false, ImGuiCond_Always );
        ImGui::SetNextWindowFocus();
        focus_outputs = false;
    }

    window_begun = true;
    if ( !ImGui::Begin( "Outputs", &show_outputs ) ) {
        return false;
    }

    bool has_open_tools = false;
    for ( int i = static_cast< int >( DemoUiTab::GpuProfiler ); i < k_tab_count; ++i ) {
        has_open_tools |= tab_available[ i ] && tab_open[ i ];
    }
    if ( !has_open_tools ) {
        ImGui::TextWrapped( "Open a tool from Raptor > Tools." );
        return false;
    }

    const ImGuiTabBarFlags flags = ImGuiTabBarFlags_Reorderable |
        ImGuiTabBarFlags_TabListPopupButton |
        ImGuiTabBarFlags_FittingPolicyScroll;
    tab_bar_begun = ImGui::BeginTabBar( "OutputTabs", flags );
    return tab_bar_begun;
}

void DemoUi::end_outputs() {
    IM_ASSERT( active_window == Window::Outputs );
    end_window();
}

void DemoUi::common_outputs_ui() {

    if ( begin_tab( DemoUiTab::GpuProfiler ) ) {
        gpu_profiler->imgui_draw();
        end_tab();
    }

    if ( begin_tab( DemoUiTab::ImageViewer ) ) {
        image_viewer->debug_ui();
        end_tab();
    }

    if ( begin_tab( DemoUiTab::FrameGraph ) ) {
        frame_graph->add_ui();
        frame_graph->debug_ui();
        end_tab();
    }

    if ( begin_tab( DemoUiTab::Memory ) ) {
        MemoryService::instance()->system_allocator.debug_ui();
        end_tab();
    }
}

void DemoUi::end_window() {
    IM_ASSERT( active_window != Window::None && !tab_begun );

    if ( tab_bar_begun ) {
        ImGui::EndTabBar();
        tab_bar_begun = false;
    }

    if ( window_begun ) {
        ImGui::End();
        window_begun = false;
    }
    active_window = Window::None;
}

bool DemoUi::begin_tab( DemoUiTab tab ) {
    IM_ASSERT( window_begun && tab_bar_begun && !tab_begun );
    IM_ASSERT( valid_tab( tab ) );

    if ( tool_tab( tab ) != ( active_window == Window::Outputs ) ) {
        IM_ASSERT( false && "Submit controls in Raptor and tool tabs in Outputs." );
        return false;
    }

    const int index = static_cast< int >( tab );
    if ( !tab_available[ index ] || !tab_open[ index ] ) {
        return false;
    }

    ImGuiTabItemFlags flags = ImGuiTabItemFlags_None;
    if ( !tool_tab( tab ) ) {
        flags |= ImGuiTabItemFlags_Leading | ImGuiTabItemFlags_NoReorder;
    }

    DemoUiTab& requested_tab = tool_tab( tab ) ? requested_output_tab : requested_control_tab;
    const bool select_requested = requested_tab == tab;
    if ( select_requested ) {
        flags |= ImGuiTabItemFlags_SetSelected;
    }

    bool* open = tool_tab( tab ) ? &tab_open[ index ] : nullptr;
    const bool selected = ImGui::BeginTabItem( k_tab_names[ index ], open, flags );
    if ( select_requested ) {
        // Consume only when submitted, not when the window is collapsed.
        requested_tab = DemoUiTab::Count;
    }

    if ( !selected ) {
        return false;
    }

    // BeginTabItem pushes its ID: each child gets independent scroll state.
    // Keeping scrolling here leaves the menu, title and tab bar visible.
    if ( !ImGui::BeginChild( "Content", ImVec2( 0.f, 0.f ) ) ) {
        ImGui::EndChild();
        ImGui::EndTabItem();
        return false;
    }

    // Leave space for labels following standard ImGui input widgets.
    ImGui::PushItemWidth( ImGui::GetContentRegionAvail().x * 0.45f );
    tab_begun = true;
    return true;
}

void DemoUi::end_tab() {
    IM_ASSERT( tab_begun );
    ImGui::PopItemWidth();
    ImGui::EndChild();
    ImGui::EndTabItem();
    tab_begun = false;
}

void DemoUi::open_tab( DemoUiTab tab ) {
    IM_ASSERT( valid_tab( tab ) );
    const int index = static_cast< int >( tab );
    if ( tab_available[ index ] ) {
        tab_open[ index ] = true;
        if ( tool_tab( tab ) ) {
            requested_output_tab = tab;
            show_outputs = true;
            focus_outputs = true;
        } else {
            requested_control_tab = tab;
        }
    }
}

void DemoUi::set_tab_available( DemoUiTab tab, bool available ) {
    IM_ASSERT( active_window == Window::None && valid_tab( tab ) );
    if ( tab == DemoUiTab::Chapter ) {
        return;
    }

    const int index = static_cast< int >( tab );
    tab_available[ index ] = available;
    if ( !available ) {
        tab_open[ index ] = false;
        if ( requested_output_tab == tab ) {
            requested_output_tab = DemoUiTab::Count;
        }
        if ( requested_control_tab == tab ) {
            requested_control_tab = DemoUiTab::Chapter;
        }
    } else if ( !tool_tab( tab ) ) {
        tab_open[ index ] = true;
    }
}

void DemoUi::set_tools( GpuVisualProfiler* gpu_profiler, ImageViewDebugger* image_viewer,
                        FrameGraph* frame_graph, MemoryService* memory ) {
    if ( gpu_profiler ) {
        tab_available[ static_cast< int >( DemoUiTab::GpuProfiler ) ] = true;
        this->gpu_profiler = gpu_profiler;
    }
    if ( image_viewer ) {
        tab_available[ static_cast< int >( DemoUiTab::ImageViewer ) ] = true;
        this->image_viewer = image_viewer;
    }
    if ( frame_graph ) {
        tab_available[ static_cast< int >( DemoUiTab::FrameGraph ) ] = true;
        this->frame_graph = frame_graph;
    }
    if ( memory ) {
        tab_available[ static_cast< int >( DemoUiTab::Memory ) ] = true;
        this->memory = memory;
    }
}

} // namespace raptor
