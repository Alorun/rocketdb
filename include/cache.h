#pragma once

#include <cstddef>
#include <cstdint>

#include "options.h"
#include "slice.h"

namespace rocketdb {

class Cache;

// Create a new cache with a fixed capacity
// The implementation of Cache use a least-recently-used eviction policy
Cache* NewLRUCache(size_t capaeity);

class Cache {
    public:
        Cache() = default;

        Cache(const Cache&) = delete;
        Cache& operator=(const Cache&) = delete;

        // Destroys entries by calling the deleter-function that was passed to the constructor
        virtual ~Cache();

        // Handle to an entry stored in the cache (reference counting mechanism)
        struct Handle {};

        // Insert a mapping from key->value into the cache and
        // assign it the specified charge against the total cache capacity
        virtual Handle* Insert(const Slice& key, void* value, size_t charge, 
                               void (*deleter)(const Slice& key, void* value)) = 0;

        // If the cache has no mapping for key, return nullptr
        // Else return ha handle that corresponds to the mapping
        virtual Handle* Lookup(const Slice& key) = 0;
        
        // Release a mapping returned by a previous Lookup()
        virtual void Release(Handle* handle) = 0;

        // Return the value encapsulated in a handle returned by a successful Lookup()
        virtual void* Value(Handle* handle) = 0;

        // If the cache contains entry for key, erase it 
        // the underlying entry will be kept around until all existing handles to it  have been released
        virtual void Erase(const Slice& key) = 0;

        // Return a new numeric id
        virtual uint64_t NewId() = 0;

        // Remove all cache entries that are not actively in use.
        virtual void Prune() {}

        // Return an estimate of the combined charges of all elements stored in the cache
        virtual size_t TotalCharge() const = 0;
};


}