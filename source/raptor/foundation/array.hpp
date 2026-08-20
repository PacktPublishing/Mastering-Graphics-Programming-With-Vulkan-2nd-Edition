#pragma once

#include "foundation/memory.hpp"
#include "foundation/assert.hpp"
#include "foundation/span.hpp"

#include <cstddef>
#include <iterator>

namespace raptor {

    // Data structures ////////////////////////////////////////////////////

    // ArrayAligned ///////////////////////////////////////////////////////
    template <typename T>
    struct Array {

        Array();
        Array( Allocator* allocator, std::initializer_list<T> list );
        ~Array();

        void                        init( Allocator* allocator, u32 initial_capacity, u32 initial_size = 0 );
        void                        shutdown();

        void                        push( const T& element );
        T&                          push_use();                 // Grow the size and return T to be filled.

        void                        pop();
        void                        delete_swap( u32 index );

        T&                          operator[]( u32 index );
        const T&                    operator[]( u32 index ) const;

        void                        clear();
        void                        set_size( u32 new_size );
        void                        set_capacity( u32 new_capacity );
        void                        grow( u32 new_capacity );

        T&                          back();
        const T&                    back() const;

        T&                          front();
        const T&                    front() const;

        u32                         size_in_bytes() const;
        u32                         capacity_in_bytes() const;
        Span<T>                     as_span() { return Span<T>( data, size ); }
        Span<const T>               as_cspan() { return Span<const T>( data, size ); }

        T*                          data = nullptr;
        u32                         size = 0;       // Occupied size
        u32                         capacity = 0;   // Allocated capacity
        Allocator*                  allocator = nullptr;

        struct iterator {
            using difference_type   = std::ptrdiff_t;
            using value_type        = T;
            using pointer           = T*;
            using reference         = T&;
            using iterator_category = std::random_access_iterator_tag;

            T* ptr;

            explicit iterator( T* ptr_ ) : ptr( ptr_ ) { }

            iterator& operator++() { ptr++; return *this; }
            iterator  operator++(int) { iterator retval = *this; ++(*this); return retval; }
            iterator& operator--() { ptr--; return *this; }
            iterator  operator--(int) { iterator retval = *this; --(*this); return retval; }
            iterator operator+(difference_type n) const { return iterator(ptr + n); }
            iterator operator-(difference_type n) const { return iterator(ptr - n); }
            difference_type operator-(const iterator& other) const { return ptr - other.ptr; }
            iterator& operator+=(difference_type n) { ptr += n; return *this; }
            iterator& operator-=(difference_type n) { ptr -= n; return *this; }
            reference operator[](difference_type n) const { return *(ptr + n); }
            bool      operator<(iterator other) const { return ptr < other.ptr; }
            bool      operator>(iterator other) const { return ptr > other.ptr; }
            bool      operator<=(iterator other) const { return ptr <= other.ptr; }
            bool      operator>=(iterator other) const { return ptr >= other.ptr; }
            bool      operator==(iterator other) const { return ptr == other.ptr; }
            bool      operator!=(iterator other) const { return !(*this == other); }
            reference operator*() const { return *ptr; }
        };

        iterator begin()        { return iterator( data ); }
        iterator end()          { return iterator( data + size ); }
        iterator cbegin() const { return iterator( data ); }
        iterator cend() const   { return iterator( data + size ); }
    }; // struct Array

    // Implementation /////////////////////////////////////////////////////

    // ArrayAligned ///////////////////////////////////////////////////////
    template<typename T>
    inline Array<T>::Array() {
        //RASSERT( true );
    }

    template<typename T>
    inline Array<T>::Array( Allocator* allocator, std::initializer_list<T> list ) {
        init( allocator, ( u32 )list.size(), ( u32 )list.size() );

        memcpy( data, list.begin(), list.size() * sizeof( T ) );
    }

    template<typename T>
    inline Array<T>::~Array() {
        //RASSERT( data == nullptr );
    }

    template<typename T>
    inline void Array<T>::init( Allocator* allocator_, u32 initial_capacity, u32 initial_size ) {
        data = nullptr;
        size = initial_size;
        capacity = 0;
        allocator = allocator_;

        if ( initial_capacity > 0 ) {
            grow( initial_capacity );
        }
    }

    template<typename T>
    inline void Array<T>::shutdown() {
        if ( capacity > 0 ) {
            allocator->deallocate( data );
        }
        data = nullptr;
        size = capacity = 0;
    }

    template<typename T>
    inline void Array<T>::push( const T& element ) {
        if ( size >= capacity ) {
            grow( capacity + 1 );
        }

        data[ size++ ] = element;
    }

    template<typename T>
    inline T& Array<T>::push_use() {
        if ( size >= capacity ) {
            grow( capacity + 1 );
        }
        ++size;

        return back();
    }

    template<typename T>
    inline void Array<T>::pop() {
        RASSERT( size > 0 );
        --size;
    }

    template<typename T>
    inline void Array<T>::delete_swap( u32 index ) {
        RASSERT( size > 0 && index < size );
        data[ index ] = data[ --size ];
    }

    template<typename T>
    inline T& Array<T>::operator []( u32 index ) {
        RASSERT( index < size );
        return data[ index ];
    }

    template<typename T>
    inline const T& Array<T>::operator []( u32 index ) const {
        RASSERT( index < size );
        return data[ index ];
    }

    template<typename T>
    inline void Array<T>::clear() {
        size = 0;
    }

    template<typename T>
    inline void Array<T>::set_size( u32 new_size ) {
        if ( new_size > capacity ) {
            grow( new_size );
        }
        size = new_size;
    }

    template<typename T>
    inline void Array<T>::set_capacity( u32 new_capacity ) {
        if ( new_capacity > capacity ) {
            grow( new_capacity );
        }
    }

    template<typename T>
    inline void Array<T>::grow( u32 new_capacity ) {
        if ( new_capacity < capacity * 2 ) {
            new_capacity = capacity * 2;
        } else if ( new_capacity < 4 ) {
            new_capacity = 4;
        }

        T* new_data = ( T* )allocator->allocate( new_capacity * sizeof( T ), alignof( T ) );
        if ( capacity ) {
            memory_copy( new_data, data, capacity * sizeof( T ) );

            allocator->deallocate( data );
        }

        data = new_data;
        capacity = new_capacity;
    }

    template<typename T>
    inline T& Array<T>::back() {
        RASSERT( size );
        return data[ size - 1 ];
    }

    template<typename T>
    inline const T& Array<T>::back() const {
        RASSERT( size );
        return data[ size - 1 ];
    }

    template<typename T>
    inline T& Array<T>::front() {
        RASSERT( size );
        return data[ 0 ];
    }

    template<typename T>
    inline const T& Array<T>::front() const {
        RASSERT( size );
        return data[ 0 ];
    }

    template<typename T>
    inline u32 Array<T>::size_in_bytes() const {
        return size * sizeof( T );
    }

    template<typename T>
    inline u32 Array<T>::capacity_in_bytes() const {
        return capacity * sizeof( T );
    }

} // namespace raptor
