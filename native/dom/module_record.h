// What the module machinery asks of V8 for a module record, as Blink's
// ModuleRecord has it: compiling a module script's text, linking a record
// with the records that it imports, which the window's Modulator finds, its
// module requests, and its namespace.
//
// Upstream: third_party/blink/renderer/bindings/core/v8/module_record.cc
// Upstream: third_party/blink/renderer/bindings/core/v8/v8_script_runner.cc

#ifndef MITTO_DOM_MODULE_RECORD_H_
#define MITTO_DOM_MODULE_RECORD_H_

#include <string_view>
#include <vector>

#include "mitto/dom/module_request.h"
#include "mitto/dom/script_fetch_options.h"
#include "url/gurl.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-data.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-script.h"
#include "v8/include/v8-value.h"

namespace mitto {

class ModuleRecord {
 public:
  ModuleRecord() = delete;

  // V8ScriptRunner::CompileModule() in the current context: `source` as a
  // module named `source_url`, which starts at `start_line` and
  // `start_column` of its document, zero-based, and whose import() calls
  // resolve against `base_url` with `options`. Empty, with the error thrown,
  // for one that does not parse.
  static v8::MaybeLocal<v8::Module> Compile(v8::Isolate* isolate,
                                            std::u16string_view source,
                                            const GURL& source_url,
                                            const GURL& base_url,
                                            const ScriptFetchOptions& options,
                                            int start_line,
                                            int start_column);

  // ModuleRecord::Instantiate(): links `record` with the records that the
  // window's module map holds for its requests. Returns what that threw, or
  // an empty value.
  static v8::Local<v8::Value> Instantiate(v8::Local<v8::Context> context,
                                          v8::Local<v8::Module> record);

  // The module requests of `record`, in its order.
  static std::vector<ModuleRequest> ModuleRequests(
      v8::Isolate* isolate,
      v8::Local<v8::Module> record);

  // The import attributes in `v8_import_attributes`, which V8 gives as key,
  // value and position for a record's requests, and as key and value alone
  // for import().
  static std::vector<ImportAttribute> ToImportAttributes(
      v8::Isolate* isolate,
      v8::Local<v8::FixedArray> v8_import_attributes,
      bool has_positions);

 private:
  static v8::MaybeLocal<v8::Module> ResolveModuleCallback(
      v8::Local<v8::Context> context,
      v8::Local<v8::String> specifier,
      v8::Local<v8::FixedArray> import_attributes,
      v8::Local<v8::Module> referrer);
};

}  // namespace mitto

#endif  // MITTO_DOM_MODULE_RECORD_H_
