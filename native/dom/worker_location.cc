#include "mitto/dom/worker_location.h"

#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/v8_worker_location.h"
#include "mitto/dom/resolved_url.h"

namespace mitto {

// static
WorkerLocation* WorkerLocation::Create(const GURL& url) {
  return bindings::MakeGarbageCollected<WorkerLocation>(url);
}

WorkerLocation::WorkerLocation(const GURL& url) : url_(url) {}

WorkerLocation::~WorkerLocation() = default;

const bindings::WrapperTypeInfo* WorkerLocation::GetWrapperTypeInfo() const {
  return &bindings::V8WorkerLocation::kWrapperTypeInfo;
}

void WorkerLocation::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
}

ResolvedURL WorkerLocation::Url() const {
  return ResolvedURL(base::UTF8ToUTF16(url_.possibly_invalid_spec()));
}

void WorkerLocation::SetUrl(const ResolvedURL& url) {
  // Every member is read-only.
}

std::u16string WorkerLocation::Input() const {
  return std::u16string();
}

}  // namespace mitto
