#include "mitto/dom/document_write_intervention.h"

#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/dom/document.h"
#include "mitto/dom/window.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "url/gurl.h"

namespace mitto {

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

}  // namespace mitto
