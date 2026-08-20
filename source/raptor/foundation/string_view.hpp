
#pragma once

#include "span.hpp"

#include <string.h>

namespace raptor {

struct StringView : public Span<const char> {

    constexpr                   StringView() : Span<const char>() {}
                                StringView( cstring c_string ) : Span<const char>( c_string, strlen( c_string ) ) {}
    constexpr                   StringView( cstring data, size_t size ) : Span<const char>( data, size ) {}

    static bool                 equals( const StringView& a, const StringView& b );
    static void                 copy_to( const StringView& a, char* buffer, sizet buffer_size );

}; // struct StringView

} // namespace raptor
