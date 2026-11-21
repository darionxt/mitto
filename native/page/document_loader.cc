#include "mitto/page/document_loader.h"

#include <memory>
#include <string>
#include <utility>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "mitto/dom/document.h"
#include "mitto/dom/document_load_timing.h"
#include "mitto/dom/http_refresh_scheduler.h"
#include "mitto/dom/page_reveal_event.h"
#include "mitto/dom/window.h"
#include "mitto/encoding/content_type.h"
#include "mitto/encoding/text_resource_decoder.h"
#include "mitto/encoding/text_resource_decoder_builder.h"
#include "mitto/header_rules.h"
#include "mitto/mime_types.h"
#include "mitto/page/page_frame.h"
#include "mitto/page/resource_timing.h"
#include "mitto/page/script_runner.h"

namespace mitto {


DocumentLoader::DocumentLoader(ScriptRunner& runner,
                               PageRequests& requests,
                               SendFrame send,
                               PageFrame& frame,
                               uint64_t number)
    : runner_(runner),
      requests_(requests),
      send_(std::move(send)),
      frame_(frame),
      number_(number) {}

DocumentLoader::DocumentLoader(ScriptRunner& runner,
                               PageRequests& requests,
                               SendFrame send,
                               PageFrame& frame)
    : DocumentLoader(runner, requests, std::move(send), frame, 0) {}

DocumentLoader::~DocumentLoader() {
  // The parser holds this as its client, so it stops for good first.
  waiting_body_.clear();
  if (parser_) {
    const cppgc::Persistent<DocumentParser> parser = parser_;
    parser->Detach();
  }
}

void DocumentLoader::StopLoading() {
  // The parser stops for good, and the body too, if it still comes.
  if (parser_) {
    const cppgc::Persistent<DocumentParser> parser = parser_;
    parser->Detach();
  }
  if (response_started_ && !body_ended_) {
    body_ended_ = true;
    requests_->Cancel(number_);
  }
}

void DocumentLoader::OnResponseStarted(const PageResponseStarted& response) {
  response_started_ = true;
  // The document of the response takes the place of the frame's, in a
  // context of its own, once the old one has unloaded, as Blink makes a new
  // window for a tab's first navigation, whose about:blank has an opaque
  // origin, and for each later one.
  url_ = GURL(response.url);
  const MimeTypeAndCharset content_type =
      GetMimeTypeAndCharset(HeaderField(response.headers, "content-type"));
  std::string mime_type =
      content_type.mime_type.empty() ? "text/html" : content_type.mime_type;
  // URLLoader::StartReading(): Chrome's network service gives a feed's
  // response as text/plain.
  if (mime_type == "application/rss+xml" ||
      mime_type == "application/atom+xml") {
    mime_type = "text/plain";
  }
  const DocumentKind document_kind = DocumentKindOf(mime_type);
  // An XML, XHTML or SVG response makes an XMLDocument of its MIME type, as
  // Blink's DocumentInit does, which the XML parser reads.
  const bool xml = document_kind == DocumentKind::kXhtml ||
                   document_kind == DocumentKind::kSvg ||
                   document_kind == DocumentKind::kXml;
  // The Session's base::TimeTicks, which the Page's clock shares.
  document_ = &frame_->CommitNavigation(
      *this, url_,
      base::TimeTicks() + base::Microseconds(response.navigation_start),
      response.connection,
      xml ? std::optional<std::u16string>(base::ASCIIToUTF16(mime_type))
          : std::nullopt);
  document_id_ = document_->DefaultView()->DocumentId();
  // The Referer that the navigation's last hop sent, as //net kept it through
  // its redirects.
  document_->SetReferrer(base::UTF8ToUTF16(response.referrer));
  // What DocumentLoader::CommitNavigation() takes from the navigation's
  // timings and response, for the window's Performance Timeline and
  // window.chrome. A restart for Critical-CH is a redirect to where the
  // navigation started, after which the fetch starts again.
  timing_ = &runner_->document_load_timing();
  DocumentLoadTiming& timing = *timing_;
  timing.has_response = true;
  timing.redirect_count = static_cast<uint16_t>(
      response.redirect_count + (response.restarted_for_critical_ch ? 1 : 0));
  timing.has_cross_origin_redirect = response.cross_origin_redirect;
  timing.fetch_start = TicksOf(response.timing.request_start);
  if (timing.redirect_count > 0) {
    timing.redirect_start = TicksOf(response.first_request_start);
    timing.redirect_end = timing.fetch_start;
  }
  timing.critical_ch_restart = TicksOf(response.critical_ch_restart);
  // CreateNavigationTimingInstance(): no start time, the document's URL, and
  // a response that always passes the TAO check, whose cache state the bytes
  // that had come with its headers decide.
  PageResponseStarted navigation_response = response;
  navigation_response.timing_allow_passed = true;
  timing.navigation = CreateResourceTimingInfo(
      base::TimeTicks(), base::UTF8ToUTF16(response.url), &navigation_response,
      base::TimeTicks::Now());
  timing.navigation.cache_state = response.validated ? CacheState::kValidated
                                  : response.received_bytes == 0
                                      ? CacheState::kLocal
                                      : CacheState::kNone;
  timing.connection_info = response.connection_info;
  timing.alpn_negotiated_protocol = response.alpn_negotiated_protocol;
  timing.was_fetched_via_spdy = response.was_fetched_via_spdy;
  timing.was_alpn_negotiated = response.was_alpn_negotiated;
  document_->SetLoadTiming(timing_);
  // Blink's Document::lastModifiedTime(): the response's Last-Modified, where
  // it parses.
  const std::string last_modified =
      HeaderField(response.headers, "last-modified");
  base::Time last_modified_time;
  if (!last_modified.empty() &&
      base::Time::FromString(last_modified.c_str(), &last_modified_time)) {
    document_->SetLastModified(last_modified_time);
  }
  // The decoder that DocumentLoader makes at commit, for a top-level
  // document of a browser in the Persona's locale. A response with no MIME
  // type is HTML here, as the parser reads it, where Chrome's network service
  // sniffs one.
  decoder_ = BuildTextResourceDecoder(url_, mime_type, content_type.charset,
                                      document_->DefaultView()->Start().locale,
                                      /*parent=*/nullptr);
  document_->SetDidDispatchLoadEvent(base::BindRepeating(
      &DocumentLoader::DidDispatchLoadEvent, weak_factory_.GetWeakPtr()));
  // DocumentInit::CreateDocument(): the kind of document of the MIME type,
  // whose contentType it is. A text, JSON, image or media document stays in
  // no-quirks mode.
  DocumentParser::Kind parser_kind = DocumentParser::Kind::kHtml;
  switch (document_kind) {
    case DocumentKind::kXhtml:
    case DocumentKind::kSvg:
    case DocumentKind::kXml:
      // Its XMLDocument has the XML parser (Document::ImplicitOpen()).
      break;
    case DocumentKind::kHtml:
      document_->SetContentType(base::ASCIIToUTF16(mime_type));
      break;
    case DocumentKind::kText:
      parser_kind = DocumentParser::Kind::kText;
      break;
    case DocumentKind::kJson:
      parser_kind = DocumentParser::Kind::kJson;
      break;
    case DocumentKind::kImage:
      parser_kind = DocumentParser::Kind::kImage;
      break;
    case DocumentKind::kMedia:
      parser_kind = DocumentParser::Kind::kMedia;
      break;
  }
  if (parser_kind != DocumentParser::Kind::kHtml) {
    document_->SetContentType(base::ASCIIToUTF16(mime_type));
    document_->SetCompatibilityMode(Document::CompatibilityMode::kNoQuirks);
    document_->LockCompatibilityMode();
  }
  // Document::OpenForNavigation(), with the document loading.
  parser_ = &document_->OpenForNavigation(*this, parser_kind);
  // DocumentLoader::DidCommitNavigation(): the first language of the
  // response's Content-Language, without HTML spaces. A header's value is
  // Latin-1.
  const std::string content_language =
      HeaderField(response.headers, "content-language");
  const std::string_view first_language = base::TrimString(
      std::string_view(content_language).substr(0, content_language.find(',')),
      " \t\n\f\r", base::TRIM_ALL);
  if (!first_language.empty()) {
    std::u16string latin1;
    latin1.reserve(first_language.size());
    for (const char c : first_language) {
      latin1.push_back(static_cast<unsigned char>(c));
    }
    document_->SetContentLanguage(std::move(latin1));
  }
  // Whether the response delivered a Content Security Policy: a policy for
  // each part of either header between commas that is not empty, as the
  // network service parses them, which hides nonces.
  for (const char* const name :
       {"content-security-policy", "content-security-policy-report-only"}) {
    const std::string policies = HeaderField(response.headers, name);
    if (!base::SplitStringPiece(policies, ",", base::TRIM_WHITESPACE,
                                base::SPLIT_WANT_NONEMPTY)
             .empty()) {
      document_->SetHasHeaderDeliveredContentSecurityPolicy(true);
    }
  }
  // The response's Referrer-Policy, which the browser's policy container
  // gives the document as it commits.
  const std::string referrer_policy =
      HeaderField(response.headers, "referrer-policy");
  if (!referrer_policy.empty()) {
    document_->ParseAndSetReferrerPolicy(
        base::UTF8ToUTF16(referrer_policy),
        Document::ReferrerPolicySource::kHttpHeader);
  }
  // DocumentLoader::CreateParserPostCommit(): the response's Refresh, before
  // anything of the body. A header's value is Latin-1.
  const std::string refresh = HeaderField(response.headers, "refresh");
  if (!refresh.empty()) {
    std::u16string latin1;
    latin1.reserve(refresh.size());
    for (const char c : refresh) {
      latin1.push_back(static_cast<unsigned char>(c));
    }
    document_->GetHttpRefreshScheduler().MaybeHandle(
        latin1, HttpRefreshScheduler::Source::kHeader);
  }
  // Document::EnqueuePageRevealEvent(): pagereveal at the window, at the
  // first rendering opportunity, before that frame's callbacks, which comes
  // once the document is ready to render: as the parser inserts its body.
  Window& window = *document_->DefaultView();
  window.DeferLifecycleUpdates();
  window.EnqueueAnimationFrameEvent(window, *PageRevealEvent::CreateTrusted());
}

void DocumentLoader::AppendDecoded(std::u16string text) {
  // The document's encoding is the decoder's after each piece, as
  // DecodedDataDocumentParser::UpdateDocument() sets it.
  document_->SetEncoding(decoder_->encoding(),
                         decoder_->EncodingWasDetectedHeuristically(),
                         decoder_->saw_error());
  parser_->AppendData(text);
}

void DocumentLoader::OnResponseBody(std::string bytes) {
  body_bytes_ += bytes.size();
  if (!parser_) {
    // What comes once the parser has stopped for good goes unread.
    requests_->TookBody(number_, bytes.size());
    return;
  }
  waiting_body_.push_back(std::move(bytes));
  parser_->DidReceiveData();
}

void DocumentLoader::OnResponseTotals(const PageResponseEnded& ended) {
  // WindowPerformance::OnBodyLoadFinished() and FinishedLoading(): the body's
  // sizes, which the navigation's entry shows, and when it ended.
  if (!timing_) {
    return;
  }
  timing_->navigation.encoded_body_size =
      static_cast<uint64_t>(ended.encoded_body_size);
  timing_->navigation.decoded_body_size = body_bytes_;
  timing_->response_end = ended.completion_time != 0
                              ? TicksOf(ended.completion_time)
                              : base::TimeTicks::Now();
}

void DocumentLoader::OnResponseEnded(int net_error) {
  if (!response_started_) {
    // No document: a 204, a download, a failure, or another navigation that
    // took its place. The frame keeps its document and lets this go.
    frame_->NavigationEndedWithoutDocument(*this);
    return;
  }
  // After a response, a body that stops short still makes a document of what
  // came, as in Chrome: HTMLDocumentParser::Finish().
  body_ended_ = true;
  if (parser_) {
    const cppgc::Persistent<DocumentParser> parser = parser_;
    parser->Finish();
  }
}

bool DocumentLoader::FeedParser() {
  if (!waiting_body_.empty()) {
    const std::string bytes = std::move(waiting_body_.front());
    waiting_body_.pop_front();
    if (!bytes.empty()) {
      parser_->AppendBytes(bytes);
    }
    // Starting the tree may have stopped the parser.
    if (parser_) {
      AppendDecoded(decoder_->Decode(base::as_byte_span(bytes)));
    }
    requests_->TookBody(number_, bytes.size());
    return true;
  }
  if (body_ended_ && !data_finished_) {
    data_finished_ = true;
    AppendDecoded(decoder_->Flush());
    parser_->FinishData();
    return true;
  }
  return false;
}

void DocumentLoader::ParserDetached() {
  parser_ = nullptr;
  // What waits of the body goes unread.
  for (const std::string& bytes : waiting_body_) {
    requests_->TookBody(number_, bytes.size());
  }
  waiting_body_.clear();
}

void DocumentLoader::DidDispatchLoadEvent() {
  send_.Run(EncodeLoaded(document_id_));
}

}  // namespace mitto
