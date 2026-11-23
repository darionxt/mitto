#include "mitto/dom/file_error.h"

#include "base/notreached.h"
#include "mitto/dom/dom_exception.h"

namespace mitto {

DOMException* CreateFileErrorDOMException(FileErrorCode code) {
  const char16_t* name = nullptr;
  const char16_t* message = nullptr;
  switch (code) {
    case FileErrorCode::kNotFoundErr:
      name = u"NotFoundError";
      message =
          u"A requested file or directory could not be found at the time an "
          "operation was processed.";
      break;
    case FileErrorCode::kSecurityErr:
      name = u"SecurityError";
      message =
          u"It was determined that certain files are unsafe for access within "
          u"a "
          "Web application, or that too many calls are being made on file "
          "resources.";
      break;
    case FileErrorCode::kAbortErr:
      name = u"AbortError";
      message =
          u"An ongoing operation was aborted, typically with a call to "
          u"abort().";
      break;
    case FileErrorCode::kNotReadableErr:
      name = u"NotReadableError";
      message =
          u"The requested file could not be read, typically due to permission "
          "problems that have occurred after a reference to a file was "
          "acquired.";
      break;
    case FileErrorCode::kOK:
      NOTREACHED();
  }
  return DOMException::Create(message, name);
}

}  // namespace mitto
