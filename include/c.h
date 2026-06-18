/*
  C bindings for rocketdb.  May be useful as a stable ABI that can be
  used by programs that keep rocketdb in a shared library, or for
  a JNI api.

  Does not support:
  . getters for the option types
  . custom comparators that implement key shortening
  . custom iter, db, env, cache implementations using just the C bindings

  Some conventions:

  (1) We expose just opaque struct pointers and functions to clients.
  This allows us to change internal representations without having to
  recompile clients.

  (2) For simplicity, there is no equivalent to the Slice type.  Instead,
  the caller has to pass the pointer and length as separate
  arguments.

  (3) Errors are represented by a null-terminated c string.  NULL
  means no error.  All operations that can raise an error are passed
  a "char** errptr" as the last argument.  One of the following must
  be true on entry:
     *errptr == NULL
     *errptr points to a malloc()ed null-terminated error message
       (On Windows, *errptr must have been malloc()-ed by this library.)
  On success, a rocketdb routine leaves *errptr unchanged.
  On failure, rocketdb frees the old value of *errptr and
  set *errptr to a malloc()ed error message.

  (4) Bools have the type uint8_t (0 == false; rest == true)

  (5) All of the pointer arguments must be non-NULL.
*/

#ifndef STORAGE_ROCKETDB_INCLUDE_C_H_
#define STORAGE_ROCKETDB_INCLUDE_C_H_

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "export.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Exported types */

typedef struct rocketdb_t rocketdb_t;
typedef struct rocketdb_cache_t rocketdb_cache_t;
typedef struct rocketdb_comparator_t rocketdb_comparator_t;
typedef struct rocketdb_env_t rocketdb_env_t;
typedef struct rocketdb_filelock_t rocketdb_filelock_t;
typedef struct rocketdb_filterpolicy_t rocketdb_filterpolicy_t;
typedef struct rocketdb_iterator_t rocketdb_iterator_t;
typedef struct rocketdb_logger_t rocketdb_logger_t;
typedef struct rocketdb_options_t rocketdb_options_t;
typedef struct rocketdb_randomfile_t rocketdb_randomfile_t;
typedef struct rocketdb_readoptions_t rocketdb_readoptions_t;
typedef struct rocketdb_seqfile_t rocketdb_seqfile_t;
typedef struct rocketdb_snapshot_t rocketdb_snapshot_t;
typedef struct rocketdb_writablefile_t rocketdb_writablefile_t;
typedef struct rocketdb_writebatch_t rocketdb_writebatch_t;
typedef struct rocketdb_writeoptions_t rocketdb_writeoptions_t;

/* DB operations */

ROCKETDB_EXPORT rocketdb_t* rocketdb_open(const rocketdb_options_t* options,
                                       const char* name, char** errptr);

ROCKETDB_EXPORT void rocketdb_close(rocketdb_t* db);

ROCKETDB_EXPORT void rocketdb_put(rocketdb_t* db,
                                const rocketdb_writeoptions_t* options,
                                const char* key, size_t keylen, const char* val,
                                size_t vallen, char** errptr);

ROCKETDB_EXPORT void rocketdb_delete(rocketdb_t* db,
                                   const rocketdb_writeoptions_t* options,
                                   const char* key, size_t keylen,
                                   char** errptr);

ROCKETDB_EXPORT void rocketdb_write(rocketdb_t* db,
                                  const rocketdb_writeoptions_t* options,
                                  rocketdb_writebatch_t* batch, char** errptr);

/* Returns NULL if not found.  A malloc()ed array otherwise.
   Stores the length of the array in *vallen. */
ROCKETDB_EXPORT char* rocketdb_get(rocketdb_t* db,
                                 const rocketdb_readoptions_t* options,
                                 const char* key, size_t keylen, size_t* vallen,
                                 char** errptr);

ROCKETDB_EXPORT rocketdb_iterator_t* rocketdb_create_iterator(
    rocketdb_t* db, const rocketdb_readoptions_t* options);

ROCKETDB_EXPORT const rocketdb_snapshot_t* rocketdb_create_snapshot(rocketdb_t* db);

ROCKETDB_EXPORT void rocketdb_release_snapshot(
    rocketdb_t* db, const rocketdb_snapshot_t* snapshot);

/* Returns NULL if property name is unknown.
   Else returns a pointer to a malloc()-ed null-terminated value. */
