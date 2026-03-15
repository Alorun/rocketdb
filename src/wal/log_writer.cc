#include "log_writer.h"

#include <cassert>
#include <cstddef>
#include <cstdint>

#include "../util/coding.h"
#include "../util/crc32c.h"
#include "log_format.h"

namespace rocketdb {
namespace log {

static void InitTypeCrc(uint32_t* type_cc) {
    for (int i = 0; i <= kMaxRecordType; i++) {
        char t = static_cast<char>(i);
        type_cc[i] = crc32c::Value(&t, 1);
    }
}

Writer::Writer(WritableFile* dest) : dest_(dest), block_offset_(0) {
    InitTypeCrc(type_crc_);
}

Writer::Writer(WritableFile* dest, uint64_t dest_length) : dest_(dest), block_offset_(dest_length % kBlockSize) {
    InitTypeCrc(type_crc_);
}

Writer::~Writer() = default;

Status Writer::AddRecord(const Slice& slice) {
    const char* ptr = slice.data();
    size_t left = slice.size();

    Status s;
    bool begin = true;
    do {
        const int leftover = kBlockSize - block_offset_;
        assert(leftover >= 0);
        // Switch to a new block
        if (leftover < kHeaderSize) {
            if (leftover > 0) {
                // Fill th remaining part of the block
                static_assert(kHeaderSize == 7, "");
                dest_->Append(Slice("\x00\x00\x00\x00\x00\x00", leftover));
            }
            block_offset_ = 0;
        }

        assert(kBlockSize - block_offset_ - kHeaderSize >= 0);

        const size_t avail = kBlockSize - block_offset_ - kHeaderSize;
        const size_t fragment_length = (left < avail) ? left : avail;

        RecordType type;
        const bool end = (left == fragment_length);
        if (begin && end) {
            type = kFullType;
        } else if (begin) {
            type = kFirstType;
        } else if (end) {
            type = kLastType;
        } else {
            type = kMiddleType;
        }

        s = EmitPhysicalRecord(type, ptr, fragment_length);
        ptr += fragment_length;
        left -= fragment_length;
        begin = false;
    } while(s.ok() && left > 0);
    return s;
}

Status Writer::EmitPhysicalRecord(RecordType t, const char* ptr, size_t length) {
    assert(length <= 0xffff);  // Must fit in two bytes, because only two bytes were left for it
    assert(block_offset_ + kHeaderSize + length <= kBlockSize);

    char buf[kHeaderSize];
    buf[4] = static_cast<char>(length & 0xff);
    buf[5] = static_cast<char>(length >> 8);
    buf[6] = static_cast<char>(t);

    uint32_t crc = crc32c::Extend(type_crc_[t], ptr, length);
    crc = crc32c::Mask(crc);
    EncodeFixed32(buf, crc);

    Status s = dest_->Append(Slice(buf, kHeaderSize));
    if (s.ok()) {
        s = dest_->Append(Slice(ptr, length));
        if (s.ok()) {
            s = dest_->Flush();
        }
    }
    block_offset_ += kHeaderSize + length;
    return s;
}

}
}