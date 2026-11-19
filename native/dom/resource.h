// What a document fetches for its elements, as Blink's Resource holds it: the
// request, the response and its body, and the clients that wait for its end.
// A document's ResourceFetcher makes each and keeps it for reuse.
//
// Upstream: third_party/blink/renderer/platform/loader/fetch/resource.h
// Upstream: third_party/blink/renderer/platform/loader/fetch/resource.cc
// Upstream: third_party/blink/renderer/core/loader/resource/image_resource.cc

#ifndef MITTO_DOM_RESOURCE_H_
#define MITTO_DOM_RESOURCE_H_

#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "mitto/dom/fetcher.h"
#include "mitto/dom/heap_vector.h"
#include "mitto/dom/image_header.h"
#include "mitto/dom/subresource_integrity.h"
#include "url/gurl.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class Resource;
class ResourceFetcher;

// Blink's ResourceType, of those a Page fetches through its ResourceFetcher.
enum class ResourceType {
  kImage,
  kCSSStyleSheet,
  kScript,
  kFont,
  // What a preload as fetch asks for.
  kRaw,
};

// Blink's ResourceStatus.
enum class ResourceStatus {
  kPending,
  kCached,
  kLoadError,
  kDecodeError,
};

// Hears of a resource's end. The resource holds its clients weakly.
class ResourceClient : public cppgc::GarbageCollectedMixin {
 public:
  // Once the resource has all of its body, or failed, and at once for one
  // added after that.
  virtual void NotifyFinished(Resource& resource) = 0;
  // Each time more of its body has come, as ImageResourceContent tells its
  // observers that the image changed.
  virtual void DataReceived(Resource& resource) {}
};

class Resource : public cppgc::GarbageCollected<Resource> {
 public:
  Resource(ResourceType type,
           GURL url,
           FetchMode mode,
           FetchCredentialsMode credentials);
  Resource(const Resource&) = delete;
  Resource& operator=(const Resource&) = delete;
  ~Resource();

  void Trace(cppgc::Visitor* visitor) const;

  ResourceType GetType() const { return type_; }
  // The URL it was requested at, which redirects do not change.
  const GURL& Url() const { return url_; }
  FetchMode Mode() const { return mode_; }
  FetchCredentialsMode Credentials() const { return credentials_; }
  ResourceStatus GetStatus() const { return status_; }
  bool IsLoading() const { return status_ == ResourceStatus::kPending; }
  bool IsLoaded() const { return status_ != ResourceStatus::kPending; }
  bool ErrorOccurred() const {
    return status_ == ResourceStatus::kLoadError ||
           status_ == ResourceStatus::kDecodeError;
  }
  // Whether it failed on its way, as a CORS check does, not in its body.
  bool LoadFailed() const { return status_ == ResourceStatus::kLoadError; }
  // The net error it failed with, 0 for none.
  int NetError() const { return net_error_; }

  // The response, once it has started: its head, its MIME type essence in
  // lower case, and its body, which an image does not keep.
  const FetchResponseHead& Response() const { return response_; }
  int HttpStatus() const { return response_.status; }
  const std::string& MimeType() const { return mime_type_; }
  const std::string& Body() const { return body_; }

  // An image's header, once it has loaded without a decode error.
  const std::optional<ImageHeader>& Image() const { return image_; }
  bool HasImage() const { return image_.has_value(); }

  // The integrity metadata it was requested with, which a request with
  // other metadata does not reuse it for.
  const IntegrityMetadataSet& Integrity() const { return integrity_; }
  void SetIntegrity(IntegrityMetadataSet integrity) {
    integrity_ = std::move(integrity);
  }

  // The encoding of the request that made it, which decodes its text for
  // each element that takes it, as Blink's resource keeps its decoder.
  const std::string& Charset() const { return charset_; }
  void SetCharset(std::string charset) { charset_ = std::move(charset); }

  // Whether a link preload made it, and no other request has used it yet.
  bool IsUnusedPreload() const { return is_unused_preload_; }
  void SetIsUnusedPreload(bool unused) { is_unused_preload_ = unused; }

  void AddClient(ResourceClient* client);
  // Removes one of `client`. A load that loses its last client is cancelled
  // soon, as Blink's is.
  void RemoveClient(ResourceClient* client);
  bool HasClients() const;

  // What the document's fetcher keeps: itself, and what cancels the fetch
  // while it runs.
  void SetFetcher(ResourceFetcher* fetcher) { fetcher_ = fetcher; }
  void SetCancelLoad(base::OnceClosure cancel) {
    cancel_load_ = std::move(cancel);
  }
  base::OnceClosure TakeCancelLoad() { return std::move(cancel_load_); }

  // What the fetch hears, in order.
  void ResponseReceived(FetchResponseHead head, std::string mime_type);
  void AppendData(const std::string& bytes);
  // 0 when all of the body has come, or else a net error.
  void Finish(int net_error);

 private:
  const ResourceType type_;
  const GURL url_;
  const FetchMode mode_;
  const FetchCredentialsMode credentials_;
  ResourceStatus status_ = ResourceStatus::kPending;
  int net_error_ = 0;
  FetchResponseHead response_;
  std::string mime_type_;
  std::string body_;
  std::optional<ImageHeader> image_;
  IntegrityMetadataSet integrity_;
  std::string charset_;
  bool is_unused_preload_ = false;
  WeakHeapVector<ResourceClient> clients_;
  cppgc::WeakMember<ResourceFetcher> fetcher_;
  base::OnceClosure cancel_load_;
};

// Blink's IntegrityMetadata::SetsEqual(): the same hashes and public keys,
// in any order.
bool IntegritySetsEqual(const IntegrityMetadataSet& a,
                        const IntegrityMetadataSet& b);

}  // namespace mitto

#endif  // MITTO_DOM_RESOURCE_H_
