#ifndef STORAGE_ROCKETDB_INCLUDE_EXPORT_H_
#define STORAGE_ROCKETDB_INCLUDE_EXPORT_H_

#if !defined(ROCKETDB_EXPORT)

#if defined(ROCKETDB_SHARED_LIBRARY)
#if defined(_WIN32)

#if defined(ROCKETDB_COMPILE_LIBRARY)
#define ROCKETDB_EXPORT __declspec(dllexport)
#else
#define ROCKETDB_EXPORT __declspec(dllimport)
#endif  // defined(ROCKETDB_COMPILE_LIBRARY)

#else  // defined(_WIN32)
#if defined(ROCKETDB_COMPILE_LIBRARY)
#define ROCKETDB_EXPORT __attribute__((visibility("default")))
#else
#define ROCKETDB_EXPORT
#endif
#endif  // defined(_WIN32)

#else  // defined(ROCKETDB_SHARED_LIBRARY)
#define ROCKETDB_EXPORT
#endif

#endif  // !defined(ROCKETDB_EXPORT)

#endif  // STORAGE_ROCKETDB_INCLUDE_EXPORT_H_
