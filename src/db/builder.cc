#include "builder.h"

#include "dbformat.h"
#include "filename.h"
#include "table_cache.h"
#include "version_edit.h"
#include "../../include/db.h"
#include "../../include/env.h"
#include "../../include/iterator.h"
#include "version_set.h"
#include "../../include/table_builder.h"
#include <cassert>

namespace rocketdb {

Status BuildTable(const std::string& dbname, Env *env, const Options &options, 
                  TableCache *table_cache, Iterator *iter, FileMetaData *meta,
                  const FlushPruningOptions* pruning) {
    assert(pruning == nullptr || pruning->user_comparator != nullptr);
    Status s;
    meta->file_size = 0;
    iter->SeekToFirst();

    std::string fname = TableFileName(dbname, meta->number);
    if (iter->Valid()) {
        // Create new file
        WritableFile* file;
        s = env->NewWritableFile(fname, &file);
        if (!s.ok()) {
            return s;
        }

        TableBuilder* builder = new TableBuilder(options, file);
        std::string current_user_key;
        bool has_previous_key = false;
        SequenceNumber previous_sequence = 0;
        for (; iter->Valid(); iter->Next()) {
            const Slice key = iter->key();
            bool drop = false;
            if (pruning != nullptr) {
                ParsedInternalKey parsed;
                if (!ParseInternalKey(key, &parsed)) {
                    // Preserve malformed keys, just as major compaction does,
                    // and do not let them hide neighboring records.
                    has_previous_key = false;
                    current_user_key.clear();
                } else {
                    if (has_previous_key &&
                        pruning->user_comparator->Compare(
                            parsed.user_key, Slice(current_user_key)) == 0) {
                        // A newer version is visible to even the oldest
                        // snapshot, so this version can never be observed.
                        drop = previous_sequence <= pruning->smallest_snapshot;
                    } else {
                        current_user_key.assign(parsed.user_key.data(),
                                                parsed.user_key.size());
                        has_previous_key = true;
                    }
                    previous_sequence = parsed.sequence;
                }
            }
            if (drop) continue;

            if (builder->NumEntries() == 0) {
                meta->smallest.DecodeFrom(key);
            }
            meta->largest.DecodeFrom(key);
            builder->Add(key, iter->value());
        }

        // Finish and check for builder errors
        s = builder->Finish();
        if (s.ok()) {
            meta->file_size = builder->FileSize();
            assert(meta->file_size > 0);
        }
        delete builder;

        // Finish and check for file errors
        if (s.ok()) {
            s = file->Sync();
        }
        if (s.ok()) {
            s = file->Close();
        }
        delete file;
        file = nullptr;

        if (s.ok()) {
            // Verify that the table is usable
            Iterator* it = table_cache->NewIterator(ReadOptions(), meta->number, meta->file_size);

            s = it->status();
            delete it;
        }
    }

    // Check for input iterator errors
    if (!iter->status().ok()) {
        s = iter->status();
    }

    if (s.ok() && meta->file_size > 0) {

    } else {
        env->RemoveFile(fname);
    }
    return s;
}

}
