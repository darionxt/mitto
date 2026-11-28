// Cross-Origin-Embedder-Policy, Cross-Origin-Opener-Policy and
// Document-Isolation-Policy, as Chrome's network service parses them from a
// response, and what Chrome's browser makes of them for a document of a
// navigation's response: its embedder policy, whether a top-level document is
// cross-origin isolated, and its document isolation policy. The source export
// leaves services/network and content/browser out, so this is ported with the
// upstream names kept.
//
// Upstream: services/network/public/cpp/cross_origin_embedder_policy.cc
// Upstream: services/network/public/cpp/cross_origin_embedder_policy_parser.cc
// Upstream: services/network/public/cpp/cross_origin_opener_policy.cc
// Upstream: services/network/public/cpp/cross_origin_opener_policy_parser.cc
// Upstream: services/network/public/cpp/document_isolation_policy_parser.cc
// Upstream: content/browser/renderer_host/navigation_request.cc
// Upstream: content/browser/security/coop/cross_origin_opener_policy_status.cc

#ifndef MITTO_CROSS_ORIGIN_POLICIES_H_
#define MITTO_CROSS_ORIGIN_POLICIES_H_

#include <optional>
#include <string>
#include <string_view>

#include "url/gurl.h"

namespace mitto {

// network::mojom::CrossOriginEmbedderPolicyValue.
enum class CrossOriginEmbedderPolicyValue {
  kNone,
  kCredentialless,
  kRequireCorp,
};

// network::CrossOriginEmbedderPolicy.
struct CrossOriginEmbedderPolicy {
  bool operator==(const CrossOriginEmbedderPolicy&) const = default;

  CrossOriginEmbedderPolicyValue value = CrossOriginEmbedderPolicyValue::kNone;
  std::optional<std::string> reporting_endpoint;
  CrossOriginEmbedderPolicyValue report_only_value =
      CrossOriginEmbedderPolicyValue::kNone;
  std::optional<std::string> report_only_reporting_endpoint;
};

// network::CompatibleWithCrossOriginIsolated(): whether a document or worker
// of this embedder policy may be cross-origin isolated.
bool CompatibleWithCrossOriginIsolated(CrossOriginEmbedderPolicyValue value);

// network::mojom::CrossOriginOpenerPolicyValue.
enum class CrossOriginOpenerPolicyValue {
  kUnsafeNone,
  kSameOrigin,
  kSameOriginPlusCoep,
  kSameOriginAllowPopups,
  kNoopenerAllowPopups,
};

// network::CrossOriginOpenerPolicy.
struct CrossOriginOpenerPolicy {
  bool operator==(const CrossOriginOpenerPolicy&) const = default;

  CrossOriginOpenerPolicyValue value =
      CrossOriginOpenerPolicyValue::kUnsafeNone;
  std::optional<std::string> reporting_endpoint;
  CrossOriginOpenerPolicyValue report_only_value =
      CrossOriginOpenerPolicyValue::kUnsafeNone;
  std::optional<std::string> report_only_reporting_endpoint;
  CrossOriginOpenerPolicyValue soap_by_default_value =
      CrossOriginOpenerPolicyValue::kUnsafeNone;
};

// ParseCrossOriginEmbedderPolicy(), from the values of a response's
// Cross-Origin-Embedder-Policy and Cross-Origin-Embedder-Policy-Report-Only,
// each as HttpResponseHeaders::GetNormalizedHeader() joins its lines, or
// nullopt for none.
CrossOriginEmbedderPolicy ParseCrossOriginEmbedderPolicy(
    const std::optional<std::string>& header,
    const std::optional<std::string>& report_only_header);

// ParseCrossOriginOpenerPolicy(), from the values of Cross-Origin-Opener-Policy
// and Cross-Origin-Opener-Policy-Report-Only, with Chrome 157's features:
// COOP on, "noopener-allow-popups" on, and COOP by default off.
CrossOriginOpenerPolicy ParseCrossOriginOpenerPolicy(
    const std::optional<std::string>& header,
    const std::optional<std::string>& report_only_header);

// AugmentCoopWithCoep(): "same-origin" with an embedder policy that is
// compatible with cross-origin isolation is "same-origin-plus-COEP".
void AugmentCoopWithCoep(CrossOriginOpenerPolicy& coop,
                         const CrossOriginEmbedderPolicy& coep);

// network::mojom::DocumentIsolationPolicyValue.
enum class DocumentIsolationPolicyValue {
  kNone,
  kIsolateAndCredentialless,
  kIsolateAndRequireCorp,
};

// network::DocumentIsolationPolicy.
struct DocumentIsolationPolicy {
  bool operator==(const DocumentIsolationPolicy&) const = default;

