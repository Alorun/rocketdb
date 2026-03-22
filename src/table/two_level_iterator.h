#pragma once

#include "../../include/iterator.h"

namespace rocketdb {

struct ReadOptions;

// Return a new two level iterator, a two-level iterator contains an index iterator 
// whose values point to a sequence of blocks 
// where each block is itself a sequence of key/value pair
Iterator* NewTwoLevelIterator(Iterator* index_iter,
    Iterator* (*block_function)(void* arg, const ReadOptions& options, const Slice& index_value), 
    void* arg, const ReadOptions& options);

}

