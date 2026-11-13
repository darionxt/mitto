// The encodings that the Runtime decodes and the labels that name them, as
// Blink's registry has them: each codec registers its labels and its
// decoders, in Blink's order, windows-1252, UTF-8, UTF-16, x-user-defined,
// then replacement, the CJK encodings and those of ICU, and a label that one
// registered first keeps its encoding. A label matches whatever its ASCII
// case.
//
// Blink registers the last three only when a name is not among the first. A
// page sees the same either way, so this registers them all at once.
//
// Upstream: third_party/blink/renderer/platform/wtf/text/text_encoding_registry.h
// Upstream: third_party/blink/renderer/platform/wtf/text/text_encoding_registry.cc
// Upstream: third_party/blink/renderer/platform/wtf/text/text_encoding.cc

#ifndef MITTO_ENCODING_TEXT_ENCODING_REGISTRY_H_
#define MITTO_ENCODING_TEXT_ENCODING_REGISTRY_H_

#include <memory>
#include <string>
#include <string_view>

#include "mitto/encoding/text_codec.h"

namespace mitto {

// The canonical name of the encoding that `label` names, such as
// "windows-1252" for "latin1", or an empty string if no encoding has that
// label. As in Blink, no label is longer than 63 characters or has a
// character beyond ASCII.
std::string CanonicalTextEncodingName(std::u16string_view label);

// A new decoder of the encoding whose canonical name is `canonical_name`, which
// CanonicalTextEncodingName() gave.
std::unique_ptr<TextCodec> NewTextCodec(std::string_view canonical_name);

}  // namespace mitto

#endif  // MITTO_ENCODING_TEXT_ENCODING_REGISTRY_H_
