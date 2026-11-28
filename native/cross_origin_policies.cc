#include "mitto/cross_origin_policies.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include "net/base/is_potentially_trustworthy.h"
#include "net/http/structured_headers.h"

namespace mitto {

namespace {

// cross_origin_embedder_policy_parser.cc's Parse().
std::pair<CrossOriginEmbedderPolicyValue, std::optional<std::string>>
ParseEmbedderPolicy(std::string_view header_value) {
  auto item = net::structured_headers::ParseItem(header_value);
  const std::string* token = item ? item->item.GetIfToken() : nullptr;
  if (!token) {
    return {CrossOriginEmbedderPolicyValue::kNone, std::nullopt};
  }
  std::optional<std::string> endpoint;
  for (auto& [key, value] : item->params) {
    if (std::string* str = value.GetIfString(); key == "report-to" && str) {
      endpoint = std::move(*str);
    }
  }
  if (*token == "require-corp") {
    return {CrossOriginEmbedderPolicyValue::kRequireCorp, std::move(endpoint)};
  }
  if (*token == "credentialless") {
    return {CrossOriginEmbedderPolicyValue::kCredentialless,
            std::move(endpoint)};
  }
  return {CrossOriginEmbedderPolicyValue::kNone, std::nullopt};
}

// document_isolation_policy_parser.cc's Parse(): a token of an item, with a
// report-to string, as COEP's.
std::pair<DocumentIsolationPolicyValue, std::optional<std::string>>
ParseIsolationPolicy(std::string_view header_value) {
  auto item = net::structured_headers::ParseItem(header_value);
  const std::string* token = item ? item->item.GetIfToken() : nullptr;
  if (!token) {
    return {DocumentIsolationPolicyValue::kNone, std::nullopt};
  }
  std::optional<std::string> endpoint;
  for (auto& [key, value] : item->params) {
    if (std::string* str = value.GetIfString(); key == "report-to" && str) {
      endpoint = std::move(*str);
    }
  }
  if (*token == "isolate-and-require-corp") {
    return {DocumentIsolationPolicyValue::kIsolateAndRequireCorp,
            std::move(endpoint)};
  }
  if (*token == "isolate-and-credentialless") {
    return {DocumentIsolationPolicyValue::kIsolateAndCredentialless,
            std::move(endpoint)};
  }
  return {DocumentIsolationPolicyValue::kNone, std::nullopt};
}

// cross_origin_opener_policy_parser.cc's ParseHeader(): fills `value`,
// `endpoint` and, if given, `soap_by_default_value` from a header, and leaves
// them for one that is not valid.
void ParseOpenerPolicy(std::string_view header_value,
                       CrossOriginOpenerPolicyValue& value,
                       CrossOriginOpenerPolicyValue* soap_by_default_value,
                       std::optional<std::string>& endpoint) {
  auto item = net::structured_headers::ParseItem(header_value);
  if (!item) {
    return;
  }
  const std::string* policy_item = item->item.GetIfToken();
  if (!policy_item) {
    return;
  }
  std::optional<CrossOriginOpenerPolicyValue> parsed;
  if (*policy_item == "same-origin") {
    parsed = CrossOriginOpenerPolicyValue::kSameOrigin;
  } else if (*policy_item == "same-origin-allow-popups") {
    parsed = CrossOriginOpenerPolicyValue::kSameOriginAllowPopups;
  } else if (*policy_item == "unsafe-none") {
    parsed = CrossOriginOpenerPolicyValue::kUnsafeNone;
  } else if (*policy_item == "noopener-allow-popups") {
    // features::kCoopNoopenerAllowPopups is on by default.
    parsed = CrossOriginOpenerPolicyValue::kNoopenerAllowPopups;
  }
  if (parsed) {
    value = *parsed;
    if (soap_by_default_value) {
      *soap_by_default_value = *parsed;
    }
  }
  const auto it = std::ranges::find(
      item->params, "report-to",
      &std::pair<std::string, net::structured_headers::Item>::first);
  if (it == item->params.end()) {
    return;
  }
  if (std::string* str = it->second.GetIfString()) {
    endpoint = std::move(*str);
  }
}

}  // namespace

bool CompatibleWithCrossOriginIsolated(CrossOriginEmbedderPolicyValue value) {
  switch (value) {
    case CrossOriginEmbedderPolicyValue::kNone:
      return false;
    case CrossOriginEmbedderPolicyValue::kCredentialless:
    case CrossOriginEmbedderPolicyValue::kRequireCorp:
      return true;
  }
}

CrossOriginEmbedderPolicy ParseCrossOriginEmbedderPolicy(
    const std::optional<std::string>& header,
    const std::optional<std::string>& report_only_header) {
  CrossOriginEmbedderPolicy coep;
  if (header) {
    std::tie(coep.value, coep.reporting_endpoint) =
        ParseEmbedderPolicy(*header);
  }
  if (report_only_header) {
    std::tie(coep.report_only_value, coep.report_only_reporting_endpoint) =
        ParseEmbedderPolicy(*report_only_header);
  }
  return coep;
}

CrossOriginOpenerPolicy ParseCrossOriginOpenerPolicy(
    const std::optional<std::string>& header,
    const std::optional<std::string>& report_only_header) {
  // features::kCrossOriginOpenerPolicy is on by default, and
  // features::kCrossOriginOpenerPolicyByDefault off.
  CrossOriginOpenerPolicy coop;
  coop.soap_by_default_value =
      CrossOriginOpenerPolicyValue::kSameOriginAllowPopups;
  if (header) {
    ParseOpenerPolicy(*header, coop.value, &coop.soap_by_default_value,
                      coop.reporting_endpoint);
  }
  if (report_only_header) {
    ParseOpenerPolicy(*report_only_header, coop.report_only_value,
                      /*soap_by_default_value=*/nullptr,
                      coop.report_only_reporting_endpoint);
  }
  return coop;
}

DocumentIsolationPolicy ParseDocumentIsolationPolicy(
    const std::optional<std::string>& header,
    const std::optional<std::string>& report_only_header) {
  DocumentIsolationPolicy dip;
  if (header) {
    std::tie(dip.value, dip.reporting_endpoint) = ParseIsolationPolicy(*header);
  }
  if (report_only_header) {
    std::tie(dip.report_only_value, dip.report_only_reporting_endpoint) =
        ParseIsolationPolicy(*report_only_header);
  }
  return dip;
}

void AugmentCoopWithCoep(CrossOriginOpenerPolicy& coop,
                         const CrossOriginEmbedderPolicy& coep) {
  if (coop.value == CrossOriginOpenerPolicyValue::kSameOrigin &&
      CompatibleWithCrossOriginIsolated(coep.value)) {
    coop.value = CrossOriginOpenerPolicyValue::kSameOriginPlusCoep;
    coop.soap_by_default_value =
        CrossOriginOpenerPolicyValue::kSameOriginPlusCoep;
  }
  if (coop.report_only_value == CrossOriginOpenerPolicyValue::kSameOrigin &&
      (CompatibleWithCrossOriginIsolated(coep.value) ||
       CompatibleWithCrossOriginIsolated(coep.report_only_value))) {
    coop.report_only_value = CrossOriginOpenerPolicyValue::kSameOriginPlusCoep;
  }
}

PolicyHeaders::PolicyHeaders() = default;
PolicyHeaders::PolicyHeaders(const PolicyHeaders&) = default;
PolicyHeaders& PolicyHeaders::operator=(const PolicyHeaders&) = default;
PolicyHeaders::~PolicyHeaders() = default;

CrossOriginEmbedderPolicy EmbedderPolicyOf(const PolicyHeaders& headers) {
  return ParseCrossOriginEmbedderPolicy(headers.coep, headers.coep_report_only);
}

TopLevelDocumentPolicies TopLevelDocumentPoliciesOf(
    const GURL& url,
    const PolicyHeaders& headers) {
  TopLevelDocumentPolicies policies;
  const CrossOriginEmbedderPolicy parsed_coep = EmbedderPolicyOf(headers);
  // A top-level document's creation URL is its own. Embedder policies hold
  // only in a secure context.
  const bool trustworthy = net::IsUrlPotentiallyTrustworthy(url);
  if (trustworthy) {
    policies.coep = parsed_coep;
  }
  // SanitizeCoopHeaders() augments the parsed COOP with the parsed COEP, and
  // keeps it only for a potentially trustworthy URL.
  if (trustworthy) {
    policies.coop =
        ParseCrossOriginOpenerPolicy(headers.coop, headers.coop_report_only);
    AugmentCoopWithCoep(policies.coop, parsed_coep);
  }
  // ComputeWebExposedIsolationInfo(): cross-origin isolated for a response
  // whose COOP is "same-origin-plus-COEP". About:blank never comes as a
  // response here.
  policies.cross_origin_isolated =
      policies.coop.value == CrossOriginOpenerPolicyValue::kSameOriginPlusCoep;
  return policies;
}

bool IgnoresCoop(const GURL& url, const PolicyHeaders& headers) {
  if (net::IsUrlPotentiallyTrustworthy(url)) {
    return false;
  }
  CrossOriginOpenerPolicy coop =
      ParseCrossOriginOpenerPolicy(headers.coop, headers.coop_report_only);
  AugmentCoopWithCoep(coop, EmbedderPolicyOf(headers));
  // SanitizeCoopHeaders()'s has_coop_header.
  return coop.value != CrossOriginOpenerPolicyValue::kUnsafeNone ||
         coop.report_only_value != CrossOriginOpenerPolicyValue::kUnsafeNone ||
         coop.reporting_endpoint || coop.report_only_reporting_endpoint;
}

NavigationDocumentIsolationPolicy NavigationDocumentIsolationPolicyOf(
    const GURL& url,
    const PolicyHeaders& headers) {
  NavigationDocumentIsolationPolicy policy;
  const DocumentIsolationPolicy parsed =
      ParseDocumentIsolationPolicy(headers.dip, headers.dip_report_only);
  // Only a secure context keeps it, and the browser drops any of it, a
  // report-only one or an endpoint too, with its console error elsewhere.
  if (net::IsUrlPotentiallyTrustworthy(url)) {
    policy.dip = parsed;
  } else {
    policy.ignored = parsed != DocumentIsolationPolicy();
  }
  return policy;
}

bool IsolatesByDocumentIsolationPolicy(DocumentIsolationPolicyValue value) {
  return value != DocumentIsolationPolicyValue::kNone;
}

bool WorkerEmbedderPolicyAllowed(const CrossOriginEmbedderPolicy& creator,
                                 const CrossOriginEmbedderPolicy& worker) {
  // A creator whose policy is "unsafe-none", or a worker whose policy is
  // "require-corp" or "credentialless", passes. Their reports, report-only
  // ones included, go to endpoints that a Session never sends to.
  return !CompatibleWithCrossOriginIsolated(creator.value) ||
         CompatibleWithCrossOriginIsolated(worker.value);
}

}  // namespace mitto
