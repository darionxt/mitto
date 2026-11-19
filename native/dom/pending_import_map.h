// An import map that a script element of type importmap gives, as Blink's
// PendingImportMap has it: its parse result, which is registered at once, as
// Chrome takes only inline import maps. Registering reports the parse's error
// as an error that nothing caught, or merges the map into the window's.
//
// Upstream: third_party/blink/renderer/core/script/pending_import_map.cc

#ifndef MITTO_DOM_PENDING_IMPORT_MAP_H_
#define MITTO_DOM_PENDING_IMPORT_MAP_H_

#include <memory>
#include <optional>
#include <string_view>

#include "mitto/dom/import_map.h"
#include "url/gurl.h"
#include "v8/include/cppgc/persistent.h"

namespace mitto {

class ScriptElementBase;
class Window;

class PendingImportMap {
 public:
  // Creating an import map parse result: parses `import_map_text` against
  // `base_url`, with the console warnings in `element`'s window.
  static std::unique_ptr<PendingImportMap> CreateInline(
      ScriptElementBase& element,
      std::u16string_view import_map_text,
      const GURL& base_url);

  PendingImportMap(Window* original_window,
                   std::unique_ptr<ImportMap> import_map,
                   std::optional<ImportMapError> error_to_rethrow);
  PendingImportMap(const PendingImportMap&) = delete;
  PendingImportMap& operator=(const PendingImportMap&) = delete;
  ~PendingImportMap();

  // Register an import map, in the window it was made in.
  void RegisterImportMap();

 private:
  const cppgc::WeakPersistent<Window> original_window_;
  std::unique_ptr<ImportMap> import_map_;
  std::optional<ImportMapError> error_to_rethrow_;
};

}  // namespace mitto

#endif  // MITTO_DOM_PENDING_IMPORT_MAP_H_
