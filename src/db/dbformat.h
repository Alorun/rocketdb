#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>

#include "../../include/comparator.h"
#include "../../include/slice.h"
#include "../util/coding.h"
#include "../../include/filter_policy.h"

namespace rocketdb {

// Core tuning parameters
namespace config {

    static const int kNumLevels = 7;

    static const int kLO_CompactionTrigger = 4;

    static const int kLO_SlowdownWritesTrigger = 8;

    static const int kLO_StopWritesTrigger = 12;

    static const int kMaxMeCompactLevel = 2;

    static const int kReadBytesPeriod = 1048576;

}

class InternalKey;

// the last component of internal keys
enum ValueType { kTypeDeletion = 0x0, kTypeValue = 0x1 };

static const ValueType kValueTypeForSeek = kTypeValue;

typedef uint64_t SequenceNumber;

static const SequenceNumber kMaxSequenceNumber = ((0x1ull << 56) - 1);


// innermost structure
struct ParsedInternalKey {
    Slice user_key;
    SequenceNumber sequence;
    ValueType type;

    ParsedInternalKey() {}
    ParsedInternalKey(const Slice& u, const SequenceNumber& seq, ValueType t) 
        : user_key(u), sequence(seq), type(t) {}
    
    std::string DebugString() const;
};

inline size_t InternalKeyEncodingLength(const ParsedInternalKey& key) {
    return key.user_key.size() + 8;
}

// Append the key to result
void AppendInternalKey(std::string* result, const ParsedInternalKey& key);

// Attemt to parese an internal key from 'internal_key' and stores the result
bool ParseInternalKey(const Slice& internal_key, ParsedInternalKey* result);

// Return the user key 
inline Slice ExtractUserKey(const Slice& internal_key) {
    assert(internal_key.size() >= 8);
    return Slice(internal_key.data(), internal_key.size() - 8);
}

class InternalKeyComparator : public Comparator {
    public:
        explicit InternalKeyComparator(const Comparator* c) : user_comparator_(c) {}
        const char* Name() const override;
        int Compare(const Slice& a, const Slice& b) const override;

        // Compress index size
        void FindShortestSeparator(std::string* start, const Slice& limit) const override;
        void FindShortSuccessor(std::string* key) const override;

        const Comparator* user_comparator() const { return user_comparator_; }

        int Compare(const InternalKey& a, const InternalKey& b) const;

    private:
        const Comparator* user_comparator_;
};

// Extract the key from the internal key to generate a filter for matching and lookup
class InternalFilterPolicy : public FilterPolicy {
    public:
        explicit InternalFilterPolicy(const FilterPolicy* p) : user_policy_(p) {}
        const char* Name() const override;
        void CreateFilter(const Slice* keys, int n, std::string* dst) const override;
        bool KeyMayMatch(const Slice& key, const Slice& filter) const override;

    private:
        const FilterPolicy* const user_policy_;
};


class InternalKey {
    public:
        InternalKey() {}
        InternalKey(const Slice& user_key, SequenceNumber s, ValueType t) {
            AppendInternalKey(&rep_, ParsedInternalKey(user_key, s, t));
        }

        // Loading a slice to rep
        bool DecodeFrom(const Slice& s) {
            rep_.assign(s.data(), s.size());
            return !rep_.empty();
        }

        // Return a slice*
        Slice Encode() const {
            assert(!rep_.empty());
            return rep_;
        }

        Slice user_key() const { return ExtractUserKey(rep_); }

        // Loading a internalkey to rep
        void SetFrom(const ParsedInternalKey& p) {
            rep_.clear();
            AppendInternalKey(&rep_, p);
        }

        void Clear() { rep_.clear(); }

        std::string DebugString() const;

    private:
        std::string rep_;  // Byte Buffer
};

inline int InternalKeyComparator::Compare(const InternalKey& a, const InternalKey& b) const {
    return Compare(a.Encode(), b.Encode());
}

inline bool ParseInternalKey(const Slice& internal_key, ParsedInternalKey* result) {
    const size_t n = internal_key.size();
    if (n < 8) return false;
    // First read tag (seq and type)
    uint64_t num = DecodeFixed64(internal_key.data() + n - 8);
    // Get type (eight bit)
    uint8_t c = num & 0xff;
    // The remain is seq
    result->sequence = num >> 8;
    result->type = static_cast<ValueType>(c);
    result->user_key = Slice(internal_key.data(), n - 8);
    return (c <= static_cast<uint8_t>(kTypeValue));
}

// Construct different internal query keys
class LookupKey {
    public:
        LookupKey(const Slice& user_key, SequenceNumber sequence);

        LookupKey(const LookupKey&) = delete;
        LookupKey& operator=(const LookupKey&) = delete;

        ~LookupKey();

        Slice memtable_key() const { return Slice(start_, end_ - start_); }

        Slice internal_key() const { return Slice(kstart_, end_ - kstart_); }

        Slice user_key() const { return Slice(kstart_, end_ - kstart_ - 8); }

    private:
        // klength   varint32       <-- start_
        // userkey   char[klength]  <-- kstart_
        // tag       uint64         <-- end_

        const char* start_;
        const char* kstart_; 
        const char* end_;
        char space_[200]; // Avoid allocation for short keys
};

inline LookupKey::~LookupKey() {
    if (start_ != space_) delete [] start_;
}


}