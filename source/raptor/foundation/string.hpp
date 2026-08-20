#pragma once

#include "foundation/platform.hpp"
#include "foundation/string_view.hpp"

namespace raptor {

    // Forward declarations ///////////////////////////////////////////////
    struct Allocator;

    template <typename K, typename V>
    struct FlatHashMap;

    struct FlatHashMapIterator;

    //
    // Class that preallocates a buffer and appends strings to it. Reserve an additional byte for the null termination when needed.
    struct StringBuffer {

        void                        init( sizet size, Allocator* allocator );
        void                        shutdown();

        void                        append( const char* string );
        void                        append( const StringView& text );
        void                        append_m( void* memory, sizet size );       // Memory version of append.
        void                        append( const StringBuffer& other_buffer );
        void                        append_f( const char* format, ... );        // Formatted version of append.

        char*                       append_use( const char* string );
        char*                       append_use_f( const char* format, ... );
        char*                       append_use( const StringView& text );       // Append and returns a pointer to the start. Used for strings mostly.
        char*                       append_use_substring( const char* string, u32 start_index, u32 end_index ); // Append a substring of the passed string.

        void                        close_current_string();

        // Index interface
        u32                         get_index( cstring text ) const;
        cstring                     get_text( u32 index ) const;

        char*                       reserve( sizet size );

        char*                       current()       { return data + current_size; }

        void                        clear();

        char*                       data            = nullptr;
        u32                         buffer_size     = 1024;
        u32                         current_size    = 0;
        Allocator*                  allocator       = nullptr;

    }; // struct StringBuffer

    //
    //
    struct StringArray {

        void                        init( u32 buffer_size, u32 initial_string_count, Allocator* allocator );
        void                        shutdown();
        void                        clear();

        FlatHashMapIterator*        begin_string_iteration();
        sizet                       get_string_count() const;
        cstring                     get_string( u32 index ) const;
        cstring                     get_next_string( FlatHashMapIterator* it ) const;
        bool                        has_next_string( FlatHashMapIterator* it ) const;

        cstring                     intern( cstring string );

        FlatHashMap<u64, u32>*      string_to_index;    // Note: trying to avoid bringing the hash map header.
        FlatHashMapIterator*        strings_iterator;

        char*                       data                    = nullptr;
        u32                         buffer_size             = 1024;
        u32                         current_size            = 0;
        
        Allocator*                  allocator               = nullptr;

    }; // struct StringArray


} // namespace raptor
