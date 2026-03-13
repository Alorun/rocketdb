#include "memtable.h"

#include "dbformat.h"
#include "../../include/iterator.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace rocketdb {

// Storage structure
// len + string
// High efficiency return fixed slice
static Slice GetLengthPrefixedSlice(const char* data) {
    uint32_t len;
    const char* p = data;
    p = GetVarint32Ptr(p, p + 5, &len);
    return Slice(p, len);
}

MemTable::MemTable(const InternalKeyComparator& comparator)
    : comparator_(comparator), refs_(0), table_(comparator_, &arena_) {}

MemTable::~MemTable() { assert(refs_ == 0); }

size_t MemTable::ApproximateMemoeyUsage() { return arena_.MemoryUsage(); }

int MemTable::KeyComparator::operator()(const char* aptr, const char* bptr) const {
    Slice a = GetLengthPrefixedSlice(aptr);
    Slice b = GetLengthPrefixedSlice(bptr);
    return comparator.Compare(a,b);
}

static const char* EncodeKey(std::string* scrach, const Slice& target) {
    scrach->clear();
    PutVarint32(scrach, target.size());
    scrach->append(target.data(), target.size());
    return scrach->data();
}

class MeTableIterator : public Iterator {
    public:
        explicit MeTableIterator(MemTable::Table* table) : iter_(table) {}

        MeTableIterator(const MeTableIterator&) = delete;
        MeTableIterator& operator=(const MeTableIterator&) = delete;

        ~MeTableIterator() override = default;

        bool Valid() const override { return iter_.Valid(); }
        void Seek(const Slice& k) override { iter_.Seek(EncodeKey(&tmp_, k)); }
        void SeekToFirst() override { iter_.SeekToFirst(); }
        void SeekToLast() override { iter_.SeekToLast(); }
        void Next() override { iter_.Next(); }
        void Prev() override { iter_.Prev(); }
        Slice key() const override { return GetLengthPrefixedSlice(iter_.key()); }
        Slice value() const override {
            Slice key_slice = GetLengthPrefixedSlice(iter_.key());
            return GetLengthPrefixedSlice(key_slice.data() + key_slice.size());
        }

        Status status() const override { return Status::OK(); }

    private:
        MemTable::Table::Iterator iter_;
        std::string tmp_;

};

Iterator* MemTable::NewIterator() { return new MemTableIterator(&table); }

void MemTable::Add(SequenceNumber s, ValueType type, const Slice& key, const Slice& value) {
    // Internal key length : varint32 byte
    // User key : key_size
    // Sequence and type : 8 byte
    // Value length : varint32 byte
    // Value : value_size
    size_t key_size = key.size();
    size_t val_size = value.size();
    size_t internal_key_size = key_size + 8;
    const size_t encoded_len = VartintLength(internal_key_size) + internal_key_size + 
                                                VartintLength(val_size) + val_size;
    char* buf =  arena_.Allocate(encoded_len);
    char* p = EncodeVarint32(buf, internal_key_size);
    std::memcpy(p, key.data(), key_size);
    p += key_size;
    EncodeFixed64(p, (s << 8) | type);
    p += 8;
    p = EncodeVarint32(p, val_size);
    std::memcpy(p, value.data(), val_size);
    assert(p + val_size == buf + encoded_len);
    table_.Insert(buf);
}

bool MemTable::Get(const LookupKey& key, std::string* value, Status* s) {
    // Klength : varint32 (include tag size)
    // Userkey : char[klength]
    // Tag : uint64
    // Vlength : varint32
    // Value : char[vlength]
    Slice memkey = key.memtable_key();
    Table::Iterator iter(&table_);
    iter.Seek(memkey.data());
    if (iter.Valid()) {
        const char* entry = iter.key();
        uint32_t key_length;
        const char* key_ptr = GetVarint32Ptr(entry, entry + 5, &key_length);
        if (comparator_.comparator.user_comparator()->Compare(Slice(key_ptr, key_length - 8), key.user_key()) == 0) {
            const uint64_t tag = DecodeFixed64(key_ptr + key_length - 8);
            switch (static_cast<ValueType>(tag & 0xff)) {
                case kTypeValue: {
                    Slice v = GetLengthPrefixedSlice(key_ptr + key_length);
                    value->assign(v.data(), v.size());
                    return true;
                }
                case kTypeDeletion: {
                    *s = Status::NotFound(Slice());
                    return true;
                }
            }
        }
    }
    return false;
}

}