  DocumentIsolationPolicyValue value = DocumentIsolationPolicyValue::kNone;
  std::optional<std::string> reporting_endpoint;
  DocumentIsolationPolicyValue report_only_value =
      DocumentIsolationPolicyValue::kNone;
  std::optional<std::string> report_only_reporting_endpoint;
};

// ParseDocumentIsolationPolicy(), from the values of a response's
// Document-Isolation-Policy and Document-Isolation-Policy-Report-Only, each as
// HttpResponseHeaders::GetNormalizedHeader() joins its lines, or nullopt for
// none.
DocumentIsolationPolicy ParseDocumentIsolationPolicy(
    const std::optional<std::string>& header,
    const std::optional<std::string>& report_only_header);

// The console error of Chrome's browser for a response whose COOP it ignored,
// at a URL that is not potentially trustworthy.
inline constexpr std::string_view kCoopIgnoredOnUntrustworthyOrigin =
    "The Cross-Origin-Opener-Policy header has been ignored, because the "
    "URL's origin was untrustworthy. It was defined either in the final "
    "response or a redirect. Please deliver the response using the HTTPS "
    "protocol. You can also use the 'localhost' origin instead. See "
    "https://www.w3.org/TR/powerful-features/#potentially-trustworthy-origin "
    "and https://html.spec.whatwg.org/#the-cross-origin-opener-policy-header.";

// The lines of a response that these policies come from, each as
// net::HttpResponseHeaders::GetNormalizedHeader() joins a header's lines, or
// nullopt for none.
struct PolicyHeaders {
  PolicyHeaders();
  PolicyHeaders(const PolicyHeaders&);
  PolicyHeaders& operator=(const PolicyHeaders&);
  ~PolicyHeaders();

  std::optional<std::string> coep;
  std::optional<std::string> coep_report_only;
  std::optional<std::string> coop;
  std::optional<std::string> coop_report_only;
  std::optional<std::string> dip;
  std::optional<std::string> dip_report_only;
};

// PopulateParsedHeaders(): the embedder policy of a response, as the network
// service parses it for every response, which a worker's script response
// gives the worker, as DedicatedWorkerHost::DidStartScriptLoad() takes it.
CrossOriginEmbedderPolicy EmbedderPolicyOf(const PolicyHeaders& headers);

// What a top-level navigation's response at `url` gives its document, as
// Chrome's browser works it out.
struct TopLevelDocumentPolicies {
  // NavigationRequest::ComputeCrossOriginEmbedderPolicy(): the response's,
  // where `url` is potentially trustworthy, and none elsewhere.
  CrossOriginEmbedderPolicy coep;
  // The response's after CrossOriginOpenerPolicyStatus::SanitizeCoopHeaders(),
  // augmented with its COEP, and none where `url` is not potentially
  // trustworthy.
  CrossOriginOpenerPolicy coop;
  // NavigationRequest::ComputeWebExposedIsolationInfo(): whether the document
  // is cross-origin isolated, which COOP "same-origin-plus-COEP" makes it.
  bool cross_origin_isolated = false;
};
TopLevelDocumentPolicies TopLevelDocumentPoliciesOf(
    const GURL& url,
    const PolicyHeaders& headers);

// Whether SanitizeCoopHeaders() drops the COOP of a top-level navigation's
// response at `url`, a redirect's or the last, with its console error: one
// that has COOP, at a URL that is not potentially trustworthy.
bool IgnoresCoop(const GURL& url, const PolicyHeaders& headers);

// The console error of Chrome's browser for a response whose
// Document-Isolation-Policy it ignored, at a URL that is not potentially
// trustworthy.
inline constexpr std::string_view kDipIgnoredOnUntrustworthyOrigin =
    "The Document-Isolation-Policy header has been ignored because the URL's "
    "origin was untrustworthy. Please deliver the response using the HTTPS "
    "protocol. You can also use the 'localhost' origin instead. See "
    "https://www.w3.org/TR/powerful-features/#potentially-trustworthy-origin.";

// What a navigation's response at `url` gives its document of
// Document-Isolation-Policy, as Chrome's browser works it out for a document
// of any frame, Chrome 157 having kDocumentIsolationPolicy and
// kDocumentIsolationPolicyWithoutSiteIsolation on.
struct NavigationDocumentIsolationPolicy {
  // NavigationRequest::SanitizeDocumentIsolationPolicyHeader(): the
  // response's, where `url` is potentially trustworthy, and none elsewhere.
  DocumentIsolationPolicy dip;
  // Whether it dropped one there, with its console error.
  bool ignored = false;
};
NavigationDocumentIsolationPolicy NavigationDocumentIsolationPolicyOf(
    const GURL& url,
    const PolicyHeaders& headers);

// NavigationRequest::ComputeCrossOriginIsolationKey(): whether a document of
// this policy is isolated in an agent cluster of its own, of its origin, which
// Chrome's renderer makes cross-origin isolated whatever its permissions
// policy says, as PolicyContainerPolicies'
// cross_origin_isolation_enabled_by_dip has it.
bool IsolatesByDocumentIsolationPolicy(DocumentIsolationPolicyValue value);

// DedicatedWorkerHost::CheckCOEP(): whether a worker of embedder policy
// `worker` may run for a creator of embedder policy `creator`.
bool WorkerEmbedderPolicyAllowed(const CrossOriginEmbedderPolicy& creator,
                                 const CrossOriginEmbedderPolicy& worker);

}  // namespace mitto

#endif  // MITTO_CROSS_ORIGIN_POLICIES_H_
