#pragma once

#include <string>
#include <cstdint>

#include "../../include/slice.h"
#include "../../include/status.h"
#include "../../include/env.h"
#include "../port/port.h"

namespace rocketdb {

enum FileType {
    kLogFile,
    kDBLockFile,
    kTableFile,
    kDescriptorFile,
    kCurrentFile,
    kTempFile,
    kInfoLogFile
};

std::string LogFileName(const std::string& dbname, uint64_t number);

std::string TableFileName(const std::string& dbname, uint64_t number);

// To be compatible with the names of previous verisons
std::string SSTTableFileName(const std::string& dbname, uint64_t number);

std::string TempFileName(const std::string& dbname, uint64_t number);

std::string DescriptorFileName(const std::string& dbname, uint64_t number);

// Single file, don't need number
std::string CurrentFileName(const std::string& dbname);

std::string LockFileName(const std::string& dbname);

std::string InfoLogFileName(const std::string& dbname);

std::string OldInfoLogFileName(const std::string& dbname);

bool ParseFileName(const std::string& filename, uint64_t* number, FileType* type);

Status SetCurrentFile(Env* env, const std::string& dbname, uint64_t descriptor_number);

}
