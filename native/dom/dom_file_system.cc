#include "mitto/dom/dom_file_system.h"

#include <string>

#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/v8_dom_file_system.h"
#include "mitto/dom/directory_entry.h"
#include "url/gurl.h"
#include "url/url_constants.h"

namespace mitto {

namespace {

// storage::GetIdentifierFromOrigin(): the scheme, the host, with an IPv6
// address's colons as underscores, and the port, 0 for the scheme's default,
// each after an underscore.
std::string IdentifierOf(const url::Origin& origin) {
  const GURL url = origin.GetURL();
  if (url.SchemeIsFile()) {
    return "file__0";
  }
  std::string host = url.GetHost();
  if (host.size() >= 5 && host.front() == '[' && host.back() == ']') {
    base::ReplaceChars(host, ":", "_", &host);
  }
  const int port = url.IntPort();
  return base::StrCat(
      {url.GetScheme(), "_", host, "_",
       base::NumberToString(port == url::PORT_UNSPECIFIED ? 0 : port)});
}

}  // namespace

// static
DOMFileSystem* DOMFileSystem::Create(const url::Origin& origin, Type type) {
  return bindings::MakeGarbageCollected<DOMFileSystem>(origin, type);
}

// storage::GetFileSystemName() and GetFileSystemRootURI(), as the browser
// gives them for the origin.
DOMFileSystem::DOMFileSystem(const url::Origin& origin, Type type)
    : name_(base::UTF8ToUTF16(base::StrCat(
          {IdentifierOf(origin), ":",
           type == Type::kTemporary ? "Temporary" : "Persistent"}))),
      root_url_(base::UTF8ToUTF16(base::StrCat(
          {"filesystem:", origin.GetURL().GetWithEmptyPath().spec(),
           type == Type::kTemporary ? "temporary" : "persistent", "/"}))),
      root_(DirectoryEntry::Create(this, u"/")) {}

DOMFileSystem::~DOMFileSystem() = default;

const bindings::WrapperTypeInfo* DOMFileSystem::GetWrapperTypeInfo() const {
  return &bindings::V8DOMFileSystem::kWrapperTypeInfo;
}

void DOMFileSystem::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(root_);
}

std::u16string DOMFileSystem::UrlOf(const std::u16string& full_path) const {
  // The path after its leading slash. Only the root entry exists, since
  // making the others is a Known Gap, so no path needs the escapes that
  // EncodeWithUrlEscapeSequences() would add.
  return root_url_ + full_path.substr(1);
}

}  // namespace mitto
