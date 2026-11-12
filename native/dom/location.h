// The HTML Standard's Location, the window's location, which its document's
// location gives too: the parts of the URL of the window's document, read as
// Blink's Location reads them. Navigating, through a setter, assign(),
// replace() or reload(), is a Known Gap, and so is ancestorOrigins, whose
// default is the empty list that a top-level page's is.
//
// Upstream: third_party/blink/renderer/core/frame/location.cc
// Upstream: third_party/blink/renderer/core/url/url_utils_read_only.cc

#ifndef MITTO_DOM_LOCATION_H_
#define MITTO_DOM_LOCATION_H_

#include <string>

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

class GURL;

namespace mitto {

class Window;

class Location final : public bindings::ScriptWrappable {
 public:
  static Location* Create(Window* window);
  // A Location of no window, whose URL is about:blank's, for a Known Gap that
  // gives one.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  explicit Location(Window* window);
  ~Location() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  std::u16string Href() const;
  std::u16string Origin() const;
  std::u16string Protocol() const;
  std::u16string Host() const;
  std::u16string Hostname() const;
  std::u16string Port() const;
  std::u16string Pathname() const;
  std::u16string Search() const;
  std::u16string Hash() const;
  std::u16string ToString() const;

 private:
  GURL Url() const;

  cppgc::Member<Window> window_;
};

}  // namespace mitto

#endif  // MITTO_DOM_LOCATION_H_
