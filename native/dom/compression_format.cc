#include "mitto/dom/compression_format.h"

#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"

namespace mitto {

CompressionFormat LookupCompressionFormat(
    const std::u16string& format,
    bindings::ExceptionState& exception_state) {
  if (format == u"gzip") {
    return CompressionFormat::kGzip;
  }
  if (format == u"deflate") {
    return CompressionFormat::kDeflate;
  }
  if (format == u"deflate-raw") {
    return CompressionFormat::kDeflateRaw;
  }
  exception_state.ThrowTypeError(base::StrCat(
      {"Unsupported compression format: '", base::UTF16ToUTF8(format), "'"}));
  return CompressionFormat::kGzip;
}

}  // namespace mitto
