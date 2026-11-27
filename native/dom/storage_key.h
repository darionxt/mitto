// What a context's storage is keyed by, serialized as blink::StorageKey
// serializes it, which the areas of localStorage and sessionStorage, the
// BroadcastChannels and the lock group of the context go by. The library works
// out the same for each document that it loaded (storage/storage_key.h), and
// hands over with it what comes from the Session's other Pages.
//
// Upstream: third_party/blink/common/storage_key/storage_key.cc

#ifndef MITTO_DOM_STORAGE_KEY_H_
#define MITTO_DOM_STORAGE_KEY_H_

#include <string>
#include <vector>

#include "base/unguessable_token.h"
#include "url/origin.h"

namespace mitto {

// The key of a document of `origin` in a frame whose ancestors' documents are
// of `ancestors`, from its parent's to the top-level one's, as
// RenderFrameHostImpl::CalculateStorageKey() has it with third-party storage
// partitioning. One under a document of an opaque origin, and one of an opaque
// origin, names that origin by its nonce, which no document of another Page
// has.
std::string SerializeStorageKey(const url::Origin& origin,
                                const std::vector<url::Origin>& ancestors);

// The key of a credentialless document of `origin`,
// StorageKey::CreateWithNonce()'s, whose nonce is that of the credentialless
// frames under its top-level document, which no document under another one
// has.
std::string SerializeStorageKeyWithNonce(const url::Origin& origin,
                                         const base::UnguessableToken& nonce);

}  // namespace mitto

#endif  // MITTO_DOM_STORAGE_KEY_H_
