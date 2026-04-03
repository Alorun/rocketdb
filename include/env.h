#pragma once

#include <cstddef>
#include <string>
#include <cstdarg>
#include <vector>
#include <cstdint>

#include "options.h"
#include "slice.h"
#include "status.h"

namespace rocketdb {

class SequentialFile;
class Slice;
class WritableFile;
class RandomAccessFile;
class FileLock;

class Env {
    public:
        Env();

        Env(const Env&) = delete;
        Env& operator=(const Env&) = delete;

        virtual ~Env();

        static Env* Default();

        virtual Status NewSequentialFile(const std::string& fname, SequentialFile** result) = 0;

        virtual Status NewRandomAccessFile(const std::string& fname, RandomAccessFile** result) = 0;

        virtual Status NewWriteFile(const std::string& fname, WritableFile** result) = 0;

        virtual Status NewAppendableFile(const std::string& fname, WritableFile** result) = 0;

        virtual bool FileExists(const std::string& fname) = 0;

        virtual Status GetChildren(const std::string& dir, std::vector<std::string>* result) = 0;

        virtual Status RemoveFile(const std::string fname);

        virtual Status CreateDir(const std::string& dirname);

        virtual Status RemoveDir(const std::string& dirname);

        virtual Status DeleteDir(const std::string& dirname);

        virtual Status GetFileSize(const std::string& fname, uint64_t* file_size) = 0;

        virtual Status RenameFile(const std::string& src, const std::string& target) = 0;

        virtual Status LockFile(const std::string& fname, FileLock** lock) = 0;

        virtual Status UnlockFile(const FileLock* lock) = 0;

        virtual void Schedule(void (*function)(void* arg), void* arg) = 0;

        virtual void StartThread(void (*function)(void* arg), void* arg) = 0;

        virtual Status GetTestDiredtory(std::string* path) = 0;

        virtual Status NewLogger(const std::string& fname, Logger** result) = 0;

        virtual uint64_t NowMicros() = 0;

        virtual void SleepForMicroseconds(int micros) = 0;
};  


class SequentialFile {
    public:
        SequentialFile() = default;

        SequentialFile(const SequentialFile&) = delete;
        SequentialFile& operator=(const SequentialFile&) = delete;

        virtual ~SequentialFile();

        virtual Status Read(size_t n, Slice* result, char* scratch) = 0;

        virtual Status Skip(uint64_t n) = 0;
};

class WritableFile {
    public:
        WritableFile() = default;

        WritableFile(const WritableFile&) = delete;
        WritableFile& operator=(const WritableFile&) = delete;

        virtual ~WritableFile();

        virtual Status Append(const Slice& data) = 0;
        virtual Status Close() = 0;
        virtual Status Flush() = 0;
        virtual Status Sync() = 0;
};

class RandomAccessFile {
    public:
        RandomAccessFile() = default;

        RandomAccessFile(const RandomAccessFile&) = delete;
        RandomAccessFile& operator=(const RandomAccessFile&) = delete;

        virtual ~RandomAccessFile();

        virtual Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const = 0;
};

class Logger {
    public:
        Logger() = default;

        Logger(const Logger&) = delete;
        Logger& operator=(const Logger&) = delete;

        virtual ~Logger();

        virtual void Logv(const char* format, std::va_list ap) = 0;
};

void Log(Logger* info_log, const char* format, ...)
#if defined(__GUNC__) || defined (__clang__)
    __attribute__((__format__(__printf__, 2, 3)))
#endif
    ;

Status WriteStringToFile(Env* env, const Slice& data, const std::string& fname);

Status ReadFileToString(Env* env, const std::string& fname, const std::string* data);


}