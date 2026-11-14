#include "mitto/dom/http_refresh_scheduler.h"

#include <stddef.h>

#include <cmath>
#include <limits>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/dom/document.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/window.h"
#include "mitto/dom/window_tasks.h"
#include "third_party/icu/source/common/unicode/uchar.h"
#include "url/url_constants.h"

namespace mitto {
namespace {

// What HttpRefreshScheduler drops rather than waits for.
constexpr base::TimeDelta kMaxScheduledDelay =
    base::Seconds(std::numeric_limits<int32_t>::max() / 1000);

bool IsHtmlSpace(char16_t c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

// http_parsers.cc's IsWhitespace(), for a header.
bool IsHeaderSpace(char16_t c) {
  return c == ' ' || c == '\t';
}

// WTF's IsSpaceOrNewline(), which StripWhiteSpace() strips.
bool IsSpaceOrNewline(char16_t c) {
  if (c <= 0x7f) {
    return c == ' ' || (c >= '\t' && c <= '\r');
  }
  return u_charDirection(c) == U_WHITE_SPACE_NEUTRAL;
}

std::u16string_view StripWhiteSpace(std::u16string_view text) {
  while (!text.empty() && IsSpaceOrNewline(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && IsSpaceOrNewline(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

// ParseRefreshTime(): digits and full stops only, read up to the second
// full stop, and floored to whole seconds. Nothing to read, as for "." alone,
// is no time.
std::optional<base::TimeDelta> ParseRefreshTime(std::u16string_view source) {
  size_t number_end = source.size();
  int full_stops = 0;
  for (size_t i = 0; i < source.size(); ++i) {
    if (source[i] == '.') {
      if (++full_stops == 2) {
        number_end = i;
      }
    } else if (source[i] < '0' || source[i] > '9') {
      return std::nullopt;
    }
  }
  const std::u16string_view number = source.substr(0, number_end);
  double whole = 0;
  bool has_digit = false;
  bool in_fraction = false;
  for (const char16_t c : number) {
    if (c == '.') {
      in_fraction = true;
      continue;
    }
    has_digit = true;
    if (!in_fraction) {
      whole = whole * 10 + (c - '0');
    }
  }
  if (!has_digit) {
    return std::nullopt;
  }
  return std::isfinite(whole) ? base::Seconds(whole) : base::TimeDelta::Max();
}

// KURL::ElidedString().
std::string Elided(const std::u16string& url) {
  const std::string spec = base::UTF16ToUTF8(url);
  if (url.size() <= 1024) {
    return spec;
  }
  return base::UTF16ToUTF8(
      base::StrCat({std::u16string_view(url).substr(0, 511), u"...",
                    std::u16string_view(url).substr(url.size() - 510)}));
}

}  // namespace

std::optional<HttpRefresh> ParseHttpRefresh(std::u16string_view content,
                                            bool html_white_space) {
  const auto is_space = html_white_space ? &IsHtmlSpace : &IsHeaderSpace;
  const size_t length = content.size();
  size_t pos = 0;
  const auto skip_white_space = [&] {
    while (pos < length && is_space(content[pos])) {
      ++pos;
    }
    return pos < length;
  };
  if (!skip_white_space()) {
    return std::nullopt;
  }
  while (pos != length && content[pos] != ',' && content[pos] != ';' &&
         !is_space(content[pos])) {
    ++pos;
  }
  const std::optional<base::TimeDelta> delay =
      ParseRefreshTime(StripWhiteSpace(content.substr(0, pos)));
  if (!delay) {
    return std::nullopt;
  }
  HttpRefresh refresh;
  refresh.delay = *delay;
  if (pos == length) {
    return refresh;
  }
  skip_white_space();
  if (pos < length && (content[pos] == ',' || content[pos] == ';')) {
    ++pos;
  }
  skip_white_space();
  std::u16string_view url = content.substr(pos);
  // "url=", but for one with no "=", whose URL is all of it, "url" too.
  if (url.size() >= 3 &&
      base::EqualsCaseInsensitiveASCII(url.substr(0, 3), u"url")) {
    const size_t prefix_start = pos;
    pos += 3;
    skip_white_space();
    if (pos < length && content[pos] == '=') {
      ++pos;
      skip_white_space();
      url.remove_prefix(pos - prefix_start);
    }
  }
  // A URL in quotes ends at the last of its quotation mark, or else the end.
  if (!url.empty() && (url.front() == '"' || url.front() == '\'')) {
    const char16_t quotation_mark = url.front();
    url.remove_prefix(1);
    const size_t end = url.rfind(quotation_mark);
    if (end != std::u16string_view::npos) {
      url = url.substr(0, end);
    }
  }
  refresh.url = std::u16string(StripWhiteSpace(url));
  return refresh;
}

HttpRefreshScheduler::HttpRefreshScheduler(Document& document)
    : document_(document) {}

HttpRefreshScheduler::~HttpRefreshScheduler() = default;

void HttpRefreshScheduler::MaybeHandle(std::u16string_view content,
                                       Source source) {
  Window* const window = document_->DefaultView();
  if (!window) {
    return;
  }
  const std::optional<HttpRefresh> parsed =
      ParseHttpRefresh(content, source == Source::kMetaTag);
  if (!parsed) {
    return;
  }
  const GURL url = !parsed->url || parsed->url->empty()
                       ? GURL(document_->UrlForBinding())
                       : document_->CompleteUrl(*parsed->url);
  if (url.SchemeIs(url::kJavaScriptScheme)) {
    window->AddConsoleMessage(
        ConsoleLevel::kError,
        base::StrCat({"Refused to refresh ", Elided(document_->UrlForBinding()),
                      " to a javascript: URL"}));
    return;
  }
  Schedule(parsed->delay, url,
           source == Source::kHeader
               ? ClientNavigationReason::kHttpHeaderRefresh
               : ClientNavigationReason::kMetaTagRefresh);
}

void HttpRefreshScheduler::Schedule(base::TimeDelta delay,
                                    const GURL& url,
                                    ClientNavigationReason reason) {
  // A refresh that unload or its kin schedules, while no navigation starts.
  if (document_->PageDismissalEventBeingDispatched()) {
    return;
  }
  if (delay.is_negative() || delay > kMaxScheduledDelay) {
    return;
  }
  if (url.possibly_invalid_spec().empty()) {
    return;
  }
  if (refresh_ && refresh_->delay < delay) {
    return;
  }
  Cancel();
  refresh_ = Refresh{delay, url, reason};
  MaybeStartTimer();
}

void HttpRefreshScheduler::MaybeStartTimer() {
  if (!refresh_ || timer_active_ || !document_->LoadEventFinished()) {
    return;
  }
  Window* const window = document_->DefaultView();
  if (!window || !window->GetTasks()) {
    return;
  }
  timer_active_ = true;
  window->GetTasks()->PostDelayedTask(
      TaskType::kInternalLoading,
      base::BindOnce(&HttpRefreshScheduler::NavigateTask,
                     weak_factory_.GetWeakPtr()),
      refresh_->delay);
}

void HttpRefreshScheduler::Cancel() {
  weak_factory_.InvalidateWeakPtrs();
  timer_active_ = false;
  refresh_.reset();
}

void HttpRefreshScheduler::NavigateTask() {
  const Refresh refresh = *std::move(refresh_);
  refresh_.reset();
  timer_active_ = false;
  Window* const window = document_->DefaultView();
  if (!window) {
    return;
  }
  // A refresh to the document's own URL reloads it, but for one to a
  // fragment, which StartNavigation() keeps within the document; one of a
  // delay of at most a second replaces the entry.
  FrameLoadType type = FrameLoadType::kStandard;
  if (GURL(document_->UrlForBinding()).GetWithoutRef() ==
          refresh.url.GetWithoutRef() &&
      !document_->IsInitialEmptyDocument()) {
    if (!refresh.url.has_ref()) {
      type = FrameLoadType::kReload;
    }
  } else if (refresh.delay <= base::Seconds(1)) {
    type = FrameLoadType::kReplaceCurrentItem;
  }
  FrameLoadRequest request;
  request.url = refresh.url;
  request.reason = refresh.reason;
  StartNavigation(*window, request, type);
}

}  // namespace mitto
