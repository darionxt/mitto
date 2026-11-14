#include "mitto/encoding/text_encoding_detector.h"

#include <algorithm>
#include <vector>

#include "base/numerics/safe_conversions.h"
#include "mitto/encoding/text_encoding_registry.h"
#include "third_party/ced/src/compact_enc_det/compact_enc_det.h"
#include "url/gurl.h"
#include "url/url_util.h"

namespace mitto {

bool DetectTextEncoding(base::span<const uint8_t> bytes,
                        std::string_view hint_encoding_name,
                        const GURL& hint_url,
                        std::string_view hint_user_language,
                        std::string& detected_encoding) {
  detected_encoding.clear();
  // A local file is the user's own, not a web server's response, so the rules
  // below that keep sites honest about their labels do not hold for it.
  const std::vector<std::string>& local_schemes = url::GetLocalSchemes();
  const bool is_local_resource =
      std::ranges::find(local_schemes, hint_url.scheme()) !=
      local_schemes.end();
  // No language hint, so that the detector gives the same on every device,
  // but for a local file.
  Language language = UNKNOWN_LANGUAGE;
  if (is_local_resource) {
    LanguageFromCode(std::string(hint_user_language).c_str(), &language);
  }

  // Only the top-level domain of the URL, as the runtime-enabled feature
  // TextEncodingDetectorTldHint has it, stable in the Engine Release.
  std::string tld_hint;
  const std::string_view host = hint_url.host();
  if (const size_t dot = host.rfind('.'); dot != std::string_view::npos) {
    tld_hint = std::string(host.substr(dot + 1));
  }

  int consumed_bytes = 0;
  bool is_reliable = false;
  const auto chars = base::as_chars(bytes);
  const Encoding encoding = CompactEncDet::DetectEncoding(
      chars.data(), base::checked_cast<int>(chars.size()), tld_hint.c_str(),
      nullptr, nullptr,
      EncodingNameAliasToEncoding(std::string(hint_encoding_name).c_str()),
      language, CompactEncDet::WEB_CORPUS,
      // With the 7-bit encodings, to detect ISO-2022-JP.
      false, &consumed_bytes, &is_reliable);

  if (encoding != UNKNOWN_ENCODING) {
    detected_encoding =
        CanonicalTextEncodingName(std::string_view(MimeEncodingName(encoding)));
  }
  // UTF-8 falls back to the default encoding, of the locale or the top-level
  // domain: the detector is for sites in legacy encodings.
  return !(encoding == UNKNOWN_ENCODING ||
           (!is_local_resource && encoding == UTF8));
}

}  // namespace mitto
