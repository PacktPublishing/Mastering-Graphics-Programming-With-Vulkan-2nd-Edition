#include "window.hpp"

#include "foundation/log.hpp"
#include "foundation/numerics.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "external/imgui/imgui.h"
#include "external/imgui/imgui_impl_sdl3.h"

static SDL_Window* window = nullptr;

namespace raptor {

static f32 sdl_get_monitor_refresh() {
    SDL_DisplayID display_id = SDL_GetPrimaryDisplay();
    const SDL_DisplayMode* current = SDL_GetCurrentDisplayMode( display_id );
    RASSERT( current != nullptr );
    return 1.0f / current->refresh_rate;
}

void Window::init( void* configuration_ ) {
    rprint( "WindowService init\n" );

    SDL_InitFlags init_flags = SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS;
    if ( SDL_Init( init_flags ) == false ) {
        rprint( "SDL Init error: %s\n", SDL_GetError() );
        return;
    }

    SDL_DisplayID display_id = SDL_GetPrimaryDisplay();
    const SDL_DisplayMode* current = SDL_GetCurrentDisplayMode( display_id );

    WindowConfiguration& configuration = *( WindowConfiguration* )configuration_;

    SDL_WindowFlags window_flags = ( SDL_WindowFlags )( SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY );

    float main_scale = SDL_GetDisplayContentScale(display_id);
    window = SDL_CreateWindow( configuration.name, (int)(configuration.width * main_scale), (int)(configuration.height * main_scale), window_flags );

    rprint( "Window created successfully\n" );

    int window_width, window_height;
    SDL_GetWindowSizeInPixels( window, &window_width, &window_height );

    width = ( u32 )window_width;
    height = ( u32 )window_height;

    // Assing this so it can be accessed from outside.
    platform_handle = window;

    // Callbacks
    os_messages_callbacks.init( configuration.allocator, 4 );
    os_messages_callbacks_data.init( configuration.allocator, 4 );

    display_refresh = sdl_get_monitor_refresh();
}

void Window::shutdown() {

    os_messages_callbacks_data.shutdown();
    os_messages_callbacks.shutdown();

    SDL_DestroyWindow( window );
    SDL_Quit();

    rprint( "WindowService shutdown\n" );
}

void Window::handle_os_messages() {

    SDL_Event event;
    while ( SDL_PollEvent( &event ) ) {

        ImGui_ImplSDL3_ProcessEvent( &event );

        switch ( event.type ) {
            case SDL_EVENT_QUIT:
            {
                requested_exit = true;
                goto propagate_event;
                break;
            }

            // Handle subevent
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            case SDL_EVENT_WINDOW_RESIZED:
            {
                // Resize only if even numbers are used?
                // NOTE: This goes in an infinite loop when maximising a window that has an odd width/height.
                /*if ( ( event.window.data1 % 2 == 1 ) || ( event.window.data2 % 2 == 1 ) ) {
                    u32 new_width = ( u32 )( event.window.data1 % 2 == 0 ? event.window.data1 : event.window.data1 - 1 );
                    u32 new_height = ( u32 )( event.window.data2 % 2 == 0 ? event.window.data2 : event.window.data2 - 1 );

                    if ( new_width != width || new_height != height ) {
                        SDL_SetWindowSize( window, new_width, new_height );

                        rprint( "Forcing resize to a multiple of 2, %ux%u from %ux%u\n", new_width, new_height, event.window.data1, event.window.data2 );
                    }
                }*/
                //else
                {
                    u32 new_width = ( u32 )( event.window.data1 );
                    u32 new_height = ( u32 )( event.window.data2 );

                    // Update only if needed.
                    if ( new_width != width || new_height != height ) {
                        resized = true;
                        width = new_width;
                        height = new_height;

                        rprint( "Resizing to %u, %u\n", width, height );
                    }
                }

                break;
            }

            case SDL_EVENT_WINDOW_FOCUS_GAINED:
            {
                rprint( "Focus Gained\n" );
                break;
            }
            case SDL_EVENT_WINDOW_FOCUS_LOST:
            {
                rprint( "Focus Lost\n" );
                break;
            }
            case SDL_EVENT_WINDOW_MAXIMIZED:
            {
                rprint( "Maximized\n" );
                minimized = false;
                break;
            }
            case SDL_EVENT_WINDOW_MINIMIZED:
            {
                rprint( "Minimized\n" );
                minimized = true;
                break;
            }
            case SDL_EVENT_WINDOW_RESTORED:
            {
                rprint( "Restored\n" );
                minimized = false;
                break;
            }
            case SDL_EVENT_WINDOW_EXPOSED:
            {
                rprint( "Exposed\n" );
                break;
            }

            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            {
                requested_exit = true;
                rprint( "Window close event received.\n" );
                break;
            }
            default:
            {
                display_refresh = sdl_get_monitor_refresh();
                break;
            }
        }
        goto propagate_event;
        break;

    // Maverick:
    propagate_event:
        // Callbacks
        for ( u32 i = 0; i < os_messages_callbacks.size; ++i ) {
            OsMessagesCallback callback = os_messages_callbacks[ i ];
            callback( &event, os_messages_callbacks_data[i] );
        }
    }
}

void Window::set_fullscreen( bool value ) {
    SDL_SetWindowFullscreen( window, value );
}

void Window::register_os_messages_callback( OsMessagesCallback callback, void* user_data ) {

    os_messages_callbacks.push( callback );
    os_messages_callbacks_data.push( user_data );
}

void Window::unregister_os_messages_callback( OsMessagesCallback callback ) {
    RASSERTM( os_messages_callbacks.size < 8, "This array is too big for a linear search. Consider using something different!" );

    for ( u32 i = 0; i < os_messages_callbacks.size; ++i ) {
        if ( os_messages_callbacks[ i ] == callback ) {
            os_messages_callbacks.delete_swap( i );
            os_messages_callbacks_data.delete_swap( i );
        }
    }
}

void Window::center_mouse( bool dragging ) {
    if ( dragging ) {
        SDL_WarpMouseInWindow( window, ( f32 )raptor::roundu32(width / 2.f), ( f32 )raptor::roundu32(height / 2.f) );
        SDL_SetWindowMouseGrab( window, true );
        SDL_SetWindowRelativeMouseMode( window, true );
    } else {
        SDL_SetWindowMouseGrab( window, false );
        SDL_SetWindowRelativeMouseMode( window, false );
    }
}

} // namespace raptor
