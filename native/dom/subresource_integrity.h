// Subresource Integrity as Blink checks it for a script that an element
// fetches: the integrity attribute's metadata, and the check of a response's
// body against it, with Blink's console errors.
//
// Upstream: third_party/blink/renderer/platform/loader/subresource_integrity.cc
// Upstream: third_party/blink/renderer/platform/loader/integrity_report.cc

#ifndef MITTO_DOM_SUBRESOURCE_INTEGRITY_H_
#define MITTO_DOM_SUBRESOURCE_INTEGRITY_H_

#include <stdint.h>

#include <string>
#include <string_view>
#include <vector>

namespace mitto {

// Ordered from the weakest hash to the strongest, as Blink's.
enum class IntegrityAlgorithm {
  kSha256,
  kSha384,
  kSha512,
  kEd25519,
};

struct IntegrityMetadata {
  IntegrityAlgorithm algorithm = IntegrityAlgorithm::kSha256;
  std::vector<uint8_t> value;
};

// The digests of hashes, and the public keys of signatures.
struct IntegrityMetadataSet {
  IntegrityMetadataSet();
  IntegrityMetadataSet(const IntegrityMetadataSet&);
  IntegrityMetadataSet& operator=(const IntegrityMetadataSet&);
  ~IntegrityMetadataSet();

  bool empty() const { return hashes.empty() && public_keys.empty(); }

  std::vector<IntegrityMetadata> hashes;
  std::vector<IntegrityMetadata> public_keys;
};

// SubresourceIntegrity::ParseIntegrityAttribute(): the metadata of an
// integrity attribute, and, in `messages` if not null, the console errors
// for what it skipped.
IntegrityMetadataSet ParseIntegrityAttribute(
    std::u16string_view attribute,
    std::vector<std::string>* messages);

// SubresourceIntegrity::CheckSubresourceIntegrity(): whether `body`, of a
// response from the URL that `elided_url` writes, with the header lines
// `headers`, matches `metadata`. A response that is not CORS-same-origin
// cannot be checked, and fails. Each failure adds its console error to
// `messages`.
bool CheckSubresourceIntegrity(const IntegrityMetadataSet& metadata,
                               std::string_view body,
                               std::string_view elided_url,
                               bool cors_same_origin,
                               std::string_view headers,
                               std::vector<std::string>& messages);

}  // namespace mitto

#endif  // MITTO_DOM_SUBRESOURCE_INTEGRITY_H_
