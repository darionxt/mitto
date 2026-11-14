#include "mitto/dom/url.h"

#include <string_view>

#include "base/auto_reset.h"
#include "base/notreached.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_url.h"
#include "mitto/dom/url_search_params.h"

namespace mitto {

namespace {

// The query of `url`, without its "?".
std::u16string QueryOf(const ResolvedURL& url) {
  std::u16string query = url.QueryWithLeadingQuestionMark();
  if (!query.empty()) {
    query.erase(0, 1);
  }
  return query;
}

}  // namespace

// static
URL* URL::Create(const std::u16string& url,
                 const std::optional<std::u16string>& base,
                 bindings::ExceptionState& exception_state) {
  ResolvedURL base_url;
  if (base) {
    base_url = ResolvedURL(*base);
    if (!base_url.IsValid()) {
      exception_state.ThrowTypeError("Invalid base URL");
      return nullptr;
    }
  }
  const ResolvedURL resolved(base_url, url);
  if (!resolved.IsValid()) {
    exception_state.ThrowTypeError("Invalid URL");
    return nullptr;
  }
  return bindings::MakeGarbageCollected<URL>(resolved);
}

// static
URL* URL::Parse(const std::u16string& url,
                const std::optional<std::u16string>& base) {
  ResolvedURL base_url;
  if (base) {
    base_url = ResolvedURL(*base);
    if (!base_url.IsValid()) {
      return nullptr;
    }
  }
  const ResolvedURL resolved(base_url, url);
  if (!resolved.IsValid()) {
    return nullptr;
  }
  return bindings::MakeGarbageCollected<URL>(resolved);
}

// static
bool URL::CanParse(const std::u16string& url,
                   const std::optional<std::u16string>& base) {
  ResolvedURL base_url;
  if (base) {
    base_url = ResolvedURL(*base);
    if (!base_url.IsValid()) {
      return false;
    }
  }
  return ResolvedURL(base_url, url).IsValid();
}

URL::URL(const ResolvedURL& url) : url_(url) {}

URL::~URL() = default;

const bindings::WrapperTypeInfo* URL::GetWrapperTypeInfo() const {
  return &bindings::V8URL::kWrapperTypeInfo;
}

void URL::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(search_params_);
  bindings::ScriptWrappable::Trace(visitor);
}

ResolvedURL URL::Url() const {
  return url_;
}

void URL::SetUrl(const ResolvedURL& url) {
  url_ = url;
}

std::u16string URL::Input() const {
  // A URL object always has a URL.
  NOTREACHED();
}

void URL::SetHref(const std::u16string& value,
                  bindings::ExceptionState& exception_state) {
  const ResolvedURL url(value);
  if (!url.IsValid()) {
    exception_state.ThrowTypeError("Invalid URL");
    return;
  }
  url_ = url;
  Update();
}

void URL::SetSearch(const std::u16string& value) {
  UrlUtils::SetSearch(value);
  std::u16string_view query(value);
  if (query.starts_with(u'?')) {
    query.remove_prefix(1);
  }
  UpdateSearchParams(query);
}

URLSearchParams* URL::SearchParams() {
  if (!search_params_) {
    search_params_ = URLSearchParams::Create(QueryOf(url_), this);
  }
  return search_params_.Get();
}

void URL::Update() {
  UpdateSearchParams(QueryOf(url_));
}

void URL::UpdateSearchParams(std::u16string_view query) {
  if (!search_params_) {
    return;
  }
  const base::AutoReset<bool> in_update(&is_in_update_, true);
  search_params_->SetInputWithoutUpdate(query);
}

}  // namespace mitto
