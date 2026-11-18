// A module request of a module script or of import(): the specifier and the
// import attributes, as Blink's ModuleRequest has them.
//
// Upstream: third_party/blink/renderer/bindings/core/v8/module_request.h

#ifndef MITTO_DOM_MODULE_REQUEST_H_
#define MITTO_DOM_MODULE_REQUEST_H_

#include <optional>
#include <string>
#include <vector>

namespace mitto {

struct ImportAttribute {
  std::u16string key;
  std::u16string value;
};

struct ModuleRequest {
  ModuleRequest();
  ModuleRequest(std::u16string specifier,
                std::vector<ImportAttribute> import_attributes);
  ModuleRequest(const ModuleRequest&);
  ModuleRequest& operator=(const ModuleRequest&);
  ~ModuleRequest();

  // The value of the attribute "type", if there is one.
  std::optional<std::u16string> GetModuleTypeString() const;
  // Whether an attribute has another key than "type", which it sets
  // `invalid_key` to.
  bool HasInvalidImportAttributeKey(std::u16string* invalid_key) const;

  std::u16string specifier;
  std::vector<ImportAttribute> import_attributes;
};

}  // namespace mitto

#endif  // MITTO_DOM_MODULE_REQUEST_H_
