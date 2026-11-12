// Media Source Extensions' MediaSource: what a page reads of it without
// making one, its static canConstructInDedicatedWorker. Its constructor and
// other members are Known Gaps. ManagedMediaSource, which has no class,
// inherits from it, so this class is not final.
//
// Upstream: third_party/blink/renderer/modules/mediasource/media_source.cc

#ifndef MITTO_DOM_MEDIA_SOURCE_H_
#define MITTO_DOM_MEDIA_SOURCE_H_

#include "mitto/dom/event_target.h"

namespace mitto {

class MediaSource : public EventTarget {
 public:
  static MediaSource* Create();
  // The one a Known Gap gives, whose wrapper is of `info`.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  MediaSource();
  ~MediaSource() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  // Chrome makes MediaSource in a dedicated worker, as the feature that
  // allows it is on by default.
  static bool CanConstructInDedicatedWorker() { return true; }
};

}  // namespace mitto

#endif  // MITTO_DOM_MEDIA_SOURCE_H_
