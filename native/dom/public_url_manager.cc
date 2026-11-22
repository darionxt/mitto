#include "mitto/dom/public_url_manager.h"

#include <stdint.h>

#include <utility>

#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/dom/blob_url_registry.h"
#include "mitto/dom/execution_context.h"
#include "url/gurl.h"

namespace mitto {

// static
std::u16string PublicUrlManager::RegisterUrl(
    ExecutionContext& context,
    scoped_refptr<BlobDataHandle> blob) {
  if (!context.HasContext()) {
    return std::u16string();
  }
  BlobUrlRegistry& registry = BlobUrlRegistry::Get();
  const uint64_t context_id = context.ContextId();
  if (registry.StartObserving(context_id)) {
    context.AddContextDestroyedCallback(base::BindOnce(
        [](uint64_t id) { BlobUrlRegistry::Get().RevokeAllOf(id); },
        context_id));
  }
  return base::UTF8ToUTF16(registry.Register(
      std::move(blob), context.GetSecurityOrigin(), context_id));
}

// static
void PublicUrlManager::Revoke(ExecutionContext& context, const GURL& url) {
  // A URL that cannot have been registered is left alone.
  if (!context.HasContext() || !url.SchemeIs("blob") || url.has_ref()) {
    return;
  }
  BlobUrlRegistry::Get().Revoke(url, context.GetSecurityOrigin());
}

}  // namespace mitto