ROCKETDB_EXPORT char* rocketdb_property_value(rocketdb_t* db,
                                            const char* propname);

ROCKETDB_EXPORT void rocketdb_approximate_sizes(
    rocketdb_t* db, int num_ranges, const char* const* range_start_key,
    const size_t* range_start_key_len, const char* const* range_limit_key,
    const size_t* range_limit_key_len, uint64_t* sizes);

ROCKETDB_EXPORT void rocketdb_compact_range(rocketdb_t* db, const char* start_key,
                                          size_t start_key_len,
                                          const char* limit_key,
                                          size_t limit_key_len);

/* Management operations */

ROCKETDB_EXPORT void rocketdb_destroy_db(const rocketdb_options_t* options,
                                       const char* name, char** errptr);

ROCKETDB_EXPORT void rocketdb_repair_db(const rocketdb_options_t* options,
                                      const char* name, char** errptr);

/* Iterator */

ROCKETDB_EXPORT void rocketdb_iter_destroy(rocketdb_iterator_t*);
ROCKETDB_EXPORT uint8_t rocketdb_iter_valid(const rocketdb_iterator_t*);
ROCKETDB_EXPORT void rocketdb_iter_seek_to_first(rocketdb_iterator_t*);
ROCKETDB_EXPORT void rocketdb_iter_seek_to_last(rocketdb_iterator_t*);
ROCKETDB_EXPORT void rocketdb_iter_seek(rocketdb_iterator_t*, const char* k,
                                      size_t klen);
ROCKETDB_EXPORT void rocketdb_iter_next(rocketdb_iterator_t*);
ROCKETDB_EXPORT void rocketdb_iter_prev(rocketdb_iterator_t*);
ROCKETDB_EXPORT const char* rocketdb_iter_key(const rocketdb_iterator_t*,
                                            size_t* klen);
ROCKETDB_EXPORT const char* rocketdb_iter_value(const rocketdb_iterator_t*,
                                              size_t* vlen);
ROCKETDB_EXPORT void rocketdb_iter_get_error(const rocketdb_iterator_t*,
                                           char** errptr);

/* Write batch */

ROCKETDB_EXPORT rocketdb_writebatch_t* rocketdb_writebatch_create(void);
ROCKETDB_EXPORT void rocketdb_writebatch_destroy(rocketdb_writebatch_t*);
ROCKETDB_EXPORT void rocketdb_writebatch_clear(rocketdb_writebatch_t*);
ROCKETDB_EXPORT void rocketdb_writebatch_put(rocketdb_writebatch_t*,
                                           const char* key, size_t klen,
                                           const char* val, size_t vlen);
ROCKETDB_EXPORT void rocketdb_writebatch_delete(rocketdb_writebatch_t*,
                                              const char* key, size_t klen);
ROCKETDB_EXPORT void rocketdb_writebatch_iterate(
    const rocketdb_writebatch_t*, void* state,
    void (*put)(void*, const char* k, size_t klen, const char* v, size_t vlen),
    void (*deleted)(void*, const char* k, size_t klen));
ROCKETDB_EXPORT void rocketdb_writebatch_append(
    rocketdb_writebatch_t* destination, const rocketdb_writebatch_t* source);

/* Options */

ROCKETDB_EXPORT rocketdb_options_t* rocketdb_options_create(void);
ROCKETDB_EXPORT void rocketdb_options_destroy(rocketdb_options_t*);
ROCKETDB_EXPORT void rocketdb_options_set_comparator(rocketdb_options_t*,
                                                   rocketdb_comparator_t*);
ROCKETDB_EXPORT void rocketdb_options_set_filter_policy(rocketdb_options_t*,
                                                      rocketdb_filterpolicy_t*);
ROCKETDB_EXPORT void rocketdb_options_set_create_if_missing(rocketdb_options_t*,
                                                          uint8_t);
ROCKETDB_EXPORT void rocketdb_options_set_error_if_exists(rocketdb_options_t*,
                                                        uint8_t);
ROCKETDB_EXPORT void rocketdb_options_set_paranoid_checks(rocketdb_options_t*,
                                                        uint8_t);
ROCKETDB_EXPORT void rocketdb_options_set_env(rocketdb_options_t*, rocketdb_env_t*);
ROCKETDB_EXPORT void rocketdb_options_set_info_log(rocketdb_options_t*,
                                                 rocketdb_logger_t*);
ROCKETDB_EXPORT void rocketdb_options_set_write_buffer_size(rocketdb_options_t*,
                                                          size_t);
