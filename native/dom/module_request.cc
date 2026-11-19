#include "mitto/dom/module_request.h"

#include <utility>

namespace mitto {

ModuleRequest::ModuleRequest() = default;

ModuleRequest::ModuleRequest(std::u16string specifier,
                             std::vector<ImportAttribute> import_attributes)
    : specifier(std::move(specifier)),
      import_attributes(std::move(import_attributes)) {}

ModuleRequest::ModuleRequest(const ModuleRequest&) = default;
ModuleRequest& ModuleRequest::operator=(const ModuleRequest&) = default;
ModuleRequest::~ModuleRequest() = default;

std::optional<std::u16string> ModuleRequest::GetModuleTypeString() const {
  for (const ImportAttribute& attribute : import_attributes) {
    if (attribute.key == u"type") {
      return attribute.value;
    }
  }
  return std::nullopt;
}

bool ModuleRequest::HasInvalidImportAttributeKey(
    std::u16string* invalid_key) const {
  for (const ImportAttribute& attribute : import_attributes) {
    if (attribute.key != u"type") {
      *invalid_key = attribute.key;
      return true;
    }
  }
  return false;
}

}  // namespace mitto
