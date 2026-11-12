// window.chrome as Chrome's renderer puts it on a top-level page that no
// extension names: loadTimes() and csi(), which Chrome's own renderer code
// adds, and app, which its extensions system gives every web page. Members
// that only an installed extension's pages or the Web Store get, such as
// chrome.runtime, are not there.
//
// Upstream: content/public/renderer/chrome_object_extensions_utils.cc
// Upstream: chrome/renderer/loadtimes_bindings.cc
// Upstream: chrome/renderer/extensions/api/app_hooks_delegate.cc
// Upstream: chrome/common/extensions/api/app.json
// Upstream: extensions/renderer/bindings/api_binding.cc
// Upstream: extensions/renderer/bindings/api_signature.cc

#ifndef MITTO_PAGE_CHROME_OBJECT_H_
#define MITTO_PAGE_CHROME_OBJECT_H_

#include <string>

#include "base/time/time.h"
#include "v8/include/v8-forward.h"

namespace mitto {

// What window.chrome reads of its window's document, as Chrome reads it from
// the frame's DocumentLoadTiming and the response of its navigation. One for
// each window, which lives as long as the Page.
struct ChromeObjectData {
  ChromeObjectData();
  ChromeObjectData(const ChromeObjectData&) = delete;
  ChromeObjectData& operator=(const ChromeObjectData&) = delete;
  ~ChromeObjectData();

  // Starts the times at `navigation_start`, the window's time origin, which
  // reads as a Unix time from the wall clock now.
  void StartAt(base::TimeTicks navigation_start);

  // A time of the loading as Blink's PerformanceTiming gives it: whole
  // milliseconds since the Unix epoch, or 0 for one that has not come.
  int64_t ToIntegerMilliseconds(base::TimeTicks time) const;

  // The Unix time at the zero of base::TimeTicks, as Performance takes it.
  base::Time unix_at_zero_monotonic;
  base::TimeTicks navigation_start;
  base::TimeTicks response_start;
  base::TimeTicks dom_content_loaded_end;
  base::TimeTicks load_end;

  // Of the navigation's response, as net::HttpResponseInfo has it.
  std::string connection_info = "unknown";
  std::string alpn_negotiated_protocol = "unknown";
  bool was_fetched_via_spdy = false;
  bool was_alpn_negotiated = false;

  // Whether the window has gone, so that its callbacks no longer run, as
  // Chrome's extensions system drops a request of an invalid context.
  bool detached = false;
};

// Puts window.chrome on the global object of `context`, which reads `data`.
// Chrome adds it once the context's global object has its interface objects
// and before what the context decides is installed on them.
void InstallChromeObject(v8::Local<v8::Context> context,
                         ChromeObjectData& data);

}  // namespace mitto

#endif  // MITTO_PAGE_CHROME_OBJECT_H_
