
#pragma once

#include "foundation/assert.hpp"
#include "foundation/span.hpp"
#include <initializer_list>

namespace raptor {

// StaticArray ////////////////////////////////////////////////////////////
template <typename T, u32 capacity_>
struct StaticArray {

    StaticArray() = default;
    StaticArray( std::initializer_list<T> list );

    void operator=( const Span<const T>& other ) {
        RASSERT( other.size <= capacity_ );
        size = ( u32 )other.size;
        memcpy( data, other.data, sizeof( T ) * size );
    }

    void                        push( const T& element );
    T&                          push_use();                 // Grow the size and return T to be filled.

    void                        pop();
    void                        delete_swap( u32 index );

    T&                          operator[]( u32 index );
    const T&                    operator[]( u32 index ) const;

    void                        clear();

    T&                          back();
    const T&                    back() const;

    T&                          front();
    const T&                    front() const;

    u32                         size_in_bytes() const;
    u32                         capacity_in_bytes() const;
    u32                         capacity() const { return capacity_; }
    Span<T>                     as_span() { return Span<T>( data, size ); }

    T                           data[ capacity_ ];
    u32                         size = 0;       // Occupied size

}; // struct StaticArray


// Implementation /////////////////////////////////////////////////////

// StaticArray ////////////////////////////////////////////////////////////

template<typename T, u32 capacity_>
inline StaticArray<T, capacity_>::StaticArray( std::initializer_list<T> list ) {
    RASSERT( list.size() <= capacity_ );
    size = u32( list.size() );
    u32 i = 0;
    for ( const T& element : list ) {
        data[ i++ ] = element;
    }
}

template<typename T, u32 capacity_>
inline void StaticArray<T, capacity_>::push( const T& element ) {
    RASSERT( size < capacity_ );
    data[ size++ ] = element;
}

template<typename T, u32 capacity_>
inline T& StaticArray<T, capacity_>::push_use() {
    RASSERT( size < capacity_ );

    ++size;

    return back();
}

template<typename T, u32 capacity_>
inline void StaticArray<T, capacity_>::pop() {
    RASSERT( size > 0 );
    --size;
}

template<typename T, u32 capacity_>
inline void StaticArray<T, capacity_>::delete_swap( u32 index ) {
    RASSERT( size > 0 && index < size );
    data[ index ] = data[ --size ];
}

template<typename T, u32 capacity_>
inline T& StaticArray<T, capacity_>::operator[]( u32 index ) {
    RASSERT( index < size );
    return data[ index ];
}

template<typename T, u32 capacity_>
inline const T& StaticArray<T, capacity_>::operator[]( u32 index ) const {
    RASSERT( index < size );
    return data[ index ];
}

template<typename T, u32 capacity_>
inline void StaticArray<T, capacity_>::clear() {
    size = 0;
}

template<typename T, u32 capacity_>
inline T& StaticArray<T, capacity_>::back() {
    RASSERT( size );
    return data[ size - 1 ];
}

template<typename T, u32 capacity_>
inline const T& StaticArray<T, capacity_>::back() const {
    RASSERT( size );
    return data[ size - 1 ];
}

template<typename T, u32 capacity_>
inline T& StaticArray<T, capacity_>::front() {
    RASSERT( size );
    return data[ 0 ];
}

template<typename T, u32 capacity_>
inline const T& StaticArray<T, capacity_>::front() const {
    RASSERT( size );
    return data[ 0 ];
}

template<typename T, u32 capacity_>
inline u32 StaticArray<T, capacity_>::size_in_bytes() const {
    return size * sizeof( T );
}

template<typename T, u32 capacity_>
inline u32 StaticArray<T, capacity_>::capacity_in_bytes() const {
    return capacity_ * sizeof( T );
}

} // namespace raptor
