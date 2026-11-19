#include "mitto/dom/fetcher.h"

#include <algorithm>
#include <utility>

#include "base/strings/string_util.h"

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

}  // namespace mitto
