#include "mitto/encoding/text_encoding_registry.h"

#include <functional>
#include <map>

#include "base/check.h"
#include "base/no_destructor.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/encoding/text_codec_cjk.h"
#include "mitto/encoding/text_codec_icu.h"
#include "mitto/encoding/text_codec_latin1.h"
#include "mitto/encoding/text_codec_replacement.h"
#include "mitto/encoding/text_codec_user_defined.h"
#include "mitto/encoding/text_codec_utf16.h"
#include "mitto/encoding/text_codec_utf8.h"

namespace mitto {

namespace {

constexpr size_t kMaxEncodingNameLength = 63;

struct Registry {
  // Each label in ASCII lower case, and its encoding's canonical name.
  std::map<std::string, std::string, std::less<>> names;
  // Each canonical name, and the function that makes its decoder.
  std::map<std::string, NewTextCodecFunction, std::less<>> codecs;
};

// Labels that some of ICU's converters have and that browsers do not take,
// such as "ISO_2022,locale=ja,version=0".
bool IsUndesiredAlias(std::string_view alias) {
  return alias.find(',') != std::string_view::npos || alias == "8859_1";
}

Registry MakeRegistry() {
  Registry registry;
  auto add_name = [&](std::string_view alias, std::string_view canonical_name) {
    DCHECK_LE(alias.size(), kMaxEncodingNameLength);
    if (IsUndesiredAlias(alias)) {
      return;
    }
    // A label keeps the encoding that registered it first, as WTF's
    // HashMap::insert() keeps what the map has.
    registry.names.emplace(base::ToLowerASCII(alias),
                           std::string(canonical_name));
  };
  auto add_codec = [&](std::string_view canonical_name,
                       NewTextCodecFunction function) {
    registry.codecs.emplace(std::string(canonical_name), function);
  };
  TextCodecLatin1::RegisterEncodingNames(add_name);
  TextCodecLatin1::RegisterCodecs(add_codec);
  TextCodecUtf8::RegisterEncodingNames(add_name);
  TextCodecUtf8::RegisterCodecs(add_codec);
  TextCodecUtf16::RegisterEncodingNames(add_name);
  TextCodecUtf16::RegisterCodecs(add_codec);
  TextCodecUserDefined::RegisterEncodingNames(add_name);
  TextCodecUserDefined::RegisterCodecs(add_codec);
  TextCodecReplacement::RegisterEncodingNames(add_name);
  TextCodecReplacement::RegisterCodecs(add_codec);
  TextCodecCjk::RegisterEncodingNames(add_name);
  TextCodecCjk::RegisterCodecs(add_codec);
  TextCodecIcu::RegisterEncodingNames(add_name);
  TextCodecIcu::RegisterCodecs(add_codec);
  return registry;
}

const Registry& GetRegistry() {
  static const base::NoDestructor<Registry> registry(MakeRegistry());
  return *registry;
}

}  // namespace

std::string CanonicalTextEncodingName(std::u16string_view label) {
  if (label.empty() || label.size() > kMaxEncodingNameLength ||
      !base::IsStringASCII(label)) {
    return std::string();
  }
  const Registry& registry = GetRegistry();
  const auto it =
      registry.names.find(base::ToLowerASCII(base::UTF16ToASCII(label)));
  return it == registry.names.end() ? std::string() : it->second;
}

std::unique_ptr<TextCodec> NewTextCodec(std::string_view canonical_name) {
  const Registry& registry = GetRegistry();
  const auto it = registry.codecs.find(canonical_name);
  // Every canonical name has a decoder.
  CHECK(it != registry.codecs.end()) << canonical_name;
  return it->second(canonical_name);
}

}  // namespace mitto
