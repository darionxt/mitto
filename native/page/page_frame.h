// A frame of a Page, on the Page program's main thread: its window's context,
// with what the window runs in it, and the loader of its document and that of
// the navigation that waits for its response, as Blink's LocalFrame,
// LocalWindowProxy and FrameLoader, and content's RenderFrameImpl, keep them.
// Each document that the frame loads gets a window in a context of its own,
// whose timers, idle callbacks, animation frames, fetches, tasks and workers
// the frame keeps until the next document takes its place.
//
// It is each of its windows' NavigationClient: a navigation that a document
// starts goes to the Session as a request of Kind navigation, and its
// response, unless it has none, takes the place of the document once the old
// one has unloaded, as Blink's FrameLoader::CommitNavigation() has it.
//
// A Page has one frame so far, its top-level one.
//
// Upstream: third_party/blink/renderer/core/frame/local_frame.cc
// Upstream: third_party/blink/renderer/bindings/core/v8/local_window_proxy.cc
// Upstream: third_party/blink/renderer/core/loader/frame_loader.cc
// Upstream: content/renderer/render_frame_impl.cc

#ifndef MITTO_PAGE_PAGE_FRAME_H_
#define MITTO_PAGE_PAGE_FRAME_H_

#include <stdint.h>

#include <memory>
#include <optional>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ref.h"
#include "base/time/time.h"
#include "mitto/dom/navigation_client.h"
#include "mitto/page_messages.h"
#include "url/gurl.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-persistent-handle.h"

namespace mitto {

class AnimationFrameController;
struct ChromeObjectData;
class DOMTimerCoordinator;
class Document;
class DocumentLoadTiming;
class DocumentLoader;
class IdleTaskController;
class PageFetcher;
class PageWorkerStarter;
class ScriptRunner;
class WindowTasks;

class PageFrame final : public NavigationClient {
 public:
  // A frame of the Page that `runner` runs, whose ID in the Page is `id`.
  PageFrame(ScriptRunner& runner, uint64_t id);

  PageFrame(const PageFrame&) = delete;
  PageFrame& operator=(const PageFrame&) = delete;

  ~PageFrame() override;

  // The frame's ID in the Page, which it keeps as long as it lives and no
  // other frame of the Page ever has, as Chrome's FrameTreeNode ID.
  uint64_t id() const { return id_; }

  // Makes the frame's first window, whose document is an initial about:blank
  // with an opaque origin, as Blink makes a tab's, and whose
  // navigator.connection gives `connection`.
  void Start(const PageConnection& connection);

  // The context of the frame's current window, or an empty one before
  // Start().
  v8::Local<v8::Context> context() const;

  // Has the navigation that the Session makes when the Page opens load the
  // frame's document, if it makes one.
  void ExpectOpeningNavigation();

  // NavigationClient:
  void BeginNavigation(const NavigationRequest& request) override;
  void DidNavigateWithinDocument(const GURL& url) override;
  bool HasProvisionalNavigation() const override;
  void StopAllLoaders() override;

  // What a loader of the frame calls.
  //
  // The response of `loader`'s navigation has come: the frame's document
  // stops loading and unloads, and a new context's document at `url` takes
  // its place, which this returns: an XMLDocument of `xml_content_type`
  // where one is given, in a cross-origin isolated agent if
  // `cross_origin_isolated`. `loader` becomes the document's.
  Document& CommitNavigation(
      DocumentLoader& loader,
      const GURL& url,
      base::TimeTicks navigation_start,
      const PageConnection& connection,
      bool cross_origin_isolated,
      const std::optional<std::u16string>& xml_content_type = std::nullopt);
  // `loader`'s navigation ended with no response, and the frame keeps its
  // document. `loader` goes.
  void NavigationEndedWithoutDocument(DocumentLoader& loader);
  // How the current window's document loads, which its document's loading
  // fills in, and performance.timing and window.chrome read.
  DocumentLoadTiming& document_load_timing();

 private:
  // Takes `loader` from where the frame holds it, as the opening navigation's
  // or the one that waits.
  std::unique_ptr<DocumentLoader> Take(DocumentLoader& loader);

  // Makes a new context, whose window has an empty document at `url`, the
  // response of navigation `navigation`, in place of the current one, tells
  // the Session of it, and returns the document. The old window's timers,
  // idle callbacks, animation frames, fetches, tasks and workers stop with
  // its context. The new window's performance.timeOrigin is
  // `navigation_start`, and its navigator.connection gives `connection`, and
  // its agent is cross-origin isolated if `cross_origin_isolated`. With
  // `xml_content_type`, the document is an XMLDocument of that MIME type, as
  // Blink's DocumentInit makes one for an XML response, and otherwise an
  // HTMLDocument.
  Document& CommitDocument(
      uint64_t navigation,
      const GURL& url,
      base::TimeTicks navigation_start,
      const PageConnection& connection,
      bool cross_origin_isolated,
      const std::optional<std::u16string>& xml_content_type);
  // Makes a window's context, with what Chrome's renderer adds to it, whose
  // time origin is `time_origin` and whose document is at `url`, which
  // decides whether it is a secure context and its security token, in a
  // cross-origin isolated agent if `cross_origin_isolated`.
  v8::Local<v8::Context> NewContext(base::TimeTicks time_origin,
                                    const GURL& url,
                                    bool cross_origin_isolated = false);
  // Gives the current context's window its timers, what its fetch() goes
  // through, and what it reads of the browser and the machine, with
  // `connection` for its document, and the ID of its document, which it tells
  // the Session of as made of `source`, the response of `navigation` for one
  // of a navigation.
  void AttachWindow(const PageConnection& connection,
                    PageDocumentSource source,
                    uint64_t navigation);
  // What goes with the current window's context: its timers, idle callbacks,
  // animation frames, fetches, tasks and workers, and the inspector's and
  // window.chrome's hold on it.
  void DetachWindow();

  const raw_ref<ScriptRunner> runner_;
  const uint64_t id_;
  v8::Global<v8::Context> context_;
  // What the current window runs in its context, which goes with it.
  std::unique_ptr<DOMTimerCoordinator> timers_;
  std::unique_ptr<IdleTaskController> idle_tasks_;
  std::unique_ptr<AnimationFrameController> animation_frames_;
  std::unique_ptr<PageFetcher> fetcher_;
  std::unique_ptr<WindowTasks> window_tasks_;
  std::unique_ptr<PageWorkerStarter> worker_starter_;
  // What window.chrome of the current window reads, which the runner keeps
  // as long as the Page, for a callback of a window that has gone.
  raw_ptr<ChromeObjectData> chrome_object_data_ = nullptr;
  // The loader of the frame's document, none for about:blank.
  std::unique_ptr<DocumentLoader> current_;
  // The navigation that the Session makes when the Page opens, until it ends.
  std::unique_ptr<DocumentLoader> opening_;
  // The navigation that a document started, while it waits for its response,
  // and what RenderFrameImpl compares a new one with to drop it as the same.
  std::unique_ptr<DocumentLoader> provisional_;
  NavigationRequest provisional_request_;
  base::TimeTicks provisional_start_;
  uint64_t provisional_cookie_writes_ = 0;
};

}  // namespace mitto

#endif  // MITTO_PAGE_PAGE_FRAME_H_
