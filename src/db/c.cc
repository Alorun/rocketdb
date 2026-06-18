#include "c.h"

#include <string.h>

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "cache.h"
#include "comparator.h"
#include "db.h"
#include "env.h"
#include "filter_policy.h"
#include "iterator.h"
#include "options.h"
#include "status.h"
#include "write_batch.h"

using rocketdb::Cache;
using rocketdb::Comparator;
using rocketdb::CompressionType;
using rocketdb::DB;
using rocketdb::Env;
using rocketdb::FileLock;
using rocketdb::FilterPolicy;
using rocketdb::Iterator;
using rocketdb::kMajorVersion;
using rocketdb::kMinorVersion;
using rocketdb::Logger;
using rocketdb::NewBloomFilterPolicy;
using rocketdb::NewLRUCache;
using rocketdb::Options;
using rocketdb::RandomAccessFile;
using rocketdb::Range;
using rocketdb::ReadOptions;
using rocketdb::SequentialFile;
using rocketdb::Slice;
using rocketdb::Snapshot;
using rocketdb::Status;
using rocketdb::WritableFile;
using rocketdb::WriteBatch;
using rocketdb::WriteOptions;

extern "C" {

struct rocketdb_t {
  DB* rep;
};
struct rocketdb_iterator_t {
  Iterator* rep;
};
struct rocketdb_writebatch_t {
  WriteBatch rep;
};
struct rocketdb_snapshot_t {
  const Snapshot* rep;
};
struct rocketdb_readoptions_t {
  ReadOptions rep;
};
struct rocketdb_writeoptions_t {
  WriteOptions rep;
};
struct rocketdb_options_t {
  Options rep;
};
struct rocketdb_cache_t {
  Cache* rep;
};
struct rocketdb_seqfile_t {
  SequentialFile* rep;
};
struct rocketdb_randomfile_t {
  RandomAccessFile* rep;
};
struct rocketdb_writablefile_t {
  WritableFile* rep;
};
struct rocketdb_logger_t {
  Logger* rep;
};
struct rocketdb_filelock_t {
  FileLock* rep;
};

struct rocketdb_comparator_t : public Comparator {
  ~rocketdb_comparator_t() override { (*destructor_)(state_); }

  int Compare(const Slice& a, const Slice& b) const override {
    return (*compare_)(state_, a.data(), a.size(), b.data(), b.size());
  }

  const char* Name() const override { return (*name_)(state_); }

  // No-ops since the C binding does not support key shortening methods.
  void FindShortestSeparator(std::string*, const Slice&) const override {}
  void FindShortSuccessor(std::string* key) const override {}

  void* state_;
  void (*destructor_)(void*);
  int (*compare_)(void*, const char* a, size_t alen, const char* b,
                  size_t blen);
  const char* (*name_)(void*);
};

struct rocketdb_filterpolicy_t : public FilterPolicy {
  ~rocketdb_filterpolicy_t() override { (*destructor_)(state_); }

  const char* Name() const override { return (*name_)(state_); }

  void CreateFilter(const Slice* keys, int n, std::string* dst) const override {
    std::vector<const char*> key_pointers(n);
    std::vector<size_t> key_sizes(n);
    for (int i = 0; i < n; i++) {
      key_pointers[i] = keys[i].data();
      key_sizes[i] = keys[i].size();
    }
    size_t len;
    char* filter = (*create_)(state_, &key_pointers[0], &key_sizes[0], n, &len);
    dst->append(filter, len);
    std::free(filter);
  }

  bool KeyMayMatch(const Slice& key, const Slice& filter) const override {
    return (*key_match_)(state_, key.data(), key.size(), filter.data(),
                         filter.size());
  }

  void* state_;
  void (*destructor_)(void*);
  const char* (*name_)(void*);
  char* (*create_)(void*, const char* const* key_array,
                   const size_t* key_length_array, int num_keys,
                   size_t* filter_length);
  uint8_t (*key_match_)(void*, const char* key, size_t length,
                        const char* filter, size_t filter_length);
};

struct rocketdb_env_t {
  Env* rep;
  bool is_default;
};

static bool SaveError(char** errptr, const Status& s) {
  assert(errptr != nullptr);
  if (s.ok()) {
    return false;
  } else if (*errptr == nullptr) {
    *errptr = strdup(s.ToString().c_str());
  } else {
    // TODO(sanjay): Merge with existing error?
    std::free(*errptr);
    *errptr = strdup(s.ToString().c_str());
  }
  return true;
}

static char* CopyString(const std::string& str) {
  char* result =
      reinterpret_cast<char*>(std::malloc(sizeof(char) * str.size()));
  std::memcpy(result, str.data(), sizeof(char) * str.size());
  return result;
}

rocketdb_t* rocketdb_open(const rocketdb_options_t* options, const char* name,
                        char** errptr) {
  DB* db;
  if (SaveError(errptr, DB::Open(options->rep, std::string(name), &db))) {
    return nullptr;
  }
  rocketdb_t* result = new rocketdb_t;
  result->rep = db;
  return result;
}

void rocketdb_close(rocketdb_t* db) {
  delete db->rep;
  delete db;
}

void rocketdb_put(rocketdb_t* db, const rocketdb_writeoptions_t* options,
                 const char* key, size_t keylen, const char* val, size_t vallen,
                 char** errptr) {
  SaveError(errptr,
            db->rep->Put(options->rep, Slice(key, keylen), Slice(val, vallen)));
}

void rocketdb_delete(rocketdb_t* db, const rocketdb_writeoptions_t* options,
                    const char* key, size_t keylen, char** errptr) {
  SaveError(errptr, db->rep->Delete(options->rep, Slice(key, keylen)));
}

void rocketdb_write(rocketdb_t* db, const rocketdb_writeoptions_t* options,
                   rocketdb_writebatch_t* batch, char** errptr) {
  SaveError(errptr, db->rep->Write(options->rep, &batch->rep));
}

char* rocketdb_get(rocketdb_t* db, const rocketdb_readoptions_t* options,
                  const char* key, size_t keylen, size_t* vallen,
                  char** errptr) {
  char* result = nullptr;
  std::string tmp;
  Status s = db->rep->Get(options->rep, Slice(key, keylen), &tmp);
  if (s.ok()) {
    *vallen = tmp.size();
    result = CopyString(tmp);
  } else {
    *vallen = 0;
    if (!s.IsNotFound()) {
      SaveError(errptr, s);
    }
  }
  return result;
}

rocketdb_iterator_t* rocketdb_create_iterator(
    rocketdb_t* db, const rocketdb_readoptions_t* options) {
  rocketdb_iterator_t* result = new rocketdb_iterator_t;
  result->rep = db->rep->NewIterator(options->rep);
  return result;
}

const rocketdb_snapshot_t* rocketdb_create_snapshot(rocketdb_t* db) {
  rocketdb_snapshot_t* result = new rocketdb_snapshot_t;
  result->rep = db->rep->GetSnapshot();
  return result;
}

void rocketdb_release_snapshot(rocketdb_t* db,
                              const rocketdb_snapshot_t* snapshot) {
  db->rep->ReleaseSnapshot(snapshot->rep);
  delete snapshot;
}

char* rocketdb_property_value(rocketdb_t* db, const char* propname) {
  std::string tmp;
  if (db->rep->GetProperty(Slice(propname), &tmp)) {
    // We use strdup() since we expect human readable output.
    return strdup(tmp.c_str());
  } else {
    return nullptr;
  }
}

void rocketdb_approximate_sizes(rocketdb_t* db, int num_ranges,
                               const char* const* range_start_key,
                               const size_t* range_start_key_len,
                               const char* const* range_limit_key,
                               const size_t* range_limit_key_len,
                               uint64_t* sizes) {
  Range* ranges = new Range[num_ranges];
  for (int i = 0; i < num_ranges; i++) {
    ranges[i].start = Slice(range_start_key[i], range_start_key_len[i]);
    ranges[i].limit = Slice(range_limit_key[i], range_limit_key_len[i]);
  }
  db->rep->GetApproximateSizes(ranges, num_ranges, sizes);
  delete[] ranges;
}

void rocketdb_compact_range(rocketdb_t* db, const char* start_key,
                           size_t start_key_len, const char* limit_key,
                           size_t limit_key_len) {
  Slice a, b;
  db->rep->CompactRange(
      // Pass null Slice if corresponding "const char*" is null
      (start_key ? (a = Slice(start_key, start_key_len), &a) : nullptr),
      (limit_key ? (b = Slice(limit_key, limit_key_len), &b) : nullptr));
}

void rocketdb_destroy_db(const rocketdb_options_t* options, const char* name,
                        char** errptr) {
  SaveError(errptr, DestroyDB(name, options->rep));
}

void rocketdb_repair_db(const rocketdb_options_t* options, const char* name,
                       char** errptr) {
  SaveError(errptr, RepairDB(name, options->rep));
}

void rocketdb_iter_destroy(rocketdb_iterator_t* iter) {
  delete iter->rep;
  delete iter;
}

uint8_t rocketdb_iter_valid(const rocketdb_iterator_t* iter) {
  return iter->rep->Valid();
}

void rocketdb_iter_seek_to_first(rocketdb_iterator_t* iter) {
  iter->rep->SeekToFirst();
}

void rocketdb_iter_seek_to_last(rocketdb_iterator_t* iter) {
  iter->rep->SeekToLast();
}

void rocketdb_iter_seek(rocketdb_iterator_t* iter, const char* k, size_t klen) {
  iter->rep->Seek(Slice(k, klen));
}

void rocketdb_iter_next(rocketdb_iterator_t* iter) { iter->rep->Next(); }

void rocketdb_iter_prev(rocketdb_iterator_t* iter) { iter->rep->Prev(); }

const char* rocketdb_iter_key(const rocketdb_iterator_t* iter, size_t* klen) {
  Slice s = iter->rep->key();
  *klen = s.size();
  return s.data();
}

const char* rocketdb_iter_value(const rocketdb_iterator_t* iter, size_t* vlen) {
  Slice s = iter->rep->value();
  *vlen = s.size();
  return s.data();
}

void rocketdb_iter_get_error(const rocketdb_iterator_t* iter, char** errptr) {
  SaveError(errptr, iter->rep->status());
}

rocketdb_writebatch_t* rocketdb_writebatch_create() {
  return new rocketdb_writebatch_t;
}

void rocketdb_writebatch_destroy(rocketdb_writebatch_t* b) { delete b; }

void rocketdb_writebatch_clear(rocketdb_writebatch_t* b) { b->rep.Clear(); }

void rocketdb_writebatch_put(rocketdb_writebatch_t* b, const char* key,
                            size_t klen, const char* val, size_t vlen) {
  b->rep.Put(Slice(key, klen), Slice(val, vlen));
}

void rocketdb_writebatch_delete(rocketdb_writebatch_t* b, const char* key,
                               size_t klen) {
  b->rep.Delete(Slice(key, klen));
}

void rocketdb_writebatch_iterate(const rocketdb_writebatch_t* b, void* state,
                                void (*put)(void*, const char* k, size_t klen,
                                            const char* v, size_t vlen),
                                void (*deleted)(void*, const char* k,
                                                size_t klen)) {
  class H : public WriteBatch::Handler {
   public:
    void* state_;
    void (*put_)(void*, const char* k, size_t klen, const char* v, size_t vlen);
    void (*deleted_)(void*, const char* k, size_t klen);
    void Put(const Slice& key, const Slice& value) override {
      (*put_)(state_, key.data(), key.size(), value.data(), value.size());
    }
    void Delete(const Slice& key) override {
      (*deleted_)(state_, key.data(), key.size());
    }
  };
  H handler;
  handler.state_ = state;
  handler.put_ = put;
  handler.deleted_ = deleted;
  b->rep.Iterate(&handler);
}

void rocketdb_writebatch_append(rocketdb_writebatch_t* destination,
                               const rocketdb_writebatch_t* source) {
  destination->rep.Append(source->rep);
}

rocketdb_options_t* rocketdb_options_create() { return new rocketdb_options_t; }

void rocketdb_options_destroy(rocketdb_options_t* options) { delete options; }

void rocketdb_options_set_comparator(rocketdb_options_t* opt,
                                    rocketdb_comparator_t* cmp) {
  opt->rep.comparator = cmp;
}

void rocketdb_options_set_filter_policy(rocketdb_options_t* opt,
                                       rocketdb_filterpolicy_t* policy) {
  opt->rep.filter_policy = policy;
}

void rocketdb_options_set_create_if_missing(rocketdb_options_t* opt, uint8_t v) {
  opt->rep.create_if_missing = v;
}

void rocketdb_options_set_error_if_exists(rocketdb_options_t* opt, uint8_t v) {
  opt->rep.error_if_exists = v;
}

void rocketdb_options_set_paranoid_checks(rocketdb_options_t* opt, uint8_t v) {
  opt->rep.paranoid_checks = v;
}

void rocketdb_options_set_env(rocketdb_options_t* opt, rocketdb_env_t* env) {
  opt->rep.env = (env ? env->rep : nullptr);
}

void rocketdb_options_set_info_log(rocketdb_options_t* opt, rocketdb_logger_t* l) {
  opt->rep.info_log = (l ? l->rep : nullptr);
}

void rocketdb_options_set_write_buffer_size(rocketdb_options_t* opt, size_t s) {
  opt->rep.write_buffer_size = s;
}

void rocketdb_options_set_max_open_files(rocketdb_options_t* opt, int n) {
  opt->rep.max_open_files = n;
}

void rocketdb_options_set_cache(rocketdb_options_t* opt, rocketdb_cache_t* c) {
  opt->rep.block_cache = c->rep;
}

void rocketdb_options_set_block_size(rocketdb_options_t* opt, size_t s) {
  opt->rep.block_size = s;
}

void rocketdb_options_set_block_restart_interval(rocketdb_options_t* opt, int n) {
  opt->rep.block_restart_interval = n;
}

void rocketdb_options_set_max_file_size(rocketdb_options_t* opt, size_t s) {
  opt->rep.max_file_size = s;
}

void rocketdb_options_set_compression(rocketdb_options_t* opt, int t) {
  opt->rep.compression = static_cast<CompressionType>(t);
}

rocketdb_comparator_t* rocketdb_comparator_create(
    void* state, void (*destructor)(void*),
    int (*compare)(void*, const char* a, size_t alen, const char* b,
                   size_t blen),
    const char* (*name)(void*)) {
  rocketdb_comparator_t* result = new rocketdb_comparator_t;
  result->state_ = state;
  result->destructor_ = destructor;
  result->compare_ = compare;
  result->name_ = name;
  return result;
}

void rocketdb_comparator_destroy(rocketdb_comparator_t* cmp) { delete cmp; }

rocketdb_filterpolicy_t* rocketdb_filterpolicy_create(
    void* state, void (*destructor)(void*),
    char* (*create_filter)(void*, const char* const* key_array,
                           const size_t* key_length_array, int num_keys,
                           size_t* filter_length),
    uint8_t (*key_may_match)(void*, const char* key, size_t length,
                             const char* filter, size_t filter_length),
    const char* (*name)(void*)) {
  rocketdb_filterpolicy_t* result = new rocketdb_filterpolicy_t;
  result->state_ = state;
  result->destructor_ = destructor;
  result->create_ = create_filter;
  result->key_match_ = key_may_match;
  result->name_ = name;
  return result;
}

void rocketdb_filterpolicy_destroy(rocketdb_filterpolicy_t* filter) {
  delete filter;
}

rocketdb_filterpolicy_t* rocketdb_filterpolicy_create_bloom(int bits_per_key) {
  // Make a rocketdb_filterpolicy_t, but override all of its methods so
  // they delegate to a NewBloomFilterPolicy() instead of user
  // supplied C functions.
  struct Wrapper : public rocketdb_filterpolicy_t {
    static void DoNothing(void*) {}

    ~Wrapper() { delete rep_; }
    const char* Name() const { return rep_->Name(); }
    void CreateFilter(const Slice* keys, int n, std::string* dst) const {
      return rep_->CreateFilter(keys, n, dst);
    }
    bool KeyMayMatch(const Slice& key, const Slice& filter) const {
      return rep_->KeyMayMatch(key, filter);
    }

    const FilterPolicy* rep_;
  };
  Wrapper* wrapper = new Wrapper;
  wrapper->rep_ = NewBloomFilterPolicy(bits_per_key);
  wrapper->state_ = nullptr;
  wrapper->destructor_ = &Wrapper::DoNothing;
  return wrapper;
}

rocketdb_readoptions_t* rocketdb_readoptions_create() {
  return new rocketdb_readoptions_t;
}

void rocketdb_readoptions_destroy(rocketdb_readoptions_t* opt) { delete opt; }

void rocketdb_readoptions_set_verify_checksums(rocketdb_readoptions_t* opt,
                                              uint8_t v) {
  opt->rep.verify_checksums = v;
}

void rocketdb_readoptions_set_fill_cache(rocketdb_readoptions_t* opt, uint8_t v) {
  opt->rep.fill_cache = v;
}

void rocketdb_readoptions_set_snapshot(rocketdb_readoptions_t* opt,
                                      const rocketdb_snapshot_t* snap) {
  opt->rep.snapshot = (snap ? snap->rep : nullptr);
}

rocketdb_writeoptions_t* rocketdb_writeoptions_create() {
  return new rocketdb_writeoptions_t;
}

void rocketdb_writeoptions_destroy(rocketdb_writeoptions_t* opt) { delete opt; }

void rocketdb_writeoptions_set_sync(rocketdb_writeoptions_t* opt, uint8_t v) {
  opt->rep.sync = v;
}

rocketdb_cache_t* rocketdb_cache_create_lru(size_t capacity) {
  rocketdb_cache_t* c = new rocketdb_cache_t;
  c->rep = NewLRUCache(capacity);
  return c;
}

void rocketdb_cache_destroy(rocketdb_cache_t* cache) {
  delete cache->rep;
  delete cache;
}

rocketdb_env_t* rocketdb_create_default_env() {
  rocketdb_env_t* result = new rocketdb_env_t;
  result->rep = Env::Default();
  result->is_default = true;
  return result;
}

void rocketdb_env_destroy(rocketdb_env_t* env) {
  if (!env->is_default) delete env->rep;
  delete env;
}

char* rocketdb_env_get_test_directory(rocketdb_env_t* env) {
  std::string result;
  if (!env->rep->GetTestDirectory(&result).ok()) {
    return nullptr;
  }

  char* buffer = static_cast<char*>(std::malloc(result.size() + 1));
  std::memcpy(buffer, result.data(), result.size());
  buffer[result.size()] = '\0';
  return buffer;
}

void rocketdb_free(void* ptr) { std::free(ptr); }

int rocketdb_major_version() { return kMajorVersion; }

int rocketdb_minor_version() { return kMinorVersion; }

}  // end extern "C"
