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

namespace rocketdb {

Status BuildTable(const std::string& dbname, Env *env, const Options &options, 
                  TableCache *table_cache, Iterator *iter, FileMetaData *meta) {
    Status s;
    meta->file_size = 0;
    iter->SeekToFirst();

    std::string fname = TableFileName(dbname, meta->number);
    if (iter->Valid()) {
        WritableFile* file;
        s = env->NewWriteFile(fname, &file);
        if (!s.ok()) {
            return s;
        }

        TableBuilder* builder = new TableBuilder(options, file);
        meta->smallest.DecodeFrom(iter->key());

    }

}

}
