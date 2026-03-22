#pragma once

#include <cstddef>
#include <string>

#include "status.h"
#include "slice.h"

namespace rocketdb {

class Slice;

class WriteBatch {
    public:
        class Handler {
            public:
                virtual ~Handler();
                virtual void Put(const Slice& key, const Slice& value) = 0;
                virtual void Delete(const Slice& key) = 0;
        };

        WriteBatch();

        WriteBatch(const WriteBatch&) = default;
        WriteBatch& operator=(const WriteBatch&) = default;

        ~WriteBatch();

        // Store the mapping key->value int the database
        void Put(const Slice& key, const Slice& value);

        // If the database contains a mapping for key, erase it, else do nothing
        void Delete(const Slice& key);

        // Clear all updates buffered in this batch
        void Clear();

        // The size of the database changes caused by this batch
        size_t ApproximateSize() const;

        // Copies the operations in source to this batch
        void Append(const WriteBatch& source);

        // Support for iterating over the contents of a batch
        Status Iterate(Handler* handler) const;

    private:
        friend class WriteBatchInternal;
        
        std::string rep_;
};  

}