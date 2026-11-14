#include "mitto/page/document_loader.h"

#include <memory>
#include <utility>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "mitto/dom/document.h"
#include "mitto/dom/document_load_timing.h"
#include "mitto/dom/window.h"
#include "mitto/encoding/content_type.h"
#include "mitto/encoding/text_resource_decoder.h"
#include "mitto/encoding/text_resource_decoder_builder.h"
#include "mitto/header_rules.h"
#include "mitto/page/resource_timing.h"
#include "mitto/page/script_runner.h"

namespace mitto {

DocumentLoader::DocumentLoader(ScriptRunner& runner,
                               PageRequests& requests,
                               SendFrame send)
    : runner_(runner), requests_(requests), send_(std::move(send)) {}

DocumentLoader::~DocumentLoader() = default;

void DocumentLoader::OnResponseStarted(const PageResponseStarted& response) {
  // The parser of a document that the window had goes, and what waited for
  // it of the body before.
  waiting_body_.clear();
  if (parser_) {
    parser_->Detach();
  }
  body_bytes_ = 0;
  body_ended_ = false;
  data_finished_ = false;
  // The document of the response takes the place of about:blank's, in a
  // context of its own, as Blink makes a new window for a tab's first
  // navigation, whose about:blank has an opaque origin.
  url_ = GURL(response.url);
  // The Session's base::TimeTicks, which the Page's clock shares.
  document_ = &runner_->CommitDocument(
      url_, base::TimeTicks() + base::Microseconds(response.navigation_start),
      response.connection);
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
  const MimeTypeAndCharset content_type =
      GetMimeTypeAndCharset(HeaderField(response.headers, "content-type"));
  decoder_ = BuildTextResourceDecoder(
      url_,
      content_type.mime_type.empty() ? "text/html" : content_type.mime_type,
      content_type.charset, document_->DefaultView()->Start().locale,
      /*parent=*/nullptr);
  document_->SetDidDispatchLoadEvent(base::BindRepeating(
      &DocumentLoader::DidDispatchLoadEvent, weak_factory_.GetWeakPtr()));
  // Document::OpenForNavigation(), with the document loading.
  parser_ = &document_->OpenForNavigation(*this);
}

void DocumentLoader::AppendDecoded(std::u16string text) {
  // The document's encoding is the decoder's after each piece, as
  // DecodedDataDocumentParser::UpdateDocument() sets it.
  document_->SetEncoding(decoder_->encoding(),
                         decoder_->EncodingWasDetectedHeuristically());
  parser_->AppendData(text);
}

void DocumentLoader::OnResponseBody(std::string bytes) {
  body_bytes_ += bytes.size();
  if (!parser_) {
    // What comes once the parser has stopped for good goes unread.
    requests_->TookBody(kNavigationRequest, bytes.size());
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
    AppendDecoded(decoder_->Decode(base::as_byte_span(bytes)));
    requests_->TookBody(kNavigationRequest, bytes.size());
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
    requests_->TookBody(kNavigationRequest, bytes.size());
  }
  waiting_body_.clear();
}

void DocumentLoader::DidDispatchLoadEvent() {
  send_.Run(EncodeLoaded());
}

}  // namespace mitto
