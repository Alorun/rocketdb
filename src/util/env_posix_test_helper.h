#pragma once

namespace rocketdb {

class EnvPosixTest;

// A helper for the POSIX Env to facilitate testing.
class EnvPosixTestHelper {
 private:
  friend class EnvPosixTest;

  // Set the maximum number of read-only files that will be opened.
  // Must be called before creating an Env.
  static void SetReadOnlyFDLimit(int limit);

  // Set the maximum number of read-only files that will be mapped via mmap.
  // Must be called before creating an Env.
  static void SetReadOnlyMMapLimit(int limit);
};

}  // namespace leveldb

