#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "../../include/slice.h"
#include "../../include/status.h"
#include "../../include/table_build.h"

namespace rocketdb {

class Block;
class RandomAccessFile;
struct ReadOptions;

// BlockHandle is a pointer to the extend of a file that stores a data block or meta block
class BlockHandle {
    public:
        // Maximum encodeing length of a BlockHandle
        enum { kMaxEncodedLength = 10 + 10 };

        BlockHandle();

        uint64_t offset() const { return offset_; }
        void set_offset(uint64_t offset) { offset_ = offset; }

        uint64_t size() const { return size_; }
        void set_size(uint64_t size) { size_  = size; }

        void EncodeTo(std::string* dst) const;
        Status DecodeFrom(Slice* input);

    private:
        uint64_t offset_;
        uint64_t size_;
};

// Footer encapsulates the fixed information store at the tail end of every table file
class Footer {
    public:
        // 8 is the storage location of the magic number
        enum { kEncodedLength = 2 * BlockHandle::kMaxEncodedLength + 8 };  

        Footer() = default;

        // The block handle for the mateindex block of the table
        const BlockHandle& metaindex_handle() const { return metaindex_handle_; }
        void set_metaindex_handle(const BlockHandle& h) { metaindex_handle_ = h; }

        // The block handle for the mateindex block of the table
        const BlockHandle& index_handle() const { return index_handle_; }
        void set_index_handle(const BlockHandle& h) { index_handle_ = h; }

        void EncodeTo(std::string* dst) const;
        Status DecodeFrom(Slice* input);

    private:
        BlockHandle metaindex_handle_;  // MetaIndex Block
        BlockHandle index_handle_;  // Index Block
};

// Verify if is a valid SSTable file
static const uint64_t kTableMagicNumber = 0xdb4775248b08fb57ull;

// 1-byte type + 32-bit crc
static const size_t kBlockTrailerSize = 5;

struct BlockContents {
    Slice data;                 // Actual contents of data
    bool cachable;              // True iff data can be cached
    bool heap_allocated;        // True iff caller should delete[] data.data()
};

// Read the block identified by "handle" from file
// On success fill *result and return OK
// On failture return non-Ok
Status ReadBlock(RandomAccessFile* file, const ReadOptions& options, const BlockHandle& handle, BlockContents* result);

inline BlockHandle::BlockHandle() : offset_(static_cast<uint64_t>(0)), size_(static_cast<uint64_t>(0)) {}

}