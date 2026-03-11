#pragma once

#include "slice.h"
#include "status.h" 
#include <cassert>

namespace rocketdb {

class Iterator {
    public:
        Iterator();

        Iterator(const Iterator&) = delete;
        Iterator& operator=(const Iterator&) = delete;

        virtual ~Iterator();

        // Return true iff the iterator is valid
        virtual bool Valid() const = 0;

        virtual void SeekToFirst() = 0;

        virtual void SeekToLast() = 0;

        virtual void Seek(const Slice& target) = 0;

        // Move to the next entry in the source
        virtual void Next() = 0;

        // Move to the previous entry in the source
        virtual void Prev() = 0;

        // Return the key for the current entry
        virtual Slice key() const = 0;

        // Return the value for the current entry
        virtual Slice value() const = 0;

        // If an error has occurred, return it
        virtual Status status() const = 0;

        // Register a function when the iterator is destroyed
        using CleanupFunction = void (*)(void* arg1, void* arg2);
        void RegisterCleanup(CleanupFunction function, void* arg1, void* arg2);

    private:
        // Cleanup function are stored in a single-link list
        // The list head node is inlined in the iterator
        struct CleanupNode {
            bool IsEmpty() const { return function == nullptr; }

            void Run() {
                assert(function != nullptr);
                (*function)(arg1, arg2);
            }

            CleanupFunction function;
            void* arg1;
            void* arg2;
            CleanupNode* next;
        };
        CleanupNode cleanup_head_;

};

// Return an empty iterator
Iterator* NewEmptyIterator();

// Return an empty iterator with the specified status
Iterator* NewErrorIterator(const Status& status);

}