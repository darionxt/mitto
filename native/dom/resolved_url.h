// A URL as Blink's KURL keeps it: its canonical string and its components,
// which a URL that does not parse keeps too, with KURL's setters, so that the
// parts of a link's URL read and change as in Chrome.
//
// Upstream: third_party/blink/renderer/platform/weborigin/kurl.h
// Upstream: third_party/blink/renderer/platform/weborigin/kurl.cc

#ifndef MITTO_DOM_RESOLVED_URL_H_
#define MITTO_DOM_RESOLVED_URL_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <string_view>

#include "url/third_party/mozilla/url_parse.h"
#include "url/url_canon.h"

namespace mitto {

class ResolvedURL {
 public:
  // The null URL, KURL().
  ResolvedURL();
  // `relative` resolved against `base`, Blink's KURL(base, relative).
  ResolvedURL(const ResolvedURL& base, std::u16string_view relative);
  // An absolute URL, KURL(url).
  explicit ResolvedURL(std::u16string_view url);

  bool IsNull() const { return is_null_; }
  bool IsValid() const { return is_valid_; }
  std::u16string GetString() const;

  std::u16string Protocol() const;
  std::u16string Host() const;
  // 0 for none.
  uint16_t Port() const;
  bool HasPort() const;
  std::u16string User() const;
  std::u16string Pass() const;
  std::u16string Path() const;
  // "?" and the query, or "" for none.
  std::u16string QueryWithLeadingQuestionMark() const;
  // "#" and the fragment, or "" for none.
  std::u16string FragmentIdentifierWithLeadingNumberSign() const;
  bool IsStandard() const;
  bool IsHierarchical() const;
  bool CanSetHostOrPort() const { return IsHierarchical(); }
  bool CanSetPathname() const { return IsHierarchical(); }
  bool CanRemoveHost() const;
  // Blink's KURL::HostEnd() and PathStart().
  int HostEnd() const;
  int PathStart() const;

  // KURL's setters, each as Blink writes it. SetProtocol() returns false for
  // what is no scheme, and true when it keeps the URL as it is, as for a
  // special URL that a scheme that is not special would leave.
  bool SetProtocol(std::u16string_view protocol);
  void SetUser(std::u16string_view user);
  void SetPass(std::u16string_view pass);
  void SetHost(std::u16string_view host);
  void SetHostAndPort(std::u16string_view host_and_port);
  void SetPort(std::u16string_view port);
  void RemovePort();
  void SetPath(std::u16string_view path);
  // Null clears the query.
  void SetQuery(const std::optional<std::u16string>& query);
  // Null clears the fragment.
  void SetFragmentIdentifier(const std::optional<std::u16string>& fragment);
  void RemoveFragmentIdentifier();

 private:
  std::u16string Component(const url::Component& component) const;
  void ReplaceComponents(const url::Replacements<char>& replacements,
                         bool preserve_validity = false);

  std::string string_;
  url::Parsed parsed_;
  bool is_valid_ = false;
  bool is_null_ = true;
};

// Whether `port` is the default port of `protocol`, Blink's
// IsDefaultPortForProtocol().
bool IsDefaultPortForProtocol(uint16_t port, std::u16string_view protocol);

}  // namespace mitto

#endif  // MITTO_DOM_RESOLVED_URL_H_
