#include "db_iter.h"

#include "db_impl.h"
#include "dbformat.h"
#include "filename.h"
#include "../port/port.h"
#include "../util/logging.h"
#include "../util/mutexlock.h"
#include "../util/random.h"
#include "../../include/env.h"
#include "../../include/iterator.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace rocketdb {

#if 0
static void DumpInternalIter(Iterator* iter) {
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
        ParsedInternalKey k;
        if (!ParseInternalKey(iter->key(), &k)) {
            std::fprintf(stderr, "Corrput '%s'\n", EscapeString(iter->key()).c_str());
        } else {
            std::fprintf(stderr, "@ '%s'\n", k.DebugString().c_str());
        }
    }
}
#endif

namespace  {

class DBIter : public Iterator {
    public:
        enum Direction { kForward, kReverse };

        DBIter(DBImpl* db, const Comparator* cmp, Iterator* iter, SequenceNumber s, uint32_t seed)
            : db_(db), 
                user_comparator_(cmp), 
                iter_(iter), 
                sequence_(s),
                direction_(kForward),
                valid_(false),
                rnd_(seed),
                bytes_until_read_sampling_(RandomCompactionPeriod()) {}

        DBIter(const DBIter&) = delete;
        DBIter& operator=(const DBIter&) = delete;
    
        ~DBIter() override { delete iter_; }
        bool Valid() const override { return valid_; }
        Slice key() const override {
            assert(valid_);
            return (direction_ == kForward) ? ExtractUserKey(iter_->key()) : saved_key_;
        }
        Slice value() const override {
            assert(valid_);
            return (direction_ == kForward) ? iter_->value() : saved_value_;
        }
        Status status() const override {
            if (status_.ok()) {
                return iter_->status();
            } else {
                return status_;
            }
        }

        void Next() override;
        void Prev() override;
        void Seek(const Slice& target) override;
        void SeekToFirst() override;
        void SeekToLast() override;

    private:
        void FindNextUserEntry(bool skipping, std::string* skip);
        void FindPrevUserEntry();
        bool ParseKey(ParsedInternalKey* key);

        inline void SaveKey(const Slice& k, std::string* dst) {
            dst->assign(k.data(), k.size());
        }

        inline void ClearSaveValue() {
            // The size of save_value > 1Mb
            if (saved_value_.capacity() > 1048576) {
                std::string empty;
                swap(empty, saved_value_);
            } else {
                saved_value_.clear();
            }
        }

        size_t RandomCompactionPeriod() {
            return rnd_.Uniform(2 * config::kReadBytesPeriod);
        }


        DBImpl* db_;
        const Comparator* const user_comparator_;
        Iterator* const iter_;
        SequenceNumber const sequence_;
        Status status_;
        std::string saved_key_;
        std::string saved_value_;
        Direction direction_;
        bool valid_;
        Random rnd_;
        size_t bytes_until_read_sampling_;
};

void DBIter::Next() {}

void DBIter::FindNextUserEntry(bool skipping, std::string* skip) {}

void DBIter::Prev() {}

void DBIter::FindPrevUserEntry() {}

void DBIter::Seek(const Slice& target) {}

void DBIter::SeekToFirst() {}

void DBIter::SeekToLast() {}

}

Iterator* NewDBIterator(DBImpl* db, const Comparator* user_key_comparator, Iterator* internal_iter,
                        SequenceNumber sequence, uint32_t seed) {
    return new DBIter(db, user_key_comparator, internal_iter, sequence, seed);
}

}
