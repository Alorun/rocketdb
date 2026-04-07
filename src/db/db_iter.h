#pragma once

#include <cstdint>

#include "dbformat.h"
#include "../../include/db.h"

namespace rocketdb {

class DBImpl;

// Return a new iterator that converts internal keys.
// The keys are live at seqecified "sequence" number into appropriate user keys.
Iterator* NewDBIterator(DBImpl* db, const Comparator* user_key_comparator, Iterator* internal_iter,
                        SequenceNumber sequence, uint32_t seed);

}