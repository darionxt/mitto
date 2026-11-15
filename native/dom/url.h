// The URL Standard's URL, as Blink's URL has it: a URL that parses as Blink's
// KURL parses it, each part of it read and set as a link's are (UrlUtils), and
// its URLSearchParams kept in step with its query. createObjectURL() and
// revokeObjectURL(), of the File API, are Known Gaps.
//
// Upstream: third_party/blink/renderer/core/url/url.h
// Upstream: third_party/blink/renderer/core/url/url.cc

#ifndef MITTO_DOM_URL_H_
#define MITTO_DOM_URL_H_

#include <optional>
#include <string>

#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/resolved_url.h"
#include "mitto/dom/url_utils.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class URLSearchParams;

class URL final : public bindings::ScriptWrappable, public UrlUtils {
 public:
  // `new URL(url, base)`, which throws a TypeError for a base or a URL that
  // does not parse.
  static URL* Create(const std::u16string& url,
                     const std::optional<std::u16string>& base,
                     bindings::ExceptionState& exception_state);
  // URL.parse(), null where the constructor would throw.
  static URL* Parse(const std::u16string& url,
                    const std::optional<std::u16string>& base);
  static bool CanParse(const std::u16string& url,
                       const std::optional<std::u16string>& base);

  explicit URL(const ResolvedURL& url);
  ~URL() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface that UrlUtils does not have.
  void SetHref(const std::u16string& value,
               bindings::ExceptionState& exception_state);
  void SetSearch(const std::u16string& value);
  URLSearchParams* SearchParams();
  std::u16string ToJSON() const { return Href(); }

  // Whether the URL's search parameters are being set from its query, during
  // which they do not write themselves back.
  bool IsInUpdate() const { return is_in_update_; }

 private:
  friend class URLSearchParams;

  // UrlUtils's.
  ResolvedURL Url() const override;
  void SetUrl(const ResolvedURL& url) override;
  std::u16string Input() const override;

  // The search parameters, if any, take the URL's query again.
  void Update();
  void UpdateSearchParams(std::u16string_view query);

  ResolvedURL url_;
  // Blink's is weak, as the wrapper keeps the object that searchParams gave.
  // This holds it, which nothing tells apart.
  cppgc::Member<URLSearchParams> search_params_;
  bool is_in_update_ = false;
};

}  // namespace mitto

#endif  // MITTO_DOM_URL_H_
