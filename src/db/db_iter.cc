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

void DBIter::Next() {
    assert(valid_);
    
    // Switch directions?
    if (direction_ == kReverse) {
        direction_ = kForward;

        // Advance into the range of entries for this->key().
        // Then use the normal skipping code below.
        if (!iter_->Valid()) {
            iter_->SeekToFirst(); 
        } else {
            iter_->Next();
        }
        if (!iter_->Valid()) {
            valid_ = false;
            saved_key_.clear();
            return;
        }
        // save_key_ already contains the key to skip past.
    } else {
        // Store in saved_key the current key so we skip it below/.
        SaveKey(ExtractUserKey(iter_->key()), &saved_key_);

        iter_->Next();
        if (!iter_->Valid()) {
            valid_ = false;
            saved_key_.clear();
            return;
        }
    }

    FindNextUserEntry(true, &saved_key_);
}

void DBIter::FindNextUserEntry(bool skipping, std::string* skip) {
    // Loop until we hit an acceptable entry to yield
    assert(iter_->Valid());
    assert(direction_ == kForward);
    do {
        ParsedInternalKey ikey;
        if (ParseKey(&ikey) && ikey.sequence <= sequence_) {
            switch (ikey.type) {
                case kTypeDeletion:
                    // Arrange to skip all upcoming entries for this key since they are hidden by this deletion.
                    SaveKey(ikey.user_key, skip);
                    skipping = true;
                    break;
                case kTypeValue:
                    if (skipping && user_comparator_->Compare(ikey.user_key, *skip) <= 0) {
                        // Entry hidden
                    } else {
                        valid_ = true;
                        saved_key_.clear();
                        return;
                    }
                    break;
            }
        }
        iter_->Next();
    } while (iter_->Valid());
    saved_key_.clear();
    valid_ = false;
}

void DBIter::Prev() {
    assert(valid_);

    // Switch direction?
    if (direction_ == kForward) {
        assert(iter_->Valid());
        // Save th current entry.
        SaveKey(ExtractUserKey(iter_->key()), &saved_key_);
        while (true) {
            iter_->Prev();
            if (!iter_->Valid()) {
                valid_ = false;
                saved_key_.clear();
                ClearSaveValue();
                return;
            } 
            if (user_comparator_->Compare(ExtractUserKey(iter_->key()), saved_key_) < 0) {
                break;
            }
        }
        direction_ = kReverse;
    }

    FindPrevUserEntry();
}

void DBIter::FindPrevUserEntry() {
    assert(direction_ == kReverse);

    // Reverse scan may firstly encounter a old value.
    // We need save the value util the new value is encountered
    ValueType value_type = kTypeDeletion;
    if (iter_->Valid()) {
        do {
            ParsedInternalKey ikey;
            if (ParseKey(&ikey) && ikey.sequence <= sequence_) {
                if ((value_type != kTypeDeletion) && user_comparator_->Compare(ikey.user_key, saved_value_) < 0) {
                    // We encountered a non_deleted value in entries for previous keys.
                    break;
                }
                value_type = ikey.type;
                if (value_type == kTypeDeletion) {
                    saved_key_.clear();
                    ClearSaveValue();
                } else {
                    Slice raw_value = iter_->value();
                    if (saved_value_.capacity() > raw_value.size() + 1048576) {
                        std::string empty;
                        swap(empty, saved_value_);
                    }
                    SaveKey(ExtractUserKey(iter_->key()), &saved_key_);
                    saved_value_.assign(raw_value.data(), raw_value.size());
                }
            }
            iter_->Prev();            
        } while (iter_->Valid());
    }

    if (value_type == kTypeDeletion) {
        // It's gone in the oppsite direction
        valid_ = false;
        saved_key_.clear();
        ClearSaveValue();
        direction_ = kForward;
    } else {
        valid_ = true;
    }
}

void DBIter::Seek(const Slice& target) {
    direction_ = kForward;
    ClearSaveValue();
    saved_key_.clear();
    AppendInternalKey(&saved_key_, ParsedInternalKey(target, sequence_, kValueTypeForSeek));
    iter_->Seek(saved_key_);
    if (iter_->Valid()) {
        FindNextUserEntry(false, &saved_key_);
    } else {
        valid_ = false;
    }
}

void DBIter::SeekToFirst() {
    direction_ = kForward;
    ClearSaveValue();
    iter_->SeekToFirst();
    if (iter_->Valid()) {
        FindNextUserEntry(false, &saved_key_);
    } else {
        valid_ = false;
    }
}

void DBIter::SeekToLast() {
    direction_ = kReverse;
    ClearSaveValue();
    iter_->SeekToLast();
    FindPrevUserEntry();
}

}

Iterator* NewDBIterator(DBImpl* db, const Comparator* user_key_comparator, Iterator* internal_iter,
                        SequenceNumber sequence, uint32_t seed) {
    return new DBIter(db, user_key_comparator, internal_iter, sequence, seed);
}

}
