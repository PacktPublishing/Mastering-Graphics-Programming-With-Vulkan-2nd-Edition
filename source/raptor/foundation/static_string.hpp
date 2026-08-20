#pragma once

#include "foundation/string_view.hpp"

namespace raptor {

//
//
struct StaticStringBase {

    void                format_impl( char* data, sizet capacity, cstring fmt, va_list args );

    void                append_impl( char* data, sizet capacity, cstring fmt, va_list args );

    void                set_impl( char* data, sizet capacity, cstring str );
    void                set_impl( char* data, sizet capacity, const StringView& str );

    void                append_raw_impl( char* data, sizet capacity, cstring str );
    void                append_raw_impl( char* data, sizet capacity, const StringView& str );

}; // struct StaticStringBase

//
//
struct StaticString64 : public StaticStringBase {

    StaticString64();
    StaticString64( cstring str );

    void                format( cstring fmt, ... );
    void                append( cstring fmt, ... );

    void                append_raw( cstring str );
    void                append_raw( const StringView& str );

    void                set( cstring str );
    void                set( const StringView& str );

    cstring             c_str() const { return data_; }
    size_t              size() const;
    constexpr size_t    capacity() const { return k_capacity; }

    void                clear() { data_[ 0 ] = '\0'; }

    char&               operator[]( size_t i ) { return data_[ i ]; }
    const char&         operator[]( size_t i ) const { return data_[ i ]; }

    StringView          string_view() const;

    static constexpr size_t k_capacity = 64;

    char                data_[ k_capacity ];
}; // struct StaticString64

//
//
struct StaticString256 : public StaticStringBase {

    StaticString256();
    StaticString256( cstring str );

    void                format( cstring fmt, ... );
    void                append( cstring fmt, ... );

    void                append_raw( cstring str );
    void                append_raw( const StringView& str );

    void                set( cstring str );
    void                set( const StringView& str );

    cstring             c_str() const { return data_; }
    StringView          string_view() const;

    size_t              size() const;
    constexpr size_t    capacity() const { return k_capacity; }

    void                clear() { data_[ 0 ] = '\0'; }

    char&               operator[]( size_t i ) { return data_[ i ]; }
    const char&         operator[]( size_t i ) const { return data_[ i ]; }

    static constexpr size_t k_capacity = 256;

    char                data_[ k_capacity ];
}; // struct StaticString256

//
//
struct StaticString512 : public StaticStringBase {

    StaticString512();
    StaticString512( cstring str );

    void                format( cstring fmt, ... );
    void                append( cstring fmt, ... );

    void                append_raw( cstring str );
    void                append_raw( const StringView& str );

    void                set( cstring str );
    void                set( const StringView& str );

    cstring             c_str() const { return data_; }
    StringView          string_view() const;

    size_t              size() const;
    constexpr size_t    capacity() const { return k_capacity; }

    void                clear() { data_[ 0 ] = '\0'; }

    char&               operator[]( size_t i ) { return data_[ i ]; }
    const char&         operator[]( size_t i ) const { return data_[ i ]; }

    static constexpr size_t k_capacity = 512;

    char                data_[ k_capacity ];
}; // struct StaticString512

//
//
struct StringPath {

    StringPath() = default;
    StringPath( cstring str );

    void                set( cstring str );
    void                set( const StringView& str );

    void                append( cstring str );
    void                append( const StringView& str );

    void                go_up();

    StringView          root() const; // TODO(marco): find better name
    StringView          filename() const;
    StringView          extension() const;

    cstring             c_str() const { return path_.c_str(); }
    StringView          string_view() const { return path_.string_view(); }
    size_t              size() const { return path_.size(); };

    void                clear() { path_.clear(); }

    void                normalize();

    StaticString512     path_;

}; // struct StringPath

} // namespace raptor