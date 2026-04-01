#pragma once

#include "../../include/status.h"
#include "version_edit.h"
#include "version_set.h"

namespace rocketdb {

struct Options;

class Env;
class Iterator;
class TableCache;
class VersionEdit;

// Build a Table file from the contents of *iter
// The generated file will be named according to mate->number
// On success, the rest of *meta will be filled with metadata about the generated table
// If not data is present in *iter, meta->file_size is zero, and no Table file will be produced
Status BuildTable(const std::string& dbname, Env* env, const Options& options, TableCache* table_cache, 
                  Iterator* iter, FileMetaData* meta);

}