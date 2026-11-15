// What a page's storage is keyed by, as Chrome keys it: blink::StorageKey,
// without Mojo and the parts that only fenced and credentialless frames use.
// The library works it out for each document from what the Session loaded,
// and never takes a Page's word for it.
//
// Upstream: third_party/blink/public/common/storage_key/storage_key.h
// Upstream: third_party/blink/common/storage_key/storage_key.cc

#ifndef MITTO_STORAGE_STORAGE_KEY_H_
#define MITTO_STORAGE_STORAGE_KEY_H_

#include <stdint.h>

#include <compare>
#include <string>

#include "net/base/schemeful_site.h"
#include "url/origin.h"

namespace mitto {

class StorageKey {
 public:
  // Whether every frame from the document's up to the top-level one is
  // same-site with the top-level one: blink::mojom::AncestorChainBit.
  enum class AncestorChainBit : uint8_t {
    kSameSite = 0,
    kCrossSite = 1,
  };

  // The key of a top-level document of `origin`, which is not opaque, or of a
  // frame that is same-site with its top-level document all the way up.
  static StorageKey CreateFirstParty(const url::Origin& origin);

  // The key of a frame of `origin` in a page whose top-level site is
  // `top_level_site`. Third-party storage partitioning is on in Chrome 157, so
  // it keeps all three. `origin` and `top_level_site` are not opaque, and a
  // frame of another site than the top-level one is cross-site.
  static StorageKey Create(const url::Origin& origin,
                           const net::SchemefulSite& top_level_site,
                           AncestorChainBit ancestor_chain_bit);

  StorageKey(const StorageKey&);
  StorageKey& operator=(const StorageKey&);
  ~StorageKey();

  const url::Origin& origin() const { return origin_; }
  const net::SchemefulSite& top_level_site() const { return top_level_site_; }
  AncestorChainBit ancestor_chain_bit() const { return ancestor_chain_bit_; }

  // Whether the key is a top-level document's, as Blink's
  // IsFirstPartyContext() says.
  bool IsFirstPartyContext() const;

  // The key as Chrome writes it into its stores, such as the storage_key of
  // the maps of localStorage: the origin's URL, and for a cross-site frame "^0"
  // and the top-level site, or "^31" for one of the top-level site.
  std::string Serialize() const;

  friend bool operator==(const StorageKey&, const StorageKey&) = default;
  friend std::strong_ordering operator<=>(const StorageKey& first,
                                          const StorageKey& second);

 private:
  StorageKey(const url::Origin& origin,
             const net::SchemefulSite& top_level_site,
             AncestorChainBit ancestor_chain_bit);

  url::Origin origin_;
  net::SchemefulSite top_level_site_;
  AncestorChainBit ancestor_chain_bit_;
};

}  // namespace mitto

#endif  // MITTO_STORAGE_STORAGE_KEY_H_
