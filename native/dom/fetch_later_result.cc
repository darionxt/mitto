#include "mitto/dom/fetch_later_result.h"

#include "mitto/bindings/v8_fetch_later_result.h"

namespace mitto {

FetchLaterResult::FetchLaterResult() = default;
FetchLaterResult::~FetchLaterResult() = default;

const bindings::WrapperTypeInfo* FetchLaterResult::GetWrapperTypeInfo() const {
  return &bindings::V8FetchLaterResult::kWrapperTypeInfo;
}

}  // namespace mitto
