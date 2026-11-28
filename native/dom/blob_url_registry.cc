#include "mitto/dom/blob_url_registry.h"

#include <optional>
#include <utility>

#include "base/no_destructor.h"
#include "base/strings/strcat.h"
#include "base/uuid.h"
#include "url/gurl.h"

namespace mitto {
namespace {

// `url` without its fragment, as the registry keys it.
std::string KeyOf(const GURL& url) {
  return url.GetWithoutRef().spec();
}

}  // namespace

BlobUrlRegistry::Entry::Entry() = default;
BlobUrlRegistry::Entry::Entry(const Entry&) = default;
BlobUrlRegistry::Entry& BlobUrlRegistry::Entry::operator=(const Entry&) =
    default;
BlobUrlRegistry::Entry::~Entry() = default;

// static
BlobUrlRegistry& BlobUrlRegistry::Get() {
  static base::NoDestructor<BlobUrlRegistry> registry;
  return *registry;
}

BlobUrlRegistry::BlobUrlRegistry() = default;
BlobUrlRegistry::~BlobUrlRegistry() = default;

std::string BlobUrlRegistry::Register(scoped_refptr<BlobDataHandle> blob,
                                      const url::Origin& origin,
                                      uint64_t context_id) {
  const std::string url =
      base::StrCat({"blob:", origin.Serialize(), "/",
                    base::Uuid::GenerateRandomV4().AsLowercaseString()});
  Entry entry;
  entry.blob = std::move(blob);
  entry.origin = origin;
  entry.context_id = context_id;
  base::AutoLock lock(lock_);
  entries_[url] = std::move(entry);
  urls_of_context_[context_id].insert(url);
  return url;
}

void BlobUrlRegistry::Revoke(const GURL& url, const url::Origin& origin) {
  base::AutoLock lock(lock_);
  const auto found = entries_.find(KeyOf(url));
  if (found == entries_.end() ||
      !found->second.origin.IsSameOriginWith(origin)) {
    return;
  }
  const auto urls = urls_of_context_.find(found->second.context_id);
  if (urls != urls_of_context_.end()) {
    urls->second.erase(found->first);
    if (urls->second.empty()) {
      urls_of_context_.erase(urls);
    }
  }
  entries_.erase(found);
}

void BlobUrlRegistry::RevokeAllOf(uint64_t context_id) {
  base::AutoLock lock(lock_);
  const auto urls = urls_of_context_.find(context_id);
  if (urls == urls_of_context_.end()) {
    return;
  }
  for (const std::string& url : urls->second) {
    entries_.erase(url);
  }
  urls_of_context_.erase(urls);
}

bool BlobUrlRegistry::HasUrlsOf(uint64_t context_id) const {
  base::AutoLock lock(lock_);
  return urls_of_context_.contains(context_id);
}

bool BlobUrlRegistry::StartObserving(uint64_t context_id) {
  base::AutoLock lock(lock_);
  return observed_contexts_.insert(context_id).second;
}

scoped_refptr<BlobDataHandle> BlobUrlRegistry::Resolve(const GURL& url) const {
  base::AutoLock lock(lock_);
  const auto found = entries_.find(KeyOf(url));
  return found == entries_.end() ? nullptr : found->second.blob;
}

url::Origin BlobUrlRegistry::OriginOf(const GURL& url) const {
  const url::Origin origin = url::Origin::Create(url);
  if (!origin.opaque()) {
    return origin;
  }
  const std::optional<Entry> entry = ResolveEntry(url.GetWithoutRef());
  return entry ? entry->origin : origin;
}

std::optional<BlobUrlRegistry::Entry> BlobUrlRegistry::ResolveEntry(
    const GURL& url) const {
  base::AutoLock lock(lock_);
  const auto found = entries_.find(KeyOf(url));
  if (found == entries_.end()) {
    return std::nullopt;
  }
  return found->second;
}

}  // namespace mitto
