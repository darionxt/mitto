// The formats of CompressionStream and DecompressionStream, as Blink's.
//
// Upstream: third_party/blink/renderer/modules/compression/compression_format.h
// Upstream: third_party/blink/renderer/modules/compression/compression_format.cc

#ifndef MITTO_DOM_COMPRESSION_FORMAT_H_
#define MITTO_DOM_COMPRESSION_FORMAT_H_

#include <string>

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

enum class CompressionFormat {
  kGzip,
  kDeflate,
  kDeflateRaw,
};

// The format that `format` names, or kGzip after throwing a TypeError for one
// that Chrome does not support.
CompressionFormat LookupCompressionFormat(
    const std::u16string& format,
    bindings::ExceptionState& exception_state);

}  // namespace mitto

#endif  // MITTO_DOM_COMPRESSION_FORMAT_H_
