// A document's refresh, which a Refresh header of its response or a
// <meta http-equiv="refresh"> asks for: a navigation that starts once the
// document's load event has finished and the delay has passed, as Blink's
// HttpRefreshScheduler has it. Of several, the one of the shortest delay wins,
// the newest of equal ones.
//
// Upstream: third_party/blink/renderer/core/loader/http_refresh_scheduler.cc
// Upstream: third_party/blink/renderer/core/dom/document.cc
// Upstream: third_party/blink/renderer/platform/network/http_parsers.cc

#ifndef MITTO_DOM_HTTP_REFRESH_SCHEDULER_H_
#define MITTO_DOM_HTTP_REFRESH_SCHEDULER_H_

#include <optional>
#include <string>
#include <string_view>

#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "mitto/dom/frame_loader.h"
#include "url/gurl.h"

namespace mitto {

class Document;

// ParseHTTPRefresh(): a delay of whole seconds, from digits and full stops,
// then, after white space or one comma or semicolon, the URL, which may follow
// "url=" and stand in quotes, and which is null for none. nullopt for content
// that holds no delay. White space is the HTML Standard's for a meta element,
// and a space or a tab for a header.
struct HttpRefresh {
  base::TimeDelta delay;
  std::optional<std::u16string> url;
};
std::optional<HttpRefresh> ParseHttpRefresh(std::u16string_view content,
                                            bool html_white_space);

class HttpRefreshScheduler {
 public:
  // Where a refresh comes from.
  enum class Source {
    kHeader,
    kMetaTag,
  };

  explicit HttpRefreshScheduler(Document& document);

  HttpRefreshScheduler(const HttpRefreshScheduler&) = delete;
  HttpRefreshScheduler& operator=(const HttpRefreshScheduler&) = delete;

  ~HttpRefreshScheduler();

  // Document::MaybeHandleHttpRefresh(): schedules the refresh that `content`
  // says, of the document's own URL for none, and nothing for a document of
  // no window or a javascript: URL, which the console says.
  void MaybeHandle(std::u16string_view content, Source source);

  // Starts the delay of the refresh, if one waits and the document's load
  // event has finished.
  void MaybeStartTimer();

  // Drops the refresh, as when the document stops loading.
  void Cancel();
  // Whether a refresh waits whose delay is at most `interval`.
  bool IsScheduledWithin(base::TimeDelta interval) const {
    return refresh_ && refresh_->delay <= interval;
  }

 private:
  struct Refresh {
    base::TimeDelta delay;
    GURL url;
    ClientNavigationReason reason = ClientNavigationReason::kMetaTagRefresh;
  };

  void Schedule(base::TimeDelta delay,
                const GURL& url,
                ClientNavigationReason reason);
  // The refresh navigates: a reload to the document's own URL, a replace for
  // a delay of at most a second, and a new entry otherwise.
  void NavigateTask();

  const raw_ref<Document> document_;
  std::optional<Refresh> refresh_;
  bool timer_active_ = false;
  base::WeakPtrFactory<HttpRefreshScheduler> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_DOM_HTTP_REFRESH_SCHEDULER_H_
