// Why a read of a Blob failed, and the DOMException that tells a page, as
// Blink's FileErrorCode and file_error::CreateDOMException() have them. A
// Page holds every blob's bytes itself, so a read can only be aborted, or
// fail for a size it cannot hold.
//
// Upstream: third_party/blink/renderer/core/fileapi/file_error.h
// Upstream: third_party/blink/renderer/core/fileapi/file_error.cc

#ifndef MITTO_DOM_FILE_ERROR_H_
#define MITTO_DOM_FILE_ERROR_H_

namespace mitto {

class DOMException;

enum class FileErrorCode {
  kOK = 0,
  kNotFoundErr = 1,
  kSecurityErr = 2,
  kAbortErr = 3,
  kNotReadableErr = 4,
};

// A DOMException of the error's name, with Chrome's message for it.
DOMException* CreateFileErrorDOMException(FileErrorCode code);

}  // namespace mitto

#endif  // MITTO_DOM_FILE_ERROR_H_
