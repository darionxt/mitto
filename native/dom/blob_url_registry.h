// The blob: URLs of a Page, as Chrome's browser keeps them in its
// BlobUrlRegistry: each names a blob, for the context that made it, until that
// context revokes it or goes. Chrome's registry is the browser's, so another
// renderer of the same storage key reaches a blob: URL too; a Page's reaches
// only its own contexts, a Known Gap. A Page's contexts run on the window's
// thread and on each worker's, so any of them may use it, a frame's among
// them.
//
// Upstream: storage/browser/blob/blob_url_registry.h
// Upstream: storage/browser/blob/blob_url_store_impl.cc

#ifndef MITTO_DOM_BLOB_URL_REGISTRY_H_
#define MITTO_DOM_BLOB_URL_REGISTRY_H_

#include <stdint.h>

#include <map>
#include <optional>
#include <set>
#include <string>

#include "base/memory/scoped_refptr.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "mitto/dom/blob_data.h"
#include "url/origin.h"

class GURL;

namespace mitto {

class BlobUrlRegistry {
 public:
  // The Page's one.
  static BlobUrlRegistry& Get();

  BlobUrlRegistry();
  BlobUrlRegistry(const BlobUrlRegistry&) = delete;
  BlobUrlRegistry& operator=(const BlobUrlRegistry&) = delete;
  ~BlobUrlRegistry();

  // A new URL for `blob`, which context `context_id` of `origin` made: blob:,
  // the origin serialized, or "null" for an opaque one, "/" and a new UUID, as
  // the browser's BlobURLStoreImpl makes one. Null for `blob` registers a URL
  // that names nothing, as a MediaSource's does.
  std::string Register(scoped_refptr<BlobDataHandle> blob,
                       const url::Origin& origin,
                       uint64_t context_id);
  // Revokes `url`, a blob: URL without a fragment, if a context of `origin`
  // made it.
  void Revoke(const GURL& url, const url::Origin& origin);
  // Revokes every URL that context `context_id` made, as the browser does once
  // the document or worker goes.
  void RevokeAllOf(uint64_t context_id);
  // Whether it has a URL of context `context_id`.
  bool HasUrlsOf(uint64_t context_id) const;
  // Notes that the caller will revoke context `context_id`'s URLs once it
  // goes: true the first time for a context, and false after that.
  bool StartObserving(uint64_t context_id);

  // A registered URL: the blob that it names, null for a MediaSource's, and
  // the origin of the context that made it, which a blob:null URL's origin
  // is, as Blink's BlobURLNullOriginMap keeps it.
  struct Entry {
    Entry();
    Entry(const Entry&);
    Entry& operator=(const Entry&);
    ~Entry();

    scoped_refptr<BlobDataHandle> blob;
    url::Origin origin;
    uint64_t context_id = 0;
  };

  // What `url`, without its fragment, names, if it is registered.
  std::optional<Entry> ResolveEntry(const GURL& url) const;
  // The blob that `url` names, without its fragment, or null.
  scoped_refptr<BlobDataHandle> Resolve(const GURL& url) const;

 private:
  mutable base::Lock lock_;
  // By the URL's spec, without a fragment.
  std::map<std::string, Entry> entries_ GUARDED_BY(lock_);
  std::map<uint64_t, std::set<std::string>> urls_of_context_ GUARDED_BY(lock_);
  std::set<uint64_t> observed_contexts_ GUARDED_BY(lock_);
};

}  // namespace mitto

#endif  // MITTO_DOM_BLOB_URL_REGISTRY_H_
