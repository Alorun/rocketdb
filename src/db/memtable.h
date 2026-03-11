#pragma once

#include <cassert>
#include <cstddef>
#include <string>

#include "dbformat.h"
#include "skiplist.h"
#include "../util/arena.h"
#include "../../include/iterator.h"

namespace rocketdb {

class InternalKeyComparator;
class MeTableIterator;

class MemTable {
    public:
        explicit MemTable(const InternalKeyComparator& comparator);

        MemTable(const MemTable&) = delete;
        MemTable& operator=(const MemTable&) = delete;

        void Ref() { ++refs_; }

        // Responsible for destroying and releasing memory
        void Unref() {
            --refs_;
            assert(refs_ >= 0);
            if (refs_ <= 0) {
                delete this;
            }
        }

        // Return an estimate of the number of bytes of data in use by this data structrue
        size_t ApproximateMemoeyUsage();

        Iterator* NewIterator();

        // Add an entry into memtable that maps key to value at the seq number and type
        void Add(SequenceNumber seq, ValueType type, const Slice& key, const Slice& value);

        // If memtable contains a value for key, store it in value and return true
        // If memtable contains a deletion for key, store a error in status ans return true
        // Else, return false
        bool Get(const LookupKey& key, std::string* value, Status* s);

    private:
        friend class MemTableIterator;
        friend class MemTableBackwardIterator;

        struct KeyComparator {
            const InternalKeyComparator comparator;
            explicit KeyComparator(const InternalKeyComparator& c) : comparator(c) {}
            int operator()(const char* a, const char* b) const;
        };

        typedef SkipList<const char*, KeyComparator> Table;

        ~MemTable();

        KeyComparator comparator;
        int refs_;
        Arena arena_;
        Table table_;
};


}