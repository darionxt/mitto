#ifndef MITTO_DIRECTORY_LOCK_H_
#define MITTO_DIRECTORY_LOCK_H_

#include "base/files/file_path.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_WIN)
#include "base/win/scoped_handle.h"
#else
#include "base/files/scoped_file.h"
#endif

namespace mitto {

// A lock on a directory. No other lock can take the same directory while this
// one holds it: not from another process, not from this one, and not through
// another name for the directory, such as a link. It goes away with this
// object, or with the process, however that ends.
class DirectoryLock {
 public:
  // What Take() did.
  enum class Result {
    kTaken,
    // Another lock holds the directory.
    kHeld,
    // The directory cannot be opened.
    kFailed,
  };

  // A lock that holds nothing yet.
  DirectoryLock();

  DirectoryLock(DirectoryLock&&);
  DirectoryLock& operator=(DirectoryLock&&);

  ~DirectoryLock();

  // Takes `directory`, which must exist. Called once at most.
  Result Take(const base::FilePath& directory);

 private:
#if BUILDFLAG(IS_WIN)
  // A mutex whose name says which directory it is for.
  base::win::ScopedHandle mutex_;
#else
  // The directory, open, with flock().
  base::ScopedFD directory_;
#endif
};

}  // namespace mitto

#endif  // MITTO_DIRECTORY_LOCK_H_
