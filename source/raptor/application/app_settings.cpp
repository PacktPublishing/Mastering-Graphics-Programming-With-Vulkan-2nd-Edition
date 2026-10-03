#include "application/app_settings.hpp"

#include "foundation/log.hpp"
#include "foundation/static_string.hpp"

#include "external/json.hpp"

#include <stdio.h>
#include <fstream>

namespace raptor {

using json = nlohmann::json;

// One overload per supported type. False means wrong type or out of range: the value is not touched.
static bool read_value( const json& value, bool& out ) {
    if ( !value.is_boolean() ) {
        return false;
    }
    out = value.get<bool>();
    return true;
}

static bool read_value( const json& value, u32& out ) {
    // 1280 is unsigned, -1 and 1280.0 are not.
    if ( !value.is_number_unsigned() || value.get<u64>() > 0xFFFFFFFFull ) {
        return false;
    }
    out = ( u32 )value.get<u64>();
    return true;
}

static bool read_value( const json& value, f32& out ) {
    if ( !value.is_number() ) {
        return false;
    }
    out = value.get<f32>();
    return true;
}

static cstring type_name( bool )    { return "bool"; }
static cstring type_name( u32 )     { return "u32"; }
static cstring type_name( f32 )     { return "f32"; }

// A section flattened: { "window": { "width": 1280 } } becomes { "/window/width": 1280 }.
// read() removes what it reads, so the keys left at the end are the unknown ones.
// Empty objects and nulls become null: they mean "not set".
struct Section {

    json            flat;
    cstring         path;
    cstring         name;

    template< typename T >
    void read( cstring key, T& out ) {
        auto it = flat.find( key );
        if ( it == flat.end() ) {
            return;
        }
        if ( !it->is_null() && !read_value( *it, out ) ) {
            rprint( "settings: %s [%s] %s: expected %s, got %s, ignored\n", path, name, key, type_name( out ), it->dump().c_str() );
        }
        flat.erase( it );
    }

    // Arrays are flattened too: [ 1, 2, 3 ] becomes "/key/0", "/key/1", "/key/2".
    void read( cstring key, glm::vec3& out ) {
        char element_key[ 128 ];
        for ( u32 i = 0; i < 3; ++i ) {
            snprintf( element_key, sizeof( element_key ), "%s/%u", key, i );
            read( element_key, out[ i ] );
        }
    }

    // Enums are strings in the file: one of names[ 0 .. count - 1 ].
    template<typename Enum>
    void read_enum( cstring key, Enum& out, const char* const* names, u32 count ) {
        
        auto it = flat.find( key );
        if ( it == flat.end() ) {
            return;
        }

        if ( !it->is_null() ) {
            bool found = false;
            if ( it->is_string() ) {
                const std::string& value = it->get_ref<const std::string&>();
                for ( u32 i = 0; i < count && !found; ++i ) {
                    if ( value == names[ i ] ) {
                        out = ( Enum )i;
                        found = true;
                    }
                }
            }
            if ( !found ) {
                std::string valid;
                for ( u32 i = 0; i < count; ++i ) {
                    valid += i ? ", " : "";
                    valid += names[ i ];
                }
                rprint( "settings: %s [%s] %s: expected one of %s, got %s, ignored\n", path, name, key, valid.c_str(), it->dump().c_str() );
            }
        }

        flat.erase( it );
    }

    void report_unknown_keys() const {
        for ( const auto& item : flat.items() ) {
            if ( !item.value().is_null() ) {
                rprint( "settings: %s [%s] unknown key %s, ignored\n", path, name, item.key().c_str() );
            }
        }
    }

}; // struct Section

static void apply_section( AppSettings& settings, const json& section_json, cstring path, cstring name ) {

    if ( !section_json.is_object() ) {
        rprint( "settings: %s [%s] is not an object, ignored\n", path, name );
        return;
    }

    Section section{ section_json.flatten(), path, name };

    section.read( "/window/width", settings.window_width );
    section.read( "/window/height", settings.window_height );

    section.read( "/camera/near", settings.camera_near );
    section.read( "/camera/far", settings.camera_far );
    section.read( "/camera/fov_y", settings.camera_fov_y );
    section.read( "/camera/rotation_speed", settings.camera_rotation_speed );
    section.read( "/camera/movement_speed", settings.camera_movement_speed );
    section.read( "/camera/movement_delta", settings.camera_movement_delta );
    section.read( "/camera/position", settings.camera_position );
    section.read( "/camera/direction", settings.camera_direction );

    section.read( "/gpu/validation", settings.gpu_validation );
    section.read( "/gpu/buffer_pool", settings.gpu_buffer_pool );
    section.read( "/gpu/timestamps", settings.gpu_timestamps );
    section.read_enum( "/gpu/debug", settings.gpu_debug, VulkanDebugMode::s_value_names, VulkanDebugMode::Count );
    section.read_enum( "/gpu/present_mode", settings.gpu_present_mode, PresentMode::s_value_names, PresentMode::Count );
    section.read( "/gpu/buffer_pool", settings.gpu_buffer_pool );

    section.read( "/upscale/enabled", settings.upscale_enabled );
    section.read( "/upscale/render_scale", settings.upscale_render_scale );

    section.read( "/memory/max_dynamic_mb", settings.memory_max_dynamic_mb );

    section.report_unknown_keys();
}

static void load_file( AppSettings& settings, cstring path, cstring chapter, bool required ) {

    std::ifstream file( path );
    if ( !file ) {
        if ( required ) {
            rprint( "settings: cannot open %s, using built-in defaults\n", path );
        }
        return;
    }

    json root;
    try {
        root = json::parse( file );
    } catch ( const json::parse_error& e ) {
        // e.what() has line and column.
        rprint( "settings: %s: %s, file ignored\n", path, e.what() );
        return;
    }

    if ( !root.is_object() ) {
        rprint( "settings: %s is not an object, file ignored\n", path );
        return;
    }

    rprint( "settings: loaded %s\n", path );

    // Sections of other chapters are not an error: the file is shared.
    auto it = root.find( "default" );
    if ( it != root.end() ) {
        apply_section( settings, *it, path, "default" );
    }

    it = root.find( chapter );
    if ( it != root.end() ) {
        apply_section( settings, *it, path, chapter );
    }
}

void AppSettings::load( cstring data_folder, cstring chapter ) {

    StaticString512 path;
    path.append( "%s/settings_default.json", data_folder );
    load_file( *this, path.c_str(), chapter, true);

    path.clear();
    path.append( "%s/settings.json", data_folder );
    load_file( *this, path.c_str(), chapter, false );
}

} // namespace raptor