ROCKETDB_EXPORT void rocketdb_options_set_max_open_files(rocketdb_options_t*, int);
ROCKETDB_EXPORT void rocketdb_options_set_cache(rocketdb_options_t*,
                                              rocketdb_cache_t*);
ROCKETDB_EXPORT void rocketdb_options_set_block_size(rocketdb_options_t*, size_t);
ROCKETDB_EXPORT void rocketdb_options_set_block_restart_interval(
    rocketdb_options_t*, int);
ROCKETDB_EXPORT void rocketdb_options_set_max_file_size(rocketdb_options_t*,
                                                      size_t);

enum { rocketdb_no_compression = 0, rocketdb_snappy_compression = 1 };
ROCKETDB_EXPORT void rocketdb_options_set_compression(rocketdb_options_t*, int);

/* Comparator */

ROCKETDB_EXPORT rocketdb_comparator_t* rocketdb_comparator_create(
    void* state, void (*destructor)(void*),
    int (*compare)(void*, const char* a, size_t alen, const char* b,
                   size_t blen),
    const char* (*name)(void*));
ROCKETDB_EXPORT void rocketdb_comparator_destroy(rocketdb_comparator_t*);

/* Filter policy */

ROCKETDB_EXPORT rocketdb_filterpolicy_t* rocketdb_filterpolicy_create(
    void* state, void (*destructor)(void*),
    char* (*create_filter)(void*, const char* const* key_array,
                           const size_t* key_length_array, int num_keys,
                           size_t* filter_length),
    uint8_t (*key_may_match)(void*, const char* key, size_t length,
                             const char* filter, size_t filter_length),
    const char* (*name)(void*));
ROCKETDB_EXPORT void rocketdb_filterpolicy_destroy(rocketdb_filterpolicy_t*);

ROCKETDB_EXPORT rocketdb_filterpolicy_t* rocketdb_filterpolicy_create_bloom(
    int bits_per_key);

/* Read options */

ROCKETDB_EXPORT rocketdb_readoptions_t* rocketdb_readoptions_create(void);
ROCKETDB_EXPORT void rocketdb_readoptions_destroy(rocketdb_readoptions_t*);
ROCKETDB_EXPORT void rocketdb_readoptions_set_verify_checksums(
    rocketdb_readoptions_t*, uint8_t);
ROCKETDB_EXPORT void rocketdb_readoptions_set_fill_cache(rocketdb_readoptions_t*,
                                                       uint8_t);
ROCKETDB_EXPORT void rocketdb_readoptions_set_snapshot(rocketdb_readoptions_t*,
                                                     const rocketdb_snapshot_t*);

/* Write options */

ROCKETDB_EXPORT rocketdb_writeoptions_t* rocketdb_writeoptions_create(void);
ROCKETDB_EXPORT void rocketdb_writeoptions_destroy(rocketdb_writeoptions_t*);
ROCKETDB_EXPORT void rocketdb_writeoptions_set_sync(rocketdb_writeoptions_t*,
                                                  uint8_t);

/* Cache */

ROCKETDB_EXPORT rocketdb_cache_t* rocketdb_cache_create_lru(size_t capacity);
ROCKETDB_EXPORT void rocketdb_cache_destroy(rocketdb_cache_t* cache);

/* Env */

ROCKETDB_EXPORT rocketdb_env_t* rocketdb_create_default_env(void);
ROCKETDB_EXPORT void rocketdb_env_destroy(rocketdb_env_t*);

/* If not NULL, the returned buffer must be released using rocketdb_free(). */
ROCKETDB_EXPORT char* rocketdb_env_get_test_directory(rocketdb_env_t*);

/* Utility */

/* Calls free(ptr).
   REQUIRES: ptr was malloc()-ed and returned by one of the routines
   in this file.  Note that in certain cases (typically on Windows), you
   may need to call this routine instead of free(ptr) to dispose of
   malloc()-ed memory returned by this library. */
ROCKETDB_EXPORT void rocketdb_free(void* ptr);

/* Return the major version number for this release. */
ROCKETDB_EXPORT int rocketdb_major_version(void);

/* Return the minor version number for this release. */
ROCKETDB_EXPORT int rocketdb_minor_version(void);

#ifdef __cplusplus
} /* end extern "C" */
#endif

#endif /* STORAGE_ROCKETDB_INCLUDE_C_H_ */
