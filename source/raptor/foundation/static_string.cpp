#include "foundation/static_string.hpp"
#include "foundation/assert.hpp"

#include <stdio.h>
#include <stdarg.h>
#include <memory.h>

namespace raptor {


// StaticStringBase ///////////////////////////////////////////////////////
void StaticStringBase::format_impl( char* data, sizet capacity, cstring fmt, va_list args ) {
    int written = vsnprintf( data, capacity, fmt, args );
    if ( written < 0 || static_cast<size_t>( written ) >= capacity ) {
        RASSERT( "StaticString format overflow" );
    }
}

void StaticStringBase::append_impl( char* data, sizet capacity, cstring fmt, va_list args ) {
    size_t len = strlen( data );
    if ( len >= capacity ) {
        RASSERT( "StaticString append overflow" );
    }
    int written = vsnprintf( data + len, capacity - len, fmt, args );
    if ( written < 0 || static_cast<size_t>( written ) >= capacity - len ) {
        RASSERT( "StaticString append overflow" );
    }
}

void StaticStringBase::set_impl( char* data, sizet capacity, cstring str ) {
    size_t len = strlen( str );
    if ( len >= capacity ) {
        RASSERT( "StaticString copy overflow" );
    }
    memcpy( data, str, len + 1 );  // include null terminator
}

void StaticStringBase::set_impl( char* data, sizet capacity, const StringView& str ) {
    size_t len = str.size;
    if ( len >= capacity ) {
        RASSERT( "StaticString copy overflow" );
    }
    memcpy( data, str.data, len + 1 );  // include null terminator
}

void StaticStringBase::append_raw_impl( char* data, sizet capacity, cstring str ) {
    size_t current_len = strlen( data );
    size_t append_len = strlen( str );
    if ( current_len + append_len >= capacity ) {
        RASSERT( "StaticString append_raw overflow" );
    }

    memcpy( data + current_len, str, append_len + 1 );  // include null terminator
}

void StaticStringBase::append_raw_impl( char* data, sizet capacity, const StringView& str ) {
    size_t current_len = strlen( data );
    size_t append_len = str.size;
    if ( current_len + append_len >= capacity ) {
        RASSERT( "StaticString append_raw overflow" );
    }

    memcpy( data + current_len, str.data, append_len + 1 );  // include null terminator
}

// StaticString64 /////////////////////////////////////////////////////////
StaticString64::StaticString64() {
    data_[ 0 ] = 0;
}

StaticString64::StaticString64( cstring str ) {
    set( str );
}

void StaticString64::format( cstring fmt, ... ) {
    va_list args;
    va_start( args, fmt );
    format_impl( data_, k_capacity, fmt, args );
    va_end( args );
}

void StaticString64::append( cstring fmt, ... ) {
    va_list args;
    va_start( args, fmt );
    append_impl( data_, k_capacity, fmt, args );
    va_end( args );
}

void StaticString64::append_raw( cstring str ) {
    append_raw_impl( data_, k_capacity, str );
}

void StaticString64::append_raw( const StringView& str ) {
    append_raw_impl( data_, k_capacity, str );
}

void StaticString64::set( cstring str ) {
    set_impl( data_, k_capacity, str );
}

void StaticString64::set( const StringView& str ) {
    set_impl( data_, k_capacity, str );
}

sizet StaticString64::size() const {
    return strlen( data_ );
}

StringView StaticString64::string_view() const {
    return StringView( data_, size() );
}

// StaticString256 /////////////////////////////////////////////////////////
StaticString256::StaticString256() {
    data_[ 0 ] = 0;
}

StaticString256::StaticString256( cstring str ) {
    set( str );
}

void StaticString256::format( cstring fmt, ... ) {
    va_list args;
    va_start( args, fmt );
    format_impl( data_, k_capacity, fmt, args );
    va_end( args );
}

void StaticString256::append( cstring fmt, ... ) {
    va_list args;
    va_start( args, fmt );
    append_impl( data_, k_capacity, fmt, args );
    va_end( args );
}

void StaticString256::append_raw( cstring str ) {
    append_raw_impl( data_, k_capacity, str );
}

void StaticString256::append_raw( const StringView& str ) {
    append_raw_impl( data_, k_capacity, str );
}

void StaticString256::set( cstring str ) {
    set_impl( data_, k_capacity, str );
}

void StaticString256::set( const StringView& str ) {
    set_impl( data_, k_capacity, str );
}

sizet StaticString256::size() const {
    return strlen( data_ );
}

StringView StaticString256::string_view() const {
    return StringView( data_, size() );
}

// StaticString512 /////////////////////////////////////////////////////////
StaticString512::StaticString512() {
    data_[ 0 ] = 0;
}

StaticString512::StaticString512( cstring str ) {
    set( str );
}

void StaticString512::format( cstring fmt, ... ) {
    va_list args;
    va_start( args, fmt );
    format_impl( data_, k_capacity, fmt, args );
    va_end( args );
}

void StaticString512::append( cstring fmt, ... ) {
    va_list args;
    va_start( args, fmt );
    append_impl( data_, k_capacity, fmt, args );
    va_end( args );
}

void StaticString512::append_raw( cstring str ) {
    append_raw_impl( data_, k_capacity, str );
}

void StaticString512::append_raw( const StringView& str ) {
    append_raw_impl( data_, k_capacity, str );
}

void StaticString512::set( cstring str ) {
    set_impl( data_, k_capacity, str );
}

void StaticString512::set( const StringView& str ) {
    set_impl( data_, k_capacity, str );
}

sizet StaticString512::size() const {
    return strlen( data_ );
}

StringView StaticString512::string_view() const {
    return StringView( data_, size() );
}

// StringPath /////////////////////////////////////////////////////////////
StringPath::StringPath( cstring str ) {
    path_.set( str );
}

void StringPath::set( cstring str ) {
    path_.set( str );

    //file_sanitize_path( path_.data_, path_.size() );
}

void StringPath::set( const StringView& str ) {
    path_.set( str );
}

void StringPath::append( cstring str ) {
    if ( path_.size() > 0 && path_.c_str()[ path_.size() - 1 ] != '/' )
        path_.append_raw( "/" );

    path_.append_raw( str );
}

void StringPath::append( const StringView& str ) {
    if ( path_.size() > 0 && path_.c_str()[ path_.size() - 1 ] != '/' )
        path_.append_raw( "/" );

    path_.append_raw( str );
}

void StringPath::go_up() {
    char* p = path_.data_;
    size_t len = path_.size();
    if ( len == 0 ) return;

    if ( p[ len - 1 ] == '/' )
        p[ --len ] = '\0';

    while ( len > 0 && p[ len - 1 ] != '/' )
        --len;

    if ( len > 0 ) len--; // remove the slash too
    p[ len ] = '\0';
}

StringView StringPath::root() const {
    const char* p = path_.c_str();
    size_t len = path_.size();
    for ( size_t i = len; i > 0; --i ) {
        if ( p[ i - 1 ] == '/' ) {
            return StringView( p, len - i );
        }
    }

    return StringView( p, len );
}

StringView StringPath::filename() const {
    const char* p = path_.c_str();
    size_t len = path_.size();
    for ( size_t i = len; i > 0; --i ) {
        if ( p[ i - 1 ] == '/' )
            return StringView( p + i, len - i );
    }
    return StringView( p, len );

}

StringView StringPath::extension() const {

    StringView name = filename();
    cstring dot = strrchr( name.data, '.' );
    if ( dot == nullptr )
        return {};
    //return name.substr( dot + 1 );
    return { dot + 1, name.size - ( name.data - dot ) };
}

void StringPath::normalize() {

    char* p = path_.data_;
    size_t len = path_.size();

    size_t write = 0;
    size_t i = 0;

    const size_t max_parts = 32;
    const char* parts[ max_parts ];
    size_t part_lengths[ max_parts ];
    size_t part_count = 0;

    while ( i < len ) {
        while ( i < len && p[ i ] == '/' ) ++i;
        if ( i >= len ) break;

        size_t start = i;
        while ( i < len && p[ i ] != '/' ) ++i;
        size_t part_len = i - start;

        const char* token = &p[ start ];

        if ( part_len == 1 && token[ 0 ] == '.' ) {
            continue;
        }
        else if ( part_len == 2 && token[ 0 ] == '.' && token[ 1 ] == '.' ) {
            if ( part_count > 0 ) {
                --part_count;
            }
        }
        else {
            if ( part_count < max_parts ) {
                parts[ part_count ] = token;
                part_lengths[ part_count ] = part_len;
                ++part_count;
            }
        }
    }

    write = 0;
    /*if ( is_absolute() ) {
        p[ write++ ] = '/';
    }*/

    for ( size_t j = 0; j < part_count; ++j ) {
        if ( write > 0 && p[ write - 1 ] != '/' ) {
            p[ write++ ] = '/';
        }

        memcpy( &p[ write ], parts[ j ], part_lengths[ j ] );
        write += part_lengths[ j ];
    }

    p[ write ] = '\0';
}

} // namespace raptor