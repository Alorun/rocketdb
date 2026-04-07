#include "block.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>

#include "../../include/comparator.h"
#include "format.h"
#include "../util/coding.h"
#include "../util/logging.h"

namespace rocketdb {

// The last four bytes represent the array length of the restart points
inline uint32_t Block::NumRestarts() const {
    assert(size_ >= sizeof(uint32_t));
    return DecodeFixed32(data_ + size_ - sizeof(uint32_t));
}

Block::Block(const BlockContents& contents) : data_(contents.data.data()), 
        size_(contents.data.size()), owned_(contents.heap_allocated) {
    if (size_ < sizeof(uint32_t)) {
        size_ = 0;  // Error marker
    } else {
        // One size of restart is uint32_t
        size_t max_restarts_allowed = (size_ - sizeof(uint32_t)) / sizeof(uint32_t);
        if (NumRestarts() > max_restarts_allowed) {
            size_ = 0;
        } else {
            restart_offset_ = size_ - (1 + NumRestarts() * sizeof(uint32_t));
        }
    }
}

Block::~Block() {
    if (owned_) {
        delete[] data_;
    }
}

static inline const char* DecodeEntry(const char* p, const char* limit,
    uint32_t* shared, uint32_t* non_shared, uint32_t* value_length) {
    if (limit - p < 3) return nullptr;
    *shared = reinterpret_cast<const uint8_t*>(p)[0];
    *non_shared = reinterpret_cast<const uint8_t*>(p)[1];
    *value_length = reinterpret_cast<const uint8_t*>(p)[2];
    if ((*shared | *non_shared | *value_length) < 128) {
        // Fast path: all three values are encoded in one byte each.
        // Three numbers use varint format.
        p += 3;
    } else {
        if ((p = GetVarint32Ptr(p, limit, shared)) == nullptr) return nullptr;
        if ((p = GetVarint32Ptr(p, limit, non_shared)) == nullptr) return nullptr;
        if ((p = GetVarint32Ptr(p, limit, value_length)) == nullptr) return nullptr;
    }

    if (static_cast<uint32_t>(limit - p) < (*non_shared + *value_length)) {
        return nullptr;
    }
    return p;
}

// Block format: data -- restarts array -- the num of restarts array
// Block internal format: Shared_num -- Non_shared_num -- Value_length -- Key -- Value
class Block::Iter : public Iterator {
    public:
        Iter(const Comparator* comparator, const char* data, uint32_t restarts, uint32_t num_restarts)
            : comparator_(comparator), data_(data), restarts_(restarts), num_restarts_(num_restarts),
              current_(restarts_), restart_index_(num_restarts_) {
            assert(num_restarts_ > 0);
        }

        bool Valid() const override { return current_ < restarts_; }
        Status status() const override { return status_; }
        Slice key() const override { 
            assert(Valid());
            return key_;
        }
        Slice value() const override {
            assert(Valid());
            return value_;
        }

        void Next() override {
            assert(Valid());
            ParseNextKey();
        }

        void Prev() override {
            assert(Valid());

            const uint32_t original = current_;
            while (GetRestartPoint(restart_index_) >= original) {
                if (restart_index_ == 0) {
                    // no more entries
                    current_ = restarts_;
                    restart_index_ = num_restarts_;
                    return;
                }
                restart_index_--;
            }

            SeekToRestartPoint(restart_index_);
            do {

            } while (ParseNextKey() && NextEntryOffset() < original);
        }

        void Seek(const Slice& target) override {
            uint32_t left = 0;
            uint32_t right = num_restarts_ - 1;
            int current_key_compare = 0;

            if (Valid()) {
                current_key_compare = Compare(key(), target);
                if (current_key_compare < 0) {
                    left = restart_index_;
                } else if (current_key_compare > 0) {
                    right = restart_index_;
                } else {
                    return;
                }
            }

            while (left < right) {
                uint32_t mid = (left + right + 1) / 2;
                uint32_t region_offset = GetRestartPoint(mid);
                uint32_t shared, non_shared, value_length;
                const char* key_ptr = DecodeEntry(data_ + region_offset, data_ + restarts_,
                                            &shared, &non_shared, &value_length);
                if (key_ptr == nullptr || (shared != 0)) {
                    CorruptionError();
                    return;
                }
                Slice mid_key(key_ptr, non_shared);
                if (Compare(mid_key, target) < 0) {
                    left = mid;
                } else {
                    right = mid - 1;
                }
            }

            assert(current_key_compare == 0 || Valid());
            bool skip_seek = left == restart_index_ && current_key_compare < 0;
            if (!skip_seek) {
                SeekToRestartPoint(left);
            }

            while (true) {
                if (!ParseNextKey()) {
                    return;
                }
                if (Compare(key_, target) >= 0) {
                    return;
                }
            }
        }

        void SeekToFirst() override {
            SeekToRestartPoint(0);
            ParseNextKey();
        }

        void SeekToLast() override {
            SeekToRestartPoint(num_restarts_ - 1);
            while (ParseNextKey() && NextEntryOffset() < restarts_) {

            }
        }

    private:
        inline int Compare(const Slice& a, const Slice& b) const {
            return comparator_->Compare(a, b);
        }

        // Return the offset in data_ just past the end of the current entry
        inline uint32_t NextEntryOffset() const {
            return (value_.data() + value_.size()) - data_;
        }
        
        // Get the offset of the index restart point in the block
        uint32_t GetRestartPoint(uint32_t index) {
            assert(index < num_restarts_);
            return DecodeFixed32(data_ + restarts_ + index * sizeof(uint32_t));
        }

        // Reach the restart location of the index
        void SeekToRestartPoint(uint32_t index) {
            key_.clear();
            restart_index_ = index;

            uint32_t offset = GetRestartPoint(index);
            value_ = Slice(data_ + offset);
        }

        void CorruptionError() {
            current_ = restarts_;
            restart_index_ = num_restarts_;
            status_ = Status::Corruption("bad entry in block");
            key_.clear();
            value_.clear();
        }

        bool ParseNextKey() {
            current_ = NextEntryOffset();
            const char* p = data_ + current_;
            const char* limit = data_ + restarts_;
            if ( p >= limit) {
                // No more entries
                current_ = restarts_;
                restart_index_ = num_restarts_;
                return false;
            }

            uint32_t shared, non_shared, value_length;
            p = DecodeEntry(p, limit, &shared, &non_shared, &value_length);
            if (p == nullptr || key_.size() < shared) {
                CorruptionError();
                return false;
            } else {
                key_.resize(shared);
                key_.append(p, non_shared);
                value_ = Slice(p + non_shared, value_length);
                while (restart_index_ + 1 < num_restarts_ && GetRestartPoint(restart_index_ + 1) < current_) {
                    ++restart_index_;
                }
                return true;
            }
        }

        const Comparator* const comparator_;

        // Fixed var
        const char* const data_;        // The beginning of block
        uint32_t const restarts_;       // The ending of valid data and the beginning of restart array
        uint32_t const num_restarts_;   // The number of restart

        // Var var
        uint32_t current_;          // Current point
        uint32_t restart_index_;    // Current restart index
        std::string key_;           // Current key
        Slice value_;               // Current value
        Status status_;
};

Iterator* Block::NewIterator(const Comparator* comparator) {
    if (size_ < sizeof(uint32_t)) {
        return NewErrorIterator(Status::Corruption("bad clock contents"));
    } 
    const uint32_t num_restarts = NumRestarts();
    if (num_restarts == 0) {
        return NewEmptyIterator();
    } else {
        return new Iter(comparator, data_, restart_offset_, num_restarts);
    }
}

}