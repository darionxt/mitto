// The parts of a URL that a page reads and sets on a URL object and on a
// link's href, as Blink's UrlUtilsReadOnly and UrlUtils read and set them: each
// setter changes its part of the URL that Url() gives, and gives the result to
// SetUrl() where Blink does.
//
// Upstream: third_party/blink/renderer/core/url/url_utils.h
// Upstream: third_party/blink/renderer/core/url/url_utils.cc
// Upstream: third_party/blink/renderer/core/url/url_utils_read_only.h
// Upstream: third_party/blink/renderer/core/url/url_utils_read_only.cc

#ifndef MITTO_DOM_URL_UTILS_H_
#define MITTO_DOM_URL_UTILS_H_

#include <string>

namespace mitto {

class ResolvedURL;

class UrlUtils {
 public:
  UrlUtils(const UrlUtils&) = delete;
  UrlUtils& operator=(const UrlUtils&) = delete;

  // The URL's string, or for none what Input() gives.
  std::u16string Href() const;
  std::u16string Origin() const;
  std::u16string Protocol() const;
  void SetProtocol(const std::u16string& value);
  std::u16string Username() const;
  void SetUsername(const std::u16string& value);
  std::u16string Password() const;
  void SetPassword(const std::u16string& value);
  std::u16string Host() const;
  void SetHost(const std::u16string& value);
  std::u16string Hostname() const;
  void SetHostname(const std::u16string& value);
  std::u16string Port() const;
  void SetPort(const std::u16string& value);
  std::u16string Pathname() const;
  void SetPathname(const std::u16string& value);
  std::u16string Search() const;
  void SetSearch(const std::u16string& value);
  std::u16string Hash() const;
  void SetHash(const std::u16string& value);

 protected:
  UrlUtils() = default;
  virtual ~UrlUtils() = default;

  // The URL, or the null URL for none.
  virtual ResolvedURL Url() const = 0;
  virtual void SetUrl(const ResolvedURL& url) = 0;
  // What Href() gives for no URL, such as the href attribute that did not
  // resolve.
  virtual std::u16string Input() const = 0;

  // What search's setter does, which a URL object follows with its search
  // parameters.
  void SetSearchInternal(const std::u16string& value);
};

}  // namespace mitto

#endif  // MITTO_DOM_URL_UTILS_H_
