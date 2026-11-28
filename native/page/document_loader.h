// Loads a document of the Page from the response of a navigation, the one
// that its Session makes when the Page opens or one that its document starts,
// on the Page program's main thread: a new context for it, which the Page's
// frame makes once the old document has unloaded (page_frame.h), and the
// document's parser (document_parser.h), which this feeds the body as the
// parser takes it, decoded with the encoding that Chrome works out for it
// (text_resource_decoder_builder.h). What arrives waits until the parser
// takes it, and the Session has no credit for it until then. A navigation
// that the document starts while it loads stops the parser, and no load
// event fires.
//
// Upstream: third_party/blink/renderer/core/loader/document_loader.cc

#ifndef MITTO_PAGE_DOCUMENT_LOADER_H_
#define MITTO_PAGE_DOCUMENT_LOADER_H_

#include <stdint.h>

#include <memory>
#include <string>

#include "base/containers/circular_deque.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "mitto/dom/document_parser.h"
#include "mitto/dom/fetcher.h"
#include "mitto/page/page_requests.h"
#include "url/gurl.h"
#include "v8/include/cppgc/persistent.h"

namespace mitto {

class Document;
class DocumentLoadTiming;
class PageFrame;
class TextResourceDecoder;

class DocumentLoader final : public PageRequests::Client,
                             public DocumentParser::Client {
 public:
  // Loads the response of navigation `number` into a document of `frame`.
  // What the Session sends of it runs in the frame's current window's context.
  DocumentLoader(PageRequests& requests, PageFrame& frame, uint64_t number);
  // The same for a navigation that the frame starts, whose number comes once
  // it has started: SetNumber().
  DocumentLoader(PageRequests& requests, PageFrame& frame);

  DocumentLoader(const DocumentLoader&) = delete;
  DocumentLoader& operator=(const DocumentLoader&) = delete;

  ~DocumentLoader() override;

  uint64_t number() const { return number_; }
  // Whether the navigation's response came after a redirect, which its
  // frame's commit reads.
  bool redirected() const { return redirected_; }
  // The response's status, and whether its Cache-Control says no-store, which
  // the back/forward cache reads when the document is left, as Chrome's
  // RenderFrameHost keeps them of its last navigation.
  int32_t status() const { return status_; }
  bool no_store() const { return no_store_; }
  void SetNumber(uint64_t number) { number_ = number; }
  // Makes this the loader of a response that the Page answers itself, a
  // data: URL's, of which the Session hears nothing: no credit and no cancel.
  void AnswerByPage() { answered_by_page_ = true; }
  // The referrer policy of a local URL's document, which it takes from the
  // initiator's policy container, as Chrome's browser gives it, unless its
  // response says otherwise.
  void SetInheritedReferrerPolicy(FetchReferrerPolicy policy) {
    inherited_referrer_policy_ = policy;
  }
  // Whether the document's embedder policy is compatible with cross-origin
  // isolation, for a local URL's document, which takes its initiator's.
  void SetEmbedderPolicyIsolates(bool isolates) {
    embedder_policy_isolates_ = isolates;
  }
  // For a data: URL's document: whether its initiator's document isolation
  // policy, which it takes, isolates it, as no Session says of it.
  void SetIsolatedByDocumentPolicy(bool isolated) {
    isolation_ = isolated ? PageCrossOriginIsolation::kDocumentIsolationPolicy
                          : PageCrossOriginIsolation::kNone;
  }
  // Whether the initiator's policy container has a Content-Security-Policy
  // that a header gave, which hides the nonces of the document's elements.
  void SetInheritedHeaderCsp(bool header_csp) {
    inherited_header_csp_ = header_csp;
  }
  bool answered_by_page() const { return answered_by_page_; }
  base::WeakPtr<DocumentLoader> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }
  // DocumentLoader::StopLoading(), once another document takes the place of
  // this one's: the parser stops, and so does the body, if it still comes.
  void StopLoading();

  // PageRequests::Client, for the navigation:
  void OnIsolation(const PageIsolation& isolation) override;
  void OnPermissionsPolicy(PermissionsPolicyHeaders headers) override;
  void OnResponseStarted(const PageResponseStarted& response) override;
  void OnResponseBody(std::string bytes) override;
  void OnResponseTotals(const PageResponseEnded& ended) override;
  // A navigation that ends with no response tells the frame, which lets this
  // go: nothing may come after it.
  void OnResponseEnded(int net_error) override;
  // A traversal's restore of a document that the back/forward cache keeps,
  // which the frame takes in place of a response, and which lets this go.
  void OnRestore() override;

  // DocumentParser::Client:
  bool FeedParser() override;
  void ParserDetached() override;

 private:
  // Gives the parser what the decoder decoded, after the document has taken
  // the decoder's encoding.
  void AppendDecoded(std::u16string text);
  // Tells the Session that the window's load event has been dispatched.
  void DidDispatchLoadEvent();
  // Tells the Session that the parser has taken `size` bytes of the body.
  void TookBody(size_t size);

  const raw_ref<PageRequests> requests_;
  const raw_ref<PageFrame> frame_;
  uint64_t number_;
  bool answered_by_page_ = false;
  FetchReferrerPolicy inherited_referrer_policy_ =
      FetchReferrerPolicy::kDefault;
  bool inherited_header_csp_ = false;
  bool response_started_ = false;
  // Whether the response came after a redirect.
  bool redirected_ = false;
  int32_t status_ = 0;
  bool no_store_ = false;
  // Whether the document of the response is cross-origin isolated, which
  // its window's agent is, as Blink's agent cluster key from the browser
  // says, and whether by its page's agent cluster or its own document
  // isolation policy.
  PageCrossOriginIsolation isolation_ = PageCrossOriginIsolation::kNone;
  PermissionsPolicyHeaders policy_headers_;
  // Whether its embedder policy is compatible with cross-origin isolation.
  bool embedder_policy_isolates_ = false;

  GURL url_;
  cppgc::Persistent<Document> document_;
  // The ID by which the Page names the document to its Session.
  uint64_t document_id_ = 0;
  // How the document loads, which its window's Performance Timeline reads.
  cppgc::Persistent<DocumentLoadTiming> timing_;
  // How many bytes of the body have come, as decoded as the Page gets them.
  uint64_t body_bytes_ = 0;
  // The document's parser, until it stops for good, as when the document has
  // loaded or document.open() has replaced it.
  cppgc::Persistent<DocumentParser> parser_;
  // Decodes the body as Blink's TextResourceDecoder does, with the encoding
  // that the response, the document or Chrome's detector gives.
  std::unique_ptr<TextResourceDecoder> decoder_;
  // Pieces of the body that have arrived and wait for the parser.
  base::circular_deque<std::string> waiting_body_;
  bool body_ended_ = false;
  // Whether the parser has had all of the body.
  bool data_finished_ = false;
  // Whether the document's load event has led to a favicon fetch.
  bool favicon_fetched_ = false;
  base::WeakPtrFactory<DocumentLoader> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_PAGE_DOCUMENT_LOADER_H_
