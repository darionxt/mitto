#include "mitto/dom/fetcher.h"

#include <algorithm>
#include <utility>

#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/header_rules.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "url/url_constants.h"

namespace mitto {

std::optional<FetchPriority> ComputeFetchPriority(bool lazy,
                                                  FetchPriorityHint hint,
                                                  bool render_blocking) {
  FetchPriority priority = lazy ? FetchPriority::kLow : FetchPriority::kHigh;
  switch (hint) {
    case FetchPriorityHint::kAuto:
      break;
    case FetchPriorityHint::kHigh:
      priority = std::max(priority, FetchPriority::kHigh);
      break;
    case FetchPriorityHint::kLow:
      priority = std::min(priority, FetchPriority::kLow);
      break;
  }
  if (render_blocking) {
    priority = std::max(priority, FetchPriority::kHigh);
  }
  if (priority == FetchPriority::kHigh) {
    return std::nullopt;
  }
  return priority;
}

FetchPriorityHint FetchPriorityHintFromAttribute(std::u16string_view value) {
  if (base::EqualsCaseInsensitiveASCII(value, u"high")) {
    return FetchPriorityHint::kHigh;
  }
  if (base::EqualsCaseInsensitiveASCII(value, u"low")) {
    return FetchPriorityHint::kLow;
  }
  return FetchPriorityHint::kAuto;
}

FetchReferrerPolicy FetchReferrerPolicyFromString(std::u16string_view value) {
  static constexpr std::pair<std::u16string_view, FetchReferrerPolicy>
      kPolicies[] = {
          {u"no-referrer", FetchReferrerPolicy::kNoReferrer},
          {u"no-referrer-when-downgrade",
           FetchReferrerPolicy::kNoReferrerWhenDowngrade},
          {u"origin", FetchReferrerPolicy::kOrigin},
          {u"origin-when-cross-origin",
           FetchReferrerPolicy::kOriginWhenCrossOrigin},
          {u"same-origin", FetchReferrerPolicy::kSameOrigin},
          {u"strict-origin", FetchReferrerPolicy::kStrictOrigin},
          {u"strict-origin-when-cross-origin",
           FetchReferrerPolicy::kStrictOriginWhenCrossOrigin},
          {u"unsafe-url", FetchReferrerPolicy::kUnsafeUrl},
      };
  for (const auto& [name, policy] : kPolicies) {
    if (base::EqualsCaseInsensitiveASCII(value, name)) {
      return policy;
    }
  }
  return FetchReferrerPolicy::kDefault;
}

bool IsContentTypeOptionsNosniff(std::string_view headers) {
  const std::string field = HeaderField(headers, "x-content-type-options");
  const std::string_view first = std::string_view(field).substr(
      0, std::min(field.find(','), field.size()));
  return base::EqualsCaseInsensitiveASCII(
      base::TrimString(first, " \t", base::TRIM_ALL), "nosniff");
}

FetchReferrerPolicy FetchReferrerPolicyFromHeaderValue(std::string_view value) {
  FetchReferrerPolicy policy = FetchReferrerPolicy::kDefault;
  for (const std::string_view token : base::SplitStringPiece(
           value, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL)) {
    const FetchReferrerPolicy named =
        FetchReferrerPolicyFromString(base::UTF8ToUTF16(token));
    if (named != FetchReferrerPolicy::kDefault) {
      policy = named;
    }
  }
  return policy;
}

FetchReferrerPolicy FetchReferrerPolicyFromStringWithLegacyKeywords(
    std::u16string_view value) {
  if (base::EqualsCaseInsensitiveASCII(value, u"never") ||
      base::EqualsCaseInsensitiveASCII(value, u"none")) {
    return FetchReferrerPolicy::kNoReferrer;
  }
  if (base::EqualsCaseInsensitiveASCII(value, u"always")) {
    return FetchReferrerPolicy::kUnsafeUrl;
  }
  if (base::EqualsCaseInsensitiveASCII(value, u"origin-when-crossorigin")) {
    return FetchReferrerPolicy::kOriginWhenCrossOrigin;
  }
  // The default policy of Chrome's network service.
  if (base::EqualsCaseInsensitiveASCII(value, u"default")) {
    return FetchReferrerPolicy::kStrictOriginWhenCrossOrigin;
  }
  return FetchReferrerPolicyFromString(value);
}

GURL GenerateReferrer(FetchReferrerPolicy policy,
                      const GURL& url,
                      const GURL& referrer) {
  if (!referrer.is_valid()) {
    return GURL();
  }
  // Its origin, with no path or query, and the whole URL, but for one longer
  // than 4096 characters, which gives its origin.
  const GURL origin = referrer.DeprecatedGetOriginAsURL();
  const GURL& full = referrer.spec().size() > 4096 ? origin : referrer;
  const bool same_origin = url::Origin::Create(full).IsSameOriginWith(url);
  // SecurityPolicy::ShouldHideReferrer(): an HTTPS referrer goes only to an
  // HTTPS URL.
  const bool hide =
      full.SchemeIs(url::kHttpsScheme) && !url.SchemeIs(url::kHttpsScheme);
  switch (policy) {
    case FetchReferrerPolicy::kNoReferrer:
      return GURL();
    case FetchReferrerPolicy::kUnsafeUrl:
      return full;
    case FetchReferrerPolicy::kOrigin:
      return origin;
    case FetchReferrerPolicy::kOriginWhenCrossOrigin:
      return same_origin ? full : origin;
    case FetchReferrerPolicy::kSameOrigin:
      return same_origin ? full : GURL();
    case FetchReferrerPolicy::kStrictOrigin:
      return hide ? GURL() : origin;
    case FetchReferrerPolicy::kDefault:
    case FetchReferrerPolicy::kStrictOriginWhenCrossOrigin:
      if (!same_origin) {
        return hide ? GURL() : origin;
      }
      break;
    case FetchReferrerPolicy::kNoReferrerWhenDowngrade:
      break;
  }
  return hide ? GURL() : full;
}

CrossOriginAttribute CrossOriginAttributeFromValue(
    const std::optional<std::u16string>& value) {
  if (!value) {
    return CrossOriginAttribute::kNotSet;
  }
  return base::EqualsCaseInsensitiveASCII(*value, u"use-credentials")
             ? CrossOriginAttribute::kUseCredentials
             : CrossOriginAttribute::kAnonymous;
}

FetchRequestData::FetchRequestData() = default;
FetchRequestData::FetchRequestData(FetchRequestData&&) = default;
FetchRequestData& FetchRequestData::operator=(FetchRequestData&&) = default;
FetchRequestData::~FetchRequestData() = default;

FetchResponseHead::FetchResponseHead() = default;
FetchResponseHead::FetchResponseHead(const FetchResponseHead&) = default;
FetchResponseHead& FetchResponseHead::operator=(const FetchResponseHead&) =
    default;
FetchResponseHead::~FetchResponseHead() = default;

size_t FetchController::BodyRoom() const {
  return 0;
}

}  // namespace mitto
