#include "mitto/dom/document_write_intervention.h"

#include <optional>
#include <string>

#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/dom/document.h"
#include "mitto/dom/script_loader.h"
#include "mitto/dom/window.h"
#include "mitto/html/html_token.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "url/gurl.h"

namespace mitto {
namespace {

std::optional<std::u16string> FindAttribute(const HtmlToken& token,
                                            std::u16string_view name) {
  for (const HtmlAttribute& attribute : token.attributes) {
    if (attribute.name == name) {
      return attribute.value;
    }
  }
  return std::nullopt;
}

// HTMLTokenizer::SpeculativeStateForTag(), with scripting on: the state that
// the tree builder would set after a start tag.
std::optional<HtmlTokenizer::State> SpeculativeStateForTag(
    std::u16string_view name) {
  if (name == u"textarea" || name == u"title") {
    return HtmlTokenizer::State::kRcdata;
  }
  if (name == u"plaintext") {
    return HtmlTokenizer::State::kPlaintext;
  }
  if (name == u"script") {
    return HtmlTokenizer::State::kScriptData;
  }
  if (name == u"style" || name == u"iframe" || name == u"xmp" ||
      name == u"noembed" || name == u"noframes" || name == u"noscript") {
    return HtmlTokenizer::State::kRawtext;
  }
  return std::nullopt;
}

}  // namespace

void MaybeDisallowFetchForDocWrittenScript(const GURL& url,
                                           bool deferred,
                                           Document& document) {
  // Only a script that blocks the parser, that a write inserts in the main
  // frame's document.
  Window* const window = document.DefaultView();
  if (!document.IsInDocumentWrite() || !window || deferred ||
      !url.SchemeIsHTTPOrHTTPS()) {
    return;
  }
  // One of the document's host, or of another host of its registrable
  // domain, is a first party's. Hosts with no registrable domain, such as
  // localhost and IP addresses, are compared as hosts alone.
  const std::string document_host = base::UTF16ToUTF8(document.Domain());
  const std::string request_host(url.host());
  if (request_host == document_host) {
    return;
  }
  using net::registry_controlled_domains::GetDomainAndRegistry;
  using net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES;
  const std::string request_domain =
      GetDomainAndRegistry(request_host, INCLUDE_PRIVATE_REGISTRIES);
  const std::string document_domain =
      GetDomainAndRegistry(document_host, INCLUDE_PRIVATE_REGISTRIES);
  if (!request_domain.empty() && !document_domain.empty() &&
      request_domain == document_domain) {
    return;
  }
  window->AddConsoleMessage(
      ConsoleLevel::kWarning,
      base::StrCat(
          {"A parser-blocking, cross site (i.e. different eTLD+1) script, ",
           url.possibly_invalid_spec(),
           ", is invoked via document.write. The network request for this "
           "script MAY be blocked by the browser in this or a future page "
           "load due to poor network connectivity. If blocked in this page "
           "load, it will be confirmed in a subsequent console message. See "
           "https://www.chromestatus.com/feature/5718547946799104 for more "
           "details."}));
  // Chrome would not block a reload, which a Page does not make yet, and
  // blocks no fetch with its desktop settings, so the script is fetched as
  // any other.
}

DocWrittenScriptScanner::DocWrittenScriptScanner()
    : tokenizer_(HtmlTokenizer::Options()) {}

DocWrittenScriptScanner::~DocWrittenScriptScanner() = default;

void DocWrittenScriptScanner::Scan(std::u16string_view source,
                                   Document& document) {
  tokenizer_.Append(source);
  HtmlToken token;
  while (tokenizer_.NextToken(token)) {
    if (token.type == HtmlToken::Type::kEndTag) {
      if (token.name == u"template") {
        if (template_count_) {
          --template_count_;
        } else if (shadow_root_count_) {
          --shadow_root_count_;
        }
      }
      continue;
    }
    if (token.type != HtmlToken::Type::kStartTag) {
      continue;
    }
    if (token.name == u"template") {
      const std::optional<std::u16string> mode =
          FindAttribute(token, u"shadowrootmode");
      if (mode && !template_count_ &&
          (base::EqualsCaseInsensitiveASCII(*mode, u"open") ||
           base::EqualsCaseInsensitiveASCII(*mode, u"closed"))) {
        ++shadow_root_count_;
      } else {
        ++template_count_;
      }
    }
    // HTMLPreloadScanner updates the tokenizer's state for every start tag.
    if (const std::optional<HtmlTokenizer::State> state =
            SpeculativeStateForTag(token.name)) {
      tokenizer_.SetState(*state);
    }
    if (template_count_ || token.name != u"script") {
      continue;
    }
    // The scanner fetches a classic or module script with src, but a classic
    // one with nomodule.
    const std::optional<std::u16string> src = FindAttribute(token, u"src");
    if (!src) {
      continue;
    }
    const std::u16string_view stripped =
        base::TrimString(*src, u" \t\n\f\r", base::TRIM_ALL);
    const ScriptLoader::ScriptTypeAtPrepare type =
        ScriptLoader::GetScriptTypeAtPrepare(FindAttribute(token, u"type"),
                                             FindAttribute(token, u"language"));
    const bool nomodule = FindAttribute(token, u"nomodule").has_value();
    if (stripped.empty() ||
        (type != ScriptLoader::ScriptTypeAtPrepare::kClassic &&
         type != ScriptLoader::ScriptTypeAtPrepare::kModule) ||
        (type == ScriptLoader::ScriptTypeAtPrepare::kClassic && nomodule)) {
      continue;
    }
    const GURL url = document.CompleteUrl(stripped);
    if (!url.is_valid()) {
      continue;
    }
    MaybeDisallowFetchForDocWrittenScript(
        url,
        FindAttribute(token, u"async").has_value() ||
            FindAttribute(token, u"defer").has_value(),
        document);
  }
}

}  // namespace mitto
