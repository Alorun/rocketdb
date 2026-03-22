#pragma once

#include "dbformat.h"
#include "../../include/write_batch.h"
#include "memtable.h"

namespace rocketdb {

class MemTable;

// WriteBatchInternal provides static methods for manipulating a
// WriteBatch that we don't want in the public WriteBatch interface.
class WriteBatchInternal {
    public:
        // Return the number of entries in the batch (9 ~ 12 bytes)
        static int Count(const WriteBatch* batch);

        // Set the count for the number fo entries in the batch
        static void SetCount(WriteBatch* batch, int n);

        // Return the sequence number for the start of this batch (1 ~ 8 bytes)
        static SequenceNumber Sequence(const WriteBatch* batch);

        // Store the specified number as the sequence number
        static void SetSequence(WriteBatch* batch, SequenceNumber seq);

        static Slice Contents(const WriteBatch* batch) { return Slice(batch->rep_); } 

        static size_t ByteSize(const WriteBatch* batch) { return batch->rep_.size(); }

        // Directly replace rep_, typically used during the restart recovery phase
        static void SetContents(WriteBatch* batch, const Slice& contents);

        static Status InsertInto(const WriteBatch* batch, MemTable* memtable);

        static void Append(WriteBatch* dst, const WriteBatch* src);
};

}