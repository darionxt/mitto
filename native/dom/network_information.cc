#include "mitto/dom/network_information.h"

#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_network_information.h"

namespace mitto {

// static
NetworkInformation* NetworkInformation::Create(
    const PageConnection& connection,
    const std::string& effective_type) {
  return bindings::MakeGarbageCollected<NetworkInformation>(connection,
                                                            effective_type);
}

NetworkInformation::NetworkInformation(const PageConnection& connection,
                                       const std::string& effective_type)
    : connection_(connection),
      effective_type_(base::UTF8ToUTF16(effective_type)) {}

NetworkInformation::~NetworkInformation() = default;

const bindings::WrapperTypeInfo* NetworkInformation::GetWrapperTypeInfo()
    const {
  return &bindings::V8NetworkInformation::kWrapperTypeInfo;
}

std::u16string NetworkInformation::EffectiveType() const {
  return effective_type_;
}

uint64_t NetworkInformation::Rtt() const {
  return connection_.rtt;
}

double NetworkInformation::Downlink() const {
  return connection_.downlink;
}

bool NetworkInformation::SaveData() const {
  // Chrome's setting that sends Save-Data is off.
  return false;
}

}  // namespace mitto
