#include "mitto/dom/subresource_integrity.h"

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

#include "base/base64.h"
#include "base/containers/span.h"
#include "base/strings/strcat.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "crypto/hash.h"

namespace mitto {
namespace {

// IsIntegrityCharacter(): base64 or base64url, loosely.
bool IsIntegrityCharacter(char c) {
  return base::IsAsciiAlphaNumeric(c) || c == '_' || c == '-' || c == '+' ||
         c == '/' || c == '=';
}

// IsValueCharacter(): VCHAR.
bool IsValueCharacter(char c) {
  return c >= 0x21 && c <= 0x7e;
}

// IntegrityAlgorithmsForConsole(), as Chrome writes it.
constexpr std::string_view kAlgorithmsForConsole =
    "'sha256', 'sha384', 'sha512', or 'ed21159'";

struct AlgorithmPrefix {
  std::string_view prefix;
  IntegrityAlgorithm algorithm;
};

constexpr std::array<AlgorithmPrefix, 7> kPrefixes = {{
    {"sha256", IntegrityAlgorithm::kSha256},
    {"sha-256", IntegrityAlgorithm::kSha256},
    {"sha384", IntegrityAlgorithm::kSha384},
    {"sha-384", IntegrityAlgorithm::kSha384},
    {"sha512", IntegrityAlgorithm::kSha512},
    {"sha-512", IntegrityAlgorithm::kSha512},
    {"ed25519", IntegrityAlgorithm::kEd25519},
}};

std::string_view NameOf(IntegrityAlgorithm algorithm) {
  switch (algorithm) {
    case IntegrityAlgorithm::kSha256:
      return "SHA-256";
    case IntegrityAlgorithm::kSha384:
      return "SHA-384";
    case IntegrityAlgorithm::kSha512:
      return "SHA-512";
    case IntegrityAlgorithm::kEd25519:
      return "Ed25519";
  }
}

crypto::hash::HashKind HashKindOf(IntegrityAlgorithm algorithm) {
  switch (algorithm) {
    case IntegrityAlgorithm::kSha256:
      return crypto::hash::kSha256;
    case IntegrityAlgorithm::kSha384:
      return crypto::hash::kSha384;
    case IntegrityAlgorithm::kSha512:
    case IntegrityAlgorithm::kEd25519:
      return crypto::hash::kSha512;
  }
}

// Whether `headers` has a line named `name`.
bool HasHeader(std::string_view headers, std::string_view name) {
  for (const std::string_view line : base::SplitStringPiece(
           headers, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    const size_t colon = line.find(':');
    if (colon != std::string_view::npos &&
        base::EqualsCaseInsensitiveASCII(
            base::TrimWhitespaceASCII(line.substr(0, colon), base::TRIM_ALL),
            name)) {
      return true;
    }
  }
  return false;
}

// CheckHashesImpl(): the strongest algorithm's digests, one of which has to
// match.
bool CheckHashes(const std::vector<IntegrityMetadata>& hashes,
                 std::string_view body,
                 std::string_view elided_url,
                 std::vector<std::string>& messages) {
  if (hashes.empty()) {
    return true;
  }
  const IntegrityAlgorithm strongest =
      std::max_element(
          hashes.begin(), hashes.end(),
          [](const IntegrityMetadata& a, const IntegrityMetadata& b) {
            return a.algorithm < b.algorithm;
          })
          ->algorithm;
  const std::vector<uint8_t> actual =
      crypto::hash::Hash(HashKindOf(strongest), body);
  for (const IntegrityMetadata& metadata : hashes) {
    if (metadata.algorithm == strongest && metadata.value == actual) {
      return true;
    }
  }
  // Only the console sees the digest.
  messages.push_back(base::StrCat(
      {"Failed to find a valid digest in the 'integrity' attribute for "
       "resource '",
       elided_url, "' with computed ", NameOf(strongest), " integrity '",
       base::Base64Encode(actual), "'. The resource has been blocked."}));
  return false;
}

// CheckSignaturesImpl() for a response that is not signed. The signatures
// of signed responses are not checked: a Known Gap.
bool CheckSignatures(const std::vector<IntegrityMetadata>& public_keys,
                     std::string_view elided_url,
                     std::string_view headers,
                     std::vector<std::string>& messages) {
  if (public_keys.empty()) {
    return true;
  }
  if (!HasHeader(headers, "signature") ||
      !HasHeader(headers, "signature-input")) {
    messages.push_back(base::StrCat(
        {"Subresource Integrity: The resource at `", elided_url,
         "` was not signed, but integrity checks are required. The resource "
         "has been blocked."}));
    return false;
  }
  messages.push_back(
      base::StrCat({"Subresource Integrity: The resource at `", elided_url,
                    "` was not signed in a way that matched "
                    "the required integrity checks."}));
  return false;
}

}  // namespace

IntegrityMetadataSet::IntegrityMetadataSet() = default;
IntegrityMetadataSet::IntegrityMetadataSet(const IntegrityMetadataSet&) =
    default;
IntegrityMetadataSet& IntegrityMetadataSet::operator=(
    const IntegrityMetadataSet&) = default;
IntegrityMetadataSet::~IntegrityMetadataSet() = default;

IntegrityMetadataSet ParseIntegrityAttribute(
    std::u16string_view attribute,
    std::vector<std::string>* messages) {
  IntegrityMetadataSet metadata_set;
  const std::string text = base::UTF16ToUTF8(attribute);
  const std::string_view characters =
      base::TrimWhitespaceASCII(text, base::TRIM_ALL);
  for (const std::string_view token : base::SplitStringPiece(
           characters, base::kWhitespaceASCII, base::KEEP_WHITESPACE,
           base::SPLIT_WANT_NONEMPTY)) {
    // ParseAttributeAlgorithm(). Algorithms that are not known are skipped,
    // so that strong ones can be used without breaking older browsers.
    std::optional<AlgorithmPrefix> found;
    for (const AlgorithmPrefix& prefix : kPrefixes) {
      if (token.starts_with(prefix.prefix) &&
          token.size() > prefix.prefix.size() &&
          token[prefix.prefix.size()] == '-') {
        found = prefix;
        break;
      }
    }
    if (!found) {
      if (messages) {
        messages->push_back(
            token.find('-') != std::string_view::npos
                ? base::StrCat({"Error parsing 'integrity' attribute ('", text,
                                "'). The specified hash algorithm must be one "
                                "of ",
                                kAlgorithmsForConsole, "."})
                : base::StrCat({"Error parsing 'integrity' attribute ('", text,
                                "'). The hash algorithm must be one of ",
                                kAlgorithmsForConsole,
                                ", followed by a '-' character."}));
      }
      continue;
    }
    const std::string_view rest = token.substr(found->prefix.size() + 1);
    const size_t question = rest.find('?');
    const std::string_view digest = rest.substr(0, question);
    const std::string_view options = question == std::string_view::npos
                                         ? std::string_view()
                                         : rest.substr(question + 1);
    // ParseDigest().
    if (digest.empty() || !std::ranges::all_of(digest, IsIntegrityCharacter)) {
      if (messages) {
        messages->push_back(base::StrCat(
            {"Error parsing 'integrity' attribute ('", text,
             "'). The digest must be a valid, base64-encoded value."}));
      }
      continue;
    }
    // No option has been defined yet, so each is ignored.
    if (messages && !options.empty() &&
        std::ranges::all_of(options, IsValueCharacter)) {
      messages->push_back(
          base::StrCat({"Ignoring unrecognized 'integrity' attribute option '",
                        options, "'."}));
    }
    // NormalizeToBase64(), then Base64Decode(), which forgives.
    std::string normalized(digest);
    std::ranges::replace(normalized, '-', '+');
    std::ranges::replace(normalized, '_', '/');
    std::string decoded;
    if (!base::Base64Decode(normalized, &decoded,
                            base::Base64DecodePolicy::kForgiving)) {
      decoded.clear();
    }
    IntegrityMetadata metadata;
    metadata.algorithm = found->algorithm;
    metadata.value.assign(decoded.begin(), decoded.end());
    (found->algorithm == IntegrityAlgorithm::kEd25519 ? metadata_set.public_keys
                                                      : metadata_set.hashes)
        .push_back(std::move(metadata));
  }
  return metadata_set;
}

bool CheckSubresourceIntegrity(const IntegrityMetadataSet& metadata,
                               std::string_view body,
                               std::string_view elided_url,
                               bool cors_same_origin,
                               std::string_view headers,
                               std::vector<std::string>& messages) {
  if (metadata.empty()) {
    return true;
  }
  if (!cors_same_origin) {
    messages.push_back(base::StrCat(
        {"Subresource Integrity: The resource '", elided_url,
         "' has an integrity attribute, but the resource requires the "
         "request to be CORS enabled to check the integrity, and it is not. "
         "The resource has been blocked because the integrity cannot be "
         "enforced."}));
    return false;
  }
  return CheckHashes(metadata.hashes, body, elided_url, messages) &&
         CheckSignatures(metadata.public_keys, elided_url, headers, messages);
}

}  // namespace mitto
