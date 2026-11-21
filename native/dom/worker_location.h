// The HTML Standard's WorkerLocation, a worker's `location`: the parts of the
// worker's URL, its script's final one, as Blink's WorkerLocation reads them
// through DOMURLUtilsReadOnly.
//
// Upstream: third_party/blink/renderer/core/workers/worker_location.h

#ifndef MITTO_DOM_WORKER_LOCATION_H_
#define MITTO_DOM_WORKER_LOCATION_H_

#include <string>

#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/url_utils.h"
#include "url/gurl.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class WorkerLocation final : public bindings::ScriptWrappable, public UrlUtils {
 public:
  // The location of a worker at `url`.
  static WorkerLocation* Create(const GURL& url);

  explicit WorkerLocation(const GURL& url);
  ~WorkerLocation() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface are UrlUtils' readers.

 private:
  // UrlUtils:
  ResolvedURL Url() const override;
  void SetUrl(const ResolvedURL& url) override;
  std::u16string Input() const override;

  const GURL url_;
};

}  // namespace mitto

#endif  // MITTO_DOM_WORKER_LOCATION_H_
