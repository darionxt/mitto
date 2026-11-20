#include "mitto/dom/pending_import_map.h"

#include <string>
#include <utility>

#include "base/check.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/dom/document.h"
#include "mitto/dom/element.h"
#include "mitto/dom/modulator.h"
#include "mitto/dom/script_element_base.h"
#include "mitto/dom/window.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

// static
std::unique_ptr<PendingImportMap> PendingImportMap::CreateInline(
    ScriptElementBase& element,
    std::u16string_view import_map_text,
    const GURL& base_url) {
  Window* const window = element.AsElement().GetDocument().DefaultView();
  std::optional<ImportMapError> error_to_rethrow;
  std::unique_ptr<ImportMap> import_map = ImportMap::Parse(
      import_map_text, base_url,
      [window](std::string message) {
        if (window) {
          window->AddConsoleMessage(ConsoleLevel::kWarning, std::move(message));
        }
      },
      &error_to_rethrow);
  return std::make_unique<PendingImportMap>(window, std::move(import_map),
                                            std::move(error_to_rethrow));
}

PendingImportMap::PendingImportMap(
    Window* original_window,
    std::unique_ptr<ImportMap> import_map,
    std::optional<ImportMapError> error_to_rethrow)
    : original_window_(original_window),
      import_map_(std::move(import_map)),
      error_to_rethrow_(std::move(error_to_rethrow)) {}

PendingImportMap::~PendingImportMap() = default;

void PendingImportMap::RegisterImportMap() {
  CHECK(import_map_);
  Window* const window = original_window_.Get();
  if (!window || !window->HasContext()) {
    return;
  }
  // Step 1: a parse result's error to rethrow is reported for the window.
  if (error_to_rethrow_) {
    v8::Isolate* const isolate = v8::Isolate::GetCurrent();
    const v8::HandleScope handle_scope(isolate);
    bindings::ReportException(isolate, error_to_rethrow_->ToV8(isolate));
    return;
  }
  // Step 2: merge existing and new import maps.
  window->GetModulator()->MergeExistingAndNewImportMaps(std::move(import_map_));
}

}  // namespace mitto
