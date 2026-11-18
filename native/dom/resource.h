// What a document fetches for its elements, as Blink's Resource holds it: the
// request, the response and its body, and the clients that wait for its end.
// A document's ResourceFetcher makes each and keeps it for reuse.
//
// Upstream: third_party/blink/renderer/platform/loader/fetch/resource.h
// Upstream: third_party/blink/renderer/core/loader/resource/image_resource.cc

#ifndef MITTO_DOM_RESOURCE_H_
#define MITTO_DOM_RESOURCE_H_

#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "mitto/dom/fetcher.h"
#include "mitto/dom/heap_vector.h"
#include "mitto/dom/image_header.h"
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

  // The response: its status, MIME type essence in lower case, and body.
  int HttpStatus() const { return http_status_; }
  const std::string& MimeType() const { return mime_type_; }
  const std::string& Body() const { return body_; }

  // An image's header, once it has loaded without a decode error.
  const std::optional<ImageHeader>& Image() const { return image_; }
  bool HasImage() const { return image_.has_value(); }

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
  void ResponseReceived(int status, std::string mime_type);
  void AppendData(const std::string& bytes);
  // 0 when all of the body has come, or else a net error.
  void Finish(int net_error);

 private:
  const ResourceType type_;
  const GURL url_;
  const FetchMode mode_;
  const FetchCredentialsMode credentials_;
  ResourceStatus status_ = ResourceStatus::kPending;
  int http_status_ = 0;
  std::string mime_type_;
  std::string body_;
  std::optional<ImageHeader> image_;
  WeakHeapVector<ResourceClient> clients_;
  cppgc::WeakMember<ResourceFetcher> fetcher_;
  base::OnceClosure cancel_load_;
};

}  // namespace mitto

#endif  // MITTO_DOM_RESOURCE_H_
