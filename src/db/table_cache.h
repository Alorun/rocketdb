#pragma once

#include <cstdint>
#include <string>

#include "dbformat.h"
#include "../../include/cache.h"
#include "../../include/table.h"
#include "../port/port.h"

namespace rocketdb {

class Env;

class TableCache {
    public:
        TableCache(const std::string& dbname, const Options& options, int entries);

        TableCache(const TableCache&) = delete;
        TableCache& operator=(const TableCache&) = delete;

        ~TableCache();

        // Return an iterator for the specified file number 
        // The corresponding file length must be exactly file_size bytes
        Iterator* NewIterator(const ReadOptions& options, uint64_t file_number, uint64_t file_size, Table** tableptr = nullptr);

        // If a seek to internal key "k" in 'specified' file finds an entry
        Status Get(const ReadOptions& options, uint64_t file_number, uint64_t file_size, const Slice& k, void* arg,
                   void (*handle_result)(void*, const Slice&, const Slice&));

        // Evict any entry for the specified file number
        void Evict(uint64_t file_number);

    private:
        Status FindTable(uint64_t file_number, uint64_t file_size, Cache::Handle**);

        Env* const env_;
        const std::string dbname_;
        const Options& options_;
        Cache* cache_;  // Cache TableAndFile
};

}