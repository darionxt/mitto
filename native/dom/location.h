// The HTML Standard's Location, the window's location, which its document's
// location gives too: the parts of the URL of the window's document, read as
// Blink's Location reads them, and the navigations that its setters,
// assign(), replace() and reload() start, as Blink's start them
// (frame_loader.h). ancestorOrigins is a Known Gap, whose default is the empty
// list that a top-level page's is.
//
// Upstream: third_party/blink/renderer/core/frame/location.cc
// Upstream: third_party/blink/renderer/core/url/url_utils_read_only.cc

#ifndef MITTO_DOM_LOCATION_H_
#define MITTO_DOM_LOCATION_H_

#include <string>

#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/frame_loader.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

class GURL;

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

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
  void SetHref(const std::u16string& value,
               bindings::ExceptionState& exception_state);
  std::u16string Origin() const;
  std::u16string Protocol() const;
  void SetProtocol(const std::u16string& value,
                   bindings::ExceptionState& exception_state);
  std::u16string Host() const;
  void SetHost(const std::u16string& value,
               bindings::ExceptionState& exception_state);
  std::u16string Hostname() const;
  void SetHostname(const std::u16string& value,
                   bindings::ExceptionState& exception_state);
  std::u16string Port() const;
  void SetPort(const std::u16string& value,
               bindings::ExceptionState& exception_state);
  std::u16string Pathname() const;
  void SetPathname(const std::u16string& value,
                   bindings::ExceptionState& exception_state);
  std::u16string Search() const;
  void SetSearch(const std::u16string& value,
                 bindings::ExceptionState& exception_state);
  std::u16string Hash() const;
  void SetHash(const std::u16string& value,
               bindings::ExceptionState& exception_state);
  void Assign(const std::u16string& url,
              bindings::ExceptionState& exception_state);
  void Replace(const std::u16string& url,
               bindings::ExceptionState& exception_state);
  void Reload();
  std::u16string ToString() const;

 private:
  GURL Url() const;
  // Location::SetLocation(): `url` resolved against the entered window's
  // document, which throws a SyntaxError if it does not resolve, then the
  // window's frame navigates there.
  void SetLocation(const std::u16string& url,
                   bindings::ExceptionState& exception_state,
                   FrameLoadType type = FrameLoadType::kStandard);

  cppgc::Member<Window> window_;
};

}  // namespace mitto

#endif  // MITTO_DOM_LOCATION_H_
