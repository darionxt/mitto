#include "mitto/dom/popover_data.h"

#include "mitto/dom/element.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

PopoverData::PopoverData(PopoverValueType type) : type_(type) {}

PopoverData::~PopoverData() = default;

void PopoverData::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(invoker_);
}

}  // namespace mitto
