#include "mitto/page/favicon_loader.h"

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "mitto/dom/document.h"
#include "mitto/dom/fetcher.h"
#include "mitto/dom/html_head_element.h"
#include "mitto/dom/html_link_element.h"
#include "mitto/dom/html_parser_idioms.h"
#include "mitto/dom/image_header.h"
#include "mitto/dom/media_query_list.h"
#include "mitto/dom/resource.h"
#include "mitto/dom/window.h"
#include "mitto/encoding/content_type.h"
#include "mitto/header_rules.h"
#include "url/url_constants.h"
#include "v8/include/cppgc/persistent.h"

namespace mitto {
namespace {

// Of a body that is no image, as much as says so.
constexpr size_t kMaxIconBytes = 1 << 20;

// ImageDownloader's loader, whose WebAssociatedURLLoader names it "internal",
// which the window's Performance Timeline gives as "other".
constexpr char16_t kInitiatorType[] = u"other";

// What the favicon service holds of the icon URLs that the Page fetched:
// those that gave an image, and those that did not, which it skips, as
// UnableToDownloadFavicon() has it. A Page has a process of its own.
struct FetchedIcons {
  std::set<std::string> images;
  std::set<std::string> failures;
};

FetchedIcons& Fetched() {
  static base::NoDestructor<FetchedIcons> fetched;
  return *fetched;
}

void FetchFrom(const cppgc::WeakPersistent<Window>& window,
               std::vector<GURL> candidates,
               size_t index);

// One download of an icon, which tries the next candidate if no image comes
// of it, as FaviconHandler::OnDidDownloadFavicon() does.
class FaviconClient final : public FetchClient {
 public:
  FaviconClient(cppgc::WeakPersistent<Window> window,
                std::vector<GURL> candidates,
                size_t index)
      : window_(std::move(window)),
        candidates_(std::move(candidates)),
        index_(index) {}
  FaviconClient(const FaviconClient&) = delete;
  FaviconClient& operator=(const FaviconClient&) = delete;
  ~FaviconClient() override = default;

  // FetchClient:
  void OnStarted(FetchController& controller) override {
    controller_ = &controller;
  }
  void OnResponseStarted(const FetchResponseHead& head) override {
    mime_type_ =
        GetMimeTypeAndCharset(HeaderField(head.headers, "content-type"))
            .mime_type;
  }
  void OnResponseBody(std::string bytes) override {
    controller_->TookBody(bytes.size());
    if (body_.size() < kMaxIconBytes) {
      body_.append(bytes, 0, kMaxIconBytes - body_.size());
    }
  }
  void OnResponseEnded(int net_error) override {
    // ImageDownloader decodes what came, whatever its status.
    const std::string& url = candidates_[index_].spec();
    if (!net_error && ReadImageHeader(body_, mime_type_)) {
      Fetched().images.insert(url);
      return;
    }
    // A redirect that this document refuses, another may take.
    if (net_error != kBlockedByClient) {
      Fetched().failures.insert(url);
    }
    FetchFrom(window_, std::move(candidates_), index_ + 1);
  }

 private:
  const cppgc::WeakPersistent<Window> window_;
  std::vector<GURL> candidates_;
  const size_t index_;
  raw_ptr<FetchController> controller_ = nullptr;
  std::string mime_type_;
  std::string body_;
};

void FetchFrom(const cppgc::WeakPersistent<Window>& window,
               std::vector<GURL> candidates,
               size_t index) {
  Window* const active = window.Get();
  Fetcher* const fetcher = active ? active->GetFetcher() : nullptr;
  if (!fetcher) {
    return;
  }
  // DownloadCurrentCandidateOrAskFaviconService(): an icon that the service
  // has ends the search, and one that failed is skipped.
  for (; index < candidates.size(); ++index) {
    const std::string& url = candidates[index].spec();
    if (Fetched().images.contains(url)) {
      return;
    }
    if (Fetched().failures.contains(url)) {
      continue;
    }
    if (!active->BlocksCredentialedSubresource(candidates[index])) {
      break;
    }
    // Another document may take it, which the favicon service has heard
    // nothing of.
    active->AddBlockedResourceTiming(candidates[index], kInitiatorType,
                                     /*render_blocking=*/false);
  }
  if (index >= candidates.size()) {
    return;
  }
  const GURL url = candidates[index];
  // ImageDownloader reads a data: URL in the renderer, with no request.
  if (!url.SchemeIsHTTPOrHTTPS()) {
    Fetched().images.insert(url.spec());
    return;
  }
  FetchRequestData request;
  request.method = "GET";
  request.url = url.spec();
  request.destination = FetchDestination::kFavicon;
  request.initiator_type = kInitiatorType;
  fetcher->Fetch(std::move(request), std::make_unique<FaviconClient>(
                                         window, std::move(candidates), index));
}

}  // namespace

std::vector<GURL> FaviconCandidates(const Document& document) {
  std::vector<GURL> favicons;
  if (const HTMLHeadElement* head = document.Head()) {
    for (Node* child = head->FirstChild(); child;
         child = child->NextSibling()) {
      if (!child->IsElementNode() ||
          !static_cast<Element*>(child)->HasHTMLTagName(u"link")) {
        continue;
      }
      const auto* const link = static_cast<const HTMLLinkElement*>(child);
      const std::u16string* rel = link->FindAttribute(u"rel");
      if (!rel || !LinkRelAttribute(*rel).is_icon) {
        continue;
      }
      const std::u16string* href = link->FindAttribute(u"href");
      const std::u16string_view stripped =
          href ? StripHTMLSpaces(*href) : std::u16string_view();
      if (stripped.empty()) {
        continue;
      }
      const GURL url = document.CompleteUrl(stripped);
      if (!url.is_valid()) {
        continue;
      }
      const std::u16string* media = link->FindAttribute(u"media");
      if (media && !media->empty() &&
          !MediaAttributeMatches(document, *media)) {
        continue;
      }
      favicons.push_back(url);
    }
  }
  // Icons seen later take precedence.
  std::reverse(favicons.begin(), favicons.end());
  if (favicons.empty()) {
    // IconURL::DefaultFavicon(): the document's scheme, host and port, and
    // /favicon.ico.
    const GURL document_url(document.UrlForBinding());
    if (document_url.SchemeIsHTTPOrHTTPS()) {
      GURL::Replacements replacements;
      replacements.SetPathStr("/favicon.ico");
      replacements.ClearQuery();
      replacements.ClearRef();
      replacements.ClearUsername();
      replacements.ClearPassword();
      favicons.push_back(document_url.ReplaceComponents(replacements));
    }
  }
  return favicons;
}

void FetchFavicon(Document& document) {
  Window* const window = document.DefaultView();
  if (!window || &window->GetDocument() != &document) {
    return;
  }
  FetchFrom(cppgc::WeakPersistent<Window>(window), FaviconCandidates(document),
            0);
}

}  // namespace mitto
