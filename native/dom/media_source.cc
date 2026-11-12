#include "mitto/dom/media_source.h"

#include "mitto/bindings/v8_media_source.h"

namespace mitto {

// static
MediaSource* MediaSource::Create() {
  return bindings::MakeGarbageCollected<MediaSource>();
}

// static
bindings::ScriptWrappable* MediaSource::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<MediaSource>(
      info, bindings::V8MediaSource::kWrapperTypeInfo);
}

MediaSource::MediaSource() = default;
MediaSource::~MediaSource() = default;

const bindings::WrapperTypeInfo* MediaSource::GetWrapperTypeInfo() const {
  return &bindings::V8MediaSource::kWrapperTypeInfo;
}

}  // namespace mitto
