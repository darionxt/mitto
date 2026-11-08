#include "mitto/directory_lock.h"

#include <windows.h>

#include <string>
#include <utility>

#include "base/check.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"

namespace mitto {

DirectoryLock::DirectoryLock() = default;
DirectoryLock::DirectoryLock(DirectoryLock&&) = default;
DirectoryLock& DirectoryLock::operator=(DirectoryLock&&) = default;
DirectoryLock::~DirectoryLock() = default;

// Windows locks files and not directories. A directory held open without
// sharing would refuse a second lock, but also anything that moves a file into
// the directory, as //net does with a cache that it cannot read. So the lock is
// a mutex named after the directory's volume and its ID on that volume, which
// are the same by any path.
DirectoryLock::Result DirectoryLock::Take(const base::FilePath& directory) {
  DCHECK(!mutex_.is_valid());
  // Opened only to read its attributes, the directory stays open to anyone.
  const base::win::ScopedHandle handle(
      ::CreateFileW(directory.value().c_str(), FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    /*lpSecurityAttributes=*/nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS, /*hTemplateFile=*/nullptr));
  FILE_ID_INFO id;
  if (!handle.is_valid() || !::GetFileInformationByHandleEx(
                                handle.get(), FileIdInfo, &id, sizeof(id))) {
    return Result::kFailed;
  }
  // In the global namespace, so that a process of another logon session sees it
  // too, as flock() does elsewhere.
  const std::string name =
      base::StrCat({"Global\\mitto-directory-lock-",
                    base::NumberToString(id.VolumeSerialNumber), "-",
                    base::HexEncode(id.FileId.Identifier)});
  // A mutex goes away with the last handle to it, so with the process.
  base::win::ScopedHandle mutex(
      ::CreateMutexW(/*lpMutexAttributes=*/nullptr,
                     /*bInitialOwner=*/FALSE, base::ASCIIToWide(name).c_str()));
  if (!mutex.is_valid()) {
    // Another user's lock exists, and this one may not open it.
    return ::GetLastError() == ERROR_ACCESS_DENIED ? Result::kHeld
                                                   : Result::kFailed;
  }
  if (::GetLastError() == ERROR_ALREADY_EXISTS) {
    return Result::kHeld;
  }
  mutex_ = std::move(mutex);
  return Result::kTaken;
}

}  // namespace mitto
