#pragma once

#include <cstdint>

#include "env.h"
#include "iterator.h"
#include "options.h"
#include "slice.h"
#include "status.h"

namespace rocketdb {

class Block;
class BlockHandle;
class Footer;
class Options;
class RandomAccessFile;
class ReadOptions;
class TableCache;

// A Table is a sortec map from strings to strings
// Tables are immutable and persistend
// A Table may be safely accessed from multiple threads without external synchronization
class Table {
    public:
        // Attemt to open the table that is stored in bytes of file
        // Read the metadata entries neccessary to allow retrieving data from the table
        static Status Open(const Options& options, RandomAccessFile* file, uint64_t file_size, Table** table);

        Table(const Table&) = delete;
        Table& operator=(const Table&) = delete;

        ~Table();

        // Return a new iterator is initially invalid over the table contents
        Iterator* NewIterator(const ReadOptions&) const;

        // Given a key, return an approxmate byte offset in the file where the data for that key bugins
        uint64_t ApproximateOffsetOf(const Slice& key) const;

    private:
        friend class TableCache;
        struct Rep;

        static Iterator* BlockReader(void*, const ReadOptions&, const Slice&);

        explicit Table(Rep* rep) : rep_(rep) {}

        Status InternalGet(const ReadOptions&, const Slice& key, void* arg,
                           void (*handle_result)(void* arg, const Slice& k, const Slice& v));

        void ReadMeta(const Footer& footer);
        void ReadFilter(const Slice& filter_handle_value);

        Rep* const rep_;
};

}