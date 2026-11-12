// CSSOM View's MediaQueryList, which matchMedia() gives: a media query list,
// written back as Chrome writes it, whether it matches the Page's window and
// machine, and its change listeners.
//
// A Page's Machine does not change while it runs, nor does its window, so no
// change event is ever fired, as none would be in Chrome on that machine. The
// listeners are kept as an EventTarget keeps them. Blink also keeps a list
// with change listeners alive while its document is, which matters only to a
// WeakRef; here the collector takes it once nothing else holds it. The
// bindings run onchange, as they run every event handler attribute.
//
// Upstream: third_party/blink/renderer/core/css/media_query_list.h
// Upstream: third_party/blink/renderer/core/css/media_query_list.cc
// Upstream: third_party/blink/renderer/core/css/media_query_matcher.cc

#ifndef MITTO_DOM_MEDIA_QUERY_LIST_H_
#define MITTO_DOM_MEDIA_QUERY_LIST_H_

#include <memory>
#include <string>

#include "mitto/css/media_query.h"
#include "mitto/css/media_query_parser.h"
#include "mitto/css/media_values.h"
#include "mitto/dom/event_target.h"
#include "mitto/page_messages.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-local-handle.h"

namespace mitto {

namespace bindings {
class RuntimeFeatureSet;
}  // namespace bindings

class Window;

// Which media features parse, as the Page's runtime-enabled features say.
MediaQueryFeatures MediaQueryFeaturesFor(
    const bindings::RuntimeFeatureSet& features);

// What the media features of a window read, from the Page's start: its
// Machine's display, scale, viewport and settings. The fonts that ex, ch, ic,
// cap and lh resolve against are Blink's sizes for a document with no font
// data, until the Runtime has fonts (roadmap, M9).
MediaValues MediaValuesFor(const PageStart& start);

class MediaQueryList final : public EventTarget {
 public:
  // The list of `query`, which `window` evaluates.
  static MediaQueryList* Create(Window* window, const std::u16string& query);

  MediaQueryList(Window* window, std::unique_ptr<MediaQuerySet> media);
  ~MediaQueryList() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  std::u16string Media() const;
  bool Matches() const;
  void AddDeprecatedListener(v8::Local<v8::Object> listener);
  void RemoveDeprecatedListener(v8::Local<v8::Object> listener);

 private:
  cppgc::Member<Window> window_;
  const std::unique_ptr<MediaQuerySet> media_;
};

}  // namespace mitto

#endif  // MITTO_DOM_MEDIA_QUERY_LIST_H_
