// An img element's loader, as Blink's ImageLoader runs the HTML Standard's
// "update the image data": the URL it chose, a microtask before the fetch
// unless the image is in the list of available images, the document's load
// event held back meanwhile, load or error from a task once the fetch ends,
// complete, and decode()'s promises.
//
// Upstream: third_party/blink/renderer/core/loader/image_loader.h
// Upstream: third_party/blink/renderer/core/loader/image_loader.cc

#ifndef MITTO_DOM_IMAGE_LOADER_H_
#define MITTO_DOM_IMAGE_LOADER_H_

#include <stdint.h>

#include <memory>
#include <optional>
#include <string>

#include "mitto/dom/heap_vector.h"
#include "mitto/dom/resource.h"
#include "url/gurl.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-forward.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class Document;
class HTMLImageElement;

class ImageLoader final : public cppgc::GarbageCollected<ImageLoader>,
                          public ResourceClient {
 public:
  enum UpdateFromElementBehavior {
    // The default.
    kUpdateNormal,
    // Loads again a URL that failed before, as a change of src does.
    kUpdateIgnorePreviousError,
    // A change of the viewport, which fires no error.
    kUpdateSizeChanged,
    // Bypasses the cache.
    kUpdateForcedReload,
    // From the microtask itself.
    kUpdateFromMicrotask,
  };

  explicit ImageLoader(HTMLImageElement& element);
  ImageLoader(const ImageLoader&) = delete;
  ImageLoader& operator=(const ImageLoader&) = delete;
  ~ImageLoader();

  void Trace(cppgc::Visitor* visitor) const override;

  void UpdateFromElement(UpdateFromElementBehavior behavior = kUpdateNormal);
  void ElementDidMoveToNewDocument();
  // LoadDeferredImage(): a lazy image that waits for the viewport loads now.
  void LoadDeferredImage();
  // The element went into a document, where a lazy image that waits can come
  // near the viewport.
  void ElementInserted();

  bool ImageComplete() const { return image_complete_; }
  Resource* GetContent() const { return image_content_.Get(); }
  bool HadError() const { return !failed_load_url_.empty(); }
  // Whether the image is available or still loading, as the HTML Standard's
  // "potentially available" has it.
  bool ImageIsPotentiallyAvailable() const;
  // The natural size divided by a density, as an SVG image of no natural
  // size has the default object size. 0 by 0 for no image.
  void DensityCorrectedNaturalSize(float inverse_density,
                                   uint32_t& width,
                                   uint32_t& height) const;
  v8::Local<v8::Promise> Decode(bindings::ExceptionState& exception_state);

  // HTMLImageElement::StartLoadingImageDocument(): the next update takes
  // `content`, which an image document's loader feeds from its own response,
  // in place of a fetch.
  void SetImageForImageDocument(Resource& content) {
    image_content_for_image_document_ = &content;
  }

  // ResourceClient:
  void NotifyFinished(Resource& resource) override;
  void DataReceived(Resource& resource) override;

 private:
  class LoadEventDelay;
  class DecodeRequest;

  enum class UpdateType {
    kAsync,
    kSync,
  };

  // Blink's LazyImageLoadState.
  enum class LazyImageLoadState {
    kNone,
    // Waits until it comes near the viewport.
    kDeferred,
    // Came near it, and loads.
    kFullImage,
  };

  void DoUpdateFromElement(UpdateFromElementBehavior behavior,
                           UpdateType update_type);
  void EnqueueImageLoadingMicroTask(UpdateFromElementBehavior behavior);
  void RunMicroTask(uint64_t task, UpdateFromElementBehavior behavior);
  void UpdateImageState(Resource* new_image_content);
  void SetImageWithoutConsideringPendingLoadEvent(Resource* new_image_content);
  void ClearImage();
  GURL ImageSourceToUrl(const std::optional<std::u16string>& source) const;
  bool ShouldLoadImmediately(const GURL& url) const;
  void QueuePendingErrorEvent();
  void QueuePendingLoadEvent();
  void CancelPendingLoadEvent();
  void CancelPendingErrorEvent();
  void DispatchPendingEvent(uint64_t event, bool is_load);
  void DispatchDecodeRequestsIfComplete();
  void RejectPendingDecodes(UpdateType update_type = UpdateType::kAsync);
  void ProcessDecodeRequest(uint64_t request);
  bool IsDocumentActive() const;
  // LazyImageHelper::StartMonitoring(): a Page has no layout, so each lazy
  // image comes near the viewport at the next rendering opportunity, as the
  // owner chose (#284).
  void StartMonitoring();

  cppgc::Member<HTMLImageElement> element_;
  cppgc::Member<Resource> image_content_;
  cppgc::Member<Resource> image_content_for_image_document_;
  // The URL whose load failed an access check, which is not loaded again
  // until src changes.
  std::u16string failed_load_url_;
  bool image_complete_ = true;
  LazyImageLoadState lazy_image_load_state_ = LazyImageLoadState::kNone;
  // Which monitoring is current, so that one that stopped does nothing.
  uint64_t monitoring_ = 0;
  bool suppress_error_events_ = false;
  // The microtask that waits to run "update the image data", by number.
  uint64_t pending_task_ = 0;
  uint64_t last_task_ = 0;
  // The tasks that wait to fire load or error, by number, 0 for none.
  uint64_t pending_load_event_ = 0;
  uint64_t pending_error_event_ = 0;
  uint64_t last_event_ = 0;
  // What holds the document's load event back.
  std::unique_ptr<LoadEventDelay> delay_until_do_update_from_element_;
  std::unique_ptr<LoadEventDelay> delay_until_image_notify_finished_;
  std::unique_ptr<LoadEventDelay> delay_until_load_event_;
  std::unique_ptr<LoadEventDelay> delay_until_error_event_;
  HeapVector<DecodeRequest> decode_requests_;
  uint64_t last_decode_request_ = 0;
};

}  // namespace mitto

#endif  // MITTO_DOM_IMAGE_LOADER_H_
