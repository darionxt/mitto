#include "mitto/storage/storage_key.h"

#include <tuple>

#include "base/check.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "url/gurl.h"

namespace mitto {

namespace {

// Blink's EncodedAttribute, which a serialized key writes after a "^".
constexpr char kTopLevelSiteSeparator[] = "^0";
constexpr char kAncestorChainBitSeparator[] = "^3";

}  // namespace

// static
StorageKey StorageKey::CreateFirstParty(const url::Origin& origin) {
  DCHECK(!origin.opaque());
  return StorageKey(origin, net::SchemefulSite(origin),
                    AncestorChainBit::kSameSite);
}

// static
StorageKey StorageKey::Create(const url::Origin& origin,
                              const net::SchemefulSite& top_level_site,
                              AncestorChainBit ancestor_chain_bit) {
  DCHECK(!origin.opaque());
  DCHECK(!top_level_site.opaque());
  DCHECK(top_level_site.IsSameSiteWith(origin) ||
         ancestor_chain_bit == AncestorChainBit::kCrossSite);
  return StorageKey(origin, top_level_site, ancestor_chain_bit);
}

StorageKey::StorageKey(const url::Origin& origin,
                       const net::SchemefulSite& top_level_site,
                       AncestorChainBit ancestor_chain_bit)
    : origin_(origin),
      top_level_site_(top_level_site),
      ancestor_chain_bit_(ancestor_chain_bit) {}

StorageKey::StorageKey(const StorageKey&) = default;
StorageKey& StorageKey::operator=(const StorageKey&) = default;
StorageKey::~StorageKey() = default;

bool StorageKey::IsFirstPartyContext() const {
  return ancestor_chain_bit_ == AncestorChainBit::kSameSite &&
         top_level_site_.IsSameSiteWith(origin_);
}

std::string StorageKey::Serialize() const {
  if (ancestor_chain_bit_ == AncestorChainBit::kSameSite) {
    return origin_.GetURL().spec();
  }
  if (top_level_site_.IsSameSiteWith(origin_)) {
    return base::StrCat(
        {origin_.GetURL().spec(), kAncestorChainBitSeparator,
         base::NumberToString(static_cast<int>(ancestor_chain_bit_))});
  }
  return base::StrCat({origin_.GetURL().spec(), kTopLevelSiteSeparator,
                       top_level_site_.Serialize()});
}

std::strong_ordering operator<=>(const StorageKey& first,
                                 const StorageKey& second) {
  // As Blink orders them: by origin, then top-level site, then the bit.
  if (first.origin_ != second.origin_) {
    return first.origin_ < second.origin_ ? std::strong_ordering::less
                                          : std::strong_ordering::greater;
  }
  if (first.top_level_site_ != second.top_level_site_) {
    return first.top_level_site_ < second.top_level_site_
               ? std::strong_ordering::less
               : std::strong_ordering::greater;
  }
  return first.ancestor_chain_bit_ <=> second.ancestor_chain_bit_;
}

}  // namespace mitto
