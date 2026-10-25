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

        DBIter(DBImpl* db, const Comparator* cmp, Iterator* iter, SequenceNumber s,
               uint32_t seed, size_t max_sequential_skip)
            : db_(db), 
                user_comparator_(cmp), 
                iter_(iter), 
                sequence_(s),
                max_sequential_skip_(max_sequential_skip),
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
        void SeekBeforeUserKey(const std::string& user_key);
        bool FindValueForCurrentKeyUsingSeek(const std::string& user_key);
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
        const size_t max_sequential_skip_;
        Status status_;
        std::string saved_key_;
        std::string saved_value_;
        Direction direction_;
        bool valid_;
        Random rnd_;
        size_t bytes_until_read_sampling_;
};

inline bool DBIter::ParseKey(ParsedInternalKey* ikey) {
    Slice k = iter_->key();

    size_t bytes_read = k.size() + iter_->value().size();
    while (bytes_until_read_sampling_ < bytes_read) {
        bytes_until_read_sampling_ += RandomCompactionPeriod();
        db_->RecordReadSample(k);
    }
    assert(bytes_until_read_sampling_ >= bytes_read);
    bytes_until_read_sampling_ -= bytes_read;

    if (!ParseInternalKey(k, ikey)) {
        status_ = Status::Corruption("corrupted internal key in DBIter");
        return false;
    } else {
        return true;
    }
}

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
    std::string counted_key;
    std::string seek_target;
    size_t skipped = 0;
    bool has_counted_key = false;
    bool sought_snapshot = false;
    bool sought_past_key = false;
    do {
        ParsedInternalKey ikey;
        const bool parsed = ParseKey(&ikey);
        if (parsed && max_sequential_skip_ != 0 &&
            (!has_counted_key ||
             user_comparator_->Compare(ikey.user_key, counted_key) != 0)) {
            SaveKey(ikey.user_key, &counted_key);
            has_counted_key = true;
            skipped = 0;
            sought_snapshot = false;
            sought_past_key = false;
        }
        if (parsed && ikey.sequence <= sequence_) {
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
        if (parsed && max_sequential_skip_ != 0) {
            // Saturate so even SIZE_MAX is a usable threshold.
            if (skipped < max_sequential_skip_) ++skipped;
            const bool skip_current_key =
                skipping && user_comparator_->Compare(ikey.user_key, *skip) <= 0;
            bool& sought = skip_current_key ? sought_past_key : sought_snapshot;
            if (skipped >= max_sequential_skip_ && !sought) {
                sought = true;
                seek_target.clear();
                AppendInternalKey(&seek_target, ParsedInternalKey(
                    counted_key, skip_current_key ? 0 : sequence_,
                    skip_current_key ? kTypeDeletion : kValueTypeForSeek));
                // A sequence-zero boundary can be a real record. Never seek
                // backward or repeatedly seek to the same internal key.
                const InternalKeyComparator comparator(user_comparator_);
                if (comparator.Compare(iter_->key(), seek_target) < 0) {
                    skipped = 0;
                    iter_->Seek(seek_target);
                    continue;
                }
            }
        } else if (!parsed) {
            has_counted_key = false;
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
        size_t skipped = 0;
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
            if (max_sequential_skip_ != 0 && ++skipped >= max_sequential_skip_) {
                SeekBeforeUserKey(saved_key_);
                if (!iter_->Valid()) {
                    valid_ = false;
                    saved_key_.clear();
                    ClearSaveValue();
                    return;
                }
                break;
            }
        }
        direction_ = kReverse;
    }

    FindPrevUserEntry();
}

void DBIter::SeekBeforeUserKey(const std::string& user_key) {
    std::string target;
    AppendInternalKey(&target,
                      ParsedInternalKey(user_key, kMaxSequenceNumber, kValueTypeForSeek));
    iter_->Seek(target);
    if (iter_->Valid()) {
        iter_->Prev();
    } else if (iter_->status().ok()) {
        iter_->SeekToLast();
    }
}

bool DBIter::FindValueForCurrentKeyUsingSeek(const std::string& user_key) {
    std::string target;
    AppendInternalKey(&target, ParsedInternalKey(user_key, sequence_, kValueTypeForSeek));
    iter_->Seek(target);
    bool found = false;
    ParsedInternalKey ikey;
    if (iter_->Valid() && ParseKey(&ikey) && ikey.sequence <= sequence_ &&
        user_comparator_->Compare(ikey.user_key, user_key) == 0 &&
        ikey.type == kTypeValue) {
        SaveKey(ikey.user_key, &saved_key_);
        if (saved_value_.capacity() > iter_->value().size() + 1048576) {
            std::string empty;
            swap(empty, saved_value_);
        }
        saved_value_.assign(iter_->value().data(), iter_->value().size());
        found = true;
    }
    // Preserve a seek/parse error before a second seek could replace its state.
    if (!iter_->status().ok() && status_.ok()) status_ = iter_->status();
    if (!status_.ok()) return false;
    if (!found) {
        saved_key_.clear();
        ClearSaveValue();
    }
    // Reverse DBIter keeps the internal cursor before the returned user key.
    SeekBeforeUserKey(user_key);
    return found;
}

void DBIter::FindPrevUserEntry() {
    assert(direction_ == kReverse);

    // Reverse scan may firstly encounter a old value.
    // We need save the value util the new value is encountered
    ValueType value_type = kTypeDeletion;
    std::string counted_key;
    size_t skipped = 0;
    bool has_counted_key = false;
    if (iter_->Valid()) {
        do {
            ParsedInternalKey ikey;
            const bool parsed = ParseKey(&ikey);
            if (parsed && max_sequential_skip_ != 0) {
                if (value_type != kTypeDeletion &&
                    user_comparator_->Compare(ikey.user_key, saved_key_) < 0) {
                    break;
                }
                if (!has_counted_key ||
                    user_comparator_->Compare(ikey.user_key, counted_key) != 0) {
                    SaveKey(ikey.user_key, &counted_key);
                    has_counted_key = true;
                    skipped = 0;
                }
                if (skipped < max_sequential_skip_) ++skipped;
                if (skipped >= max_sequential_skip_) {
                    value_type = FindValueForCurrentKeyUsingSeek(counted_key)
                                     ? kTypeValue : kTypeDeletion;
                    if (!status().ok()) {
                        value_type = kTypeDeletion;
                        break;
                    }
                    if (value_type == kTypeValue) break;
                    has_counted_key = false;
                    continue;
                }
            } else if (!parsed) {
                has_counted_key = false;
            }
            if (parsed && ikey.sequence <= sequence_) {
                if ((value_type != kTypeDeletion) && user_comparator_->Compare(ikey.user_key, saved_key_) < 0) {
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
                        SequenceNumber sequence, uint32_t seed,
                        size_t max_sequential_skip_in_iterations) {
    return new DBIter(db, user_key_comparator, internal_iter, sequence, seed,
                      max_sequential_skip_in_iterations);
}

}
