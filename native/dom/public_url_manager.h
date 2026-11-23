// What URL.createObjectURL() and revokeObjectURL() do for a context, as
// Blink's PublicURLManager does over the browser's BlobURLStore: a context
// registers its blob: URLs in the Page's registry (blob_url_registry.h), which
// revokes them once the context goes, as the browser does once a document or
// a worker goes. Loading what a blob: URL names is a Known Gap.
//
// Upstream: third_party/blink/renderer/core/fileapi/public_url_manager.h
// Upstream: third_party/blink/renderer/core/fileapi/public_url_manager.cc
// Upstream: third_party/blink/renderer/core/fileapi/url_file_api.cc

#ifndef MITTO_DOM_PUBLIC_URL_MANAGER_H_
#define MITTO_DOM_PUBLIC_URL_MANAGER_H_

#include <string>

#include "base/memory/scoped_refptr.h"
#include "mitto/dom/blob_data.h"

class GURL;

namespace mitto {

class ExecutionContext;

class PublicUrlManager {
 public:
  PublicUrlManager() = delete;

  // A new blob: URL of `context`'s origin for `blob`, or for nothing when
  // `blob` is null, as a MediaSource's URL is; empty in a context that has
  // gone, as Blink's stopped manager gives.
  static std::u16string RegisterUrl(ExecutionContext& context,
                                    scoped_refptr<BlobDataHandle> blob);
  // Revokes `url` if it is a blob: URL without a fragment that a context of
  // `context`'s origin made, as Blink's Revoke() and the browser check.
  static void Revoke(ExecutionContext& context, const GURL& url);
};

}  // namespace mitto

#endif  // MITTO_DOM_PUBLIC_URL_MANAGER_H_
