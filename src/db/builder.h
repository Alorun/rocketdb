#pragma once

#include "../../include/status.h"
#include "version_edit.h"
#include "version_set.h"

namespace rocketdb {

struct Options;

class Comparator;
class Env;
class Iterator;
class TableCache;
class VersionEdit;

// Runtime flushes may discard versions hidden from every active snapshot.
// The iterator must be ordered by internal key using this user comparator.
// Tombstones are retained unless hidden by a newer version of the same key.
struct FlushPruningOptions {
    const Comparator* user_comparator;
    SequenceNumber smallest_snapshot;
};

// Build a Table file from the contents of *iter
// The generated file will be named according to mate->number
// On success, the rest of *meta will be filled with metadata about the generated table
// If not data is present in *iter, meta->file_size is zero, and no Table file will be produced
// A null pruning argument preserves all versions (used by recovery and repair).
Status BuildTable(const std::string& dbname, Env* env, const Options& options, TableCache* table_cache, 
                  Iterator* iter, FileMetaData* meta,
                  const FlushPruningOptions* pruning = nullptr);

}
