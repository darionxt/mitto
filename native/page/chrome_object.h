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

#include "base/time/time.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-forward.h"

namespace mitto {

class DocumentLoadTiming;
class PerformanceTiming;

// What window.chrome reads of its window's document: its performance.timing,
// as Chrome reads WebPerformance, and the connection of its navigation's
// response. One for each window, which lives as long as the Page.
struct ChromeObjectData {
  // Starts the window's DocumentLoadTiming at `navigation_start`, the
  // window's time origin.
  explicit ChromeObjectData(base::TimeTicks navigation_start);
  ChromeObjectData(const ChromeObjectData&) = delete;
  ChromeObjectData& operator=(const ChromeObjectData&) = delete;
  ~ChromeObjectData();

  // The window's, which its document's loading fills in.
  cppgc::Persistent<DocumentLoadTiming> load_timing;
  // Reads it as performance.timing does.
  cppgc::Persistent<PerformanceTiming> timing;

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
