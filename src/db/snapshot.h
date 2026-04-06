#pragma once

#include "dbformat.h"
#include "../../include/db.h"
#include <cassert>

namespace rocketdb {

class SnapshotList;

class SnapshotImpl : public Snapshot {
    public:
        SnapshotImpl(SequenceNumber sequence_number) :sequence_number_(sequence_number) {}

        SequenceNumber sequence_number() const { return sequence_number_; }

    private:
        friend class SnapshotList;

        SnapshotImpl* prev_;
        SnapshotImpl* next_;

        const SequenceNumber sequence_number_;

#if !defined (NDEBUG)
    SnapshotList* list_ = nullptr;
#endif
};

// It's a loop conected by a head_
// (head_) <-> n1 <-> n2 <-> n3 <-> (head_)
class SnapshotList {
    public:
        SnapshotList() : head_(0) {
            head_.prev_ = &head_;
            head_.next_ = &head_;
        }

        bool empty() const { return head_.next_ == &head_; }
        SnapshotImpl* oldest() const {
            assert(!empty());
            return head_.next_;
        }
        SnapshotImpl* newest() const {
            assert(!empty());
            return head_.prev_;
        }

        // Create a SnapshotImpl and appends it to the end of the list.
        SnapshotImpl* New(SequenceNumber sequence_number) {
            assert(empty() || newest()->sequence_number_ <= sequence_number);

            SnapshotImpl* snapshot = new SnapshotImpl(sequence_number);

#if !defined (NDEBUG)
        snapshot->list_ = this;
#endif
        snapshot->next_ = &head_;
        snapshot->prev_ = head_.prev_;

        snapshot->next_->prev_ = snapshot;  // head_->prev = snapshot
        snapshot->prev_->next_ = snapshot;  // head_->prev->next_ = snapshot
        return snapshot;
        }

        // Removes a SnapshotImpl from this list.
        void Delete(const SnapshotImpl* snapshot) {
#if !defined (NDEBUG)
        assert(snapshot->list_ == this);
#endif
        snapshot->prev_->next_ = snapshot->next_;
        snapshot->next_->prev_ = snapshot->prev_;
        delete snapshot;
        }

    private:
        SnapshotImpl head_;
};


}