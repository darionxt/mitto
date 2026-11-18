// Import maps as Chrome parses, merges and resolves them: the JSON text of a
// <script type="importmap"> made into its imports, scopes and integrity, with
// the console warnings Blink gives for each entry it ignores and the error it
// throws for a map it cannot take; the merge of a new map into a document's
// map; and the resolution of a module specifier through the map.
//
// Blink parses the JSON with its own parser, whose objects keep their keys in
// the order they first came in and the last value given for each, and that
// order decides which of two keys normalized to the same one wins and the
// order of the warnings, so this file carries a port of that parser too.
//
// Upstream: third_party/blink/renderer/core/script/import_map.h
// Upstream: third_party/blink/renderer/core/script/import_map.cc
// Upstream: third_party/blink/renderer/core/script/import_map_error.h
// Upstream: third_party/blink/renderer/core/script/import_map_error.cc
// Upstream: third_party/blink/renderer/platform/json/json_parser.cc

#ifndef MITTO_DOM_IMPORT_MAP_H_
#define MITTO_DOM_IMPORT_MAP_H_

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "base/functional/function_ref.h"
#include "url/gurl.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-local-handle.h"

namespace mitto {

class ParsedSpecifier;

// What ImportMap::Parse() throws, kept apart from V8 until the script element
// reports it: a TypeError or a SyntaxError with Chrome's message.
struct ImportMapError {
  enum class Type {
    kTypeError,
    kSyntaxError,
  };

  // The exception to report, made in the isolate's current context.
  v8::Local<v8::Value> ToV8(v8::Isolate* isolate) const;

  Type type = Type::kTypeError;
  std::string message;
};

// https://html.spec.whatwg.org/C#import-maps
class ImportMap final {
 public:
  // Takes each console warning Blink gives, in Blink's order. The functor
  // must take a std::string, which base::FunctionRef wants exactly.
  using Warn = base::FunctionRef<void(std::string message)>;

  // A specifier map, an ordered map from strings to resolution results, in
  // which an invalid GURL is a null result. Blink keeps one in a HashMap,
  // whose order no resolution depends on; this keeps the HTML Standard's
  // order, its keys descending by code units, which only
  // ToStringForTesting() and the order of a merge's warnings show.
  using SpecifierMap = std::map<std::u16string, GURL, std::greater<>>;

  // The scopes by their prefixes, descending by code units, the order Blink
  // sorts them in to try them.
  using ScopesMap = std::map<std::u16string, SpecifierMap, std::greater<>>;

  // The integrity metadata of module URLs.
  using IntegrityMap = std::map<GURL, std::u16string>;

  // https://html.spec.whatwg.org/C#parse-an-import-map-string: `text` parsed
  // as an import map whose relative URLs resolve against `base_url`. Entries
  // it ignores go to `warn`, as Blink sends them to the console. For a map it
  // cannot take, it sets `*error_to_rethrow` and gives an empty map, never
  // null.
  static std::unique_ptr<ImportMap> Parse(
      std::u16string_view text,
      const GURL& base_url,
      Warn warn,
      std::optional<ImportMapError>* error_to_rethrow);

  // An empty import map.
  ImportMap();
  ImportMap(SpecifierMap imports, ScopesMap scopes, IntegrityMap integrity);
  ImportMap(const ImportMap&) = delete;
  ImportMap& operator=(const ImportMap&) = delete;
  ~ImportMap();

  // https://html.spec.whatwg.org/C#resolve-a-module-specifier, the steps
  // that go through the import map: nullopt when nothing matches, which
  // falls back to resolving without the map; an invalid GURL when a match
  // blocks the specifier, which throws a TypeError; the mapped URL
  // otherwise. `debug_message` gets what Blink logs of how it went.
  std::optional<GURL> Resolve(const ParsedSpecifier& parsed_specifier,
                              const GURL& base_url,
                              std::string* debug_message) const;

  // The integrity metadata that the map gives `module_url`, or nullopt for
  // none, Blink's null String.
  std::optional<std::u16string> ResolveIntegrity(const GURL& module_url) const;

  // https://html.spec.whatwg.org/C/#merge-existing-and-new-import-maps:
  // merges `new_import_map` into this map, which keeps its own rules where
  // the two conflict. Rules of the new map for specifiers that already
  // resolved are dropped: `scoped_resolved_module_map` holds, by scope
  // prefix, the specifiers and specifier prefixes resolved in that scope, and
  // `toplevel_resolved_module_set` those resolved at the top level.
  //
  // Blink sends these warnings as JavaScript-source warnings with
  // discard_duplicates, so its console drops one whose text it already holds
  // from earlier on the page; the caller does that for `warn`.
  void MergeExistingAndNewImportMaps(
      std::unique_ptr<ImportMap> new_import_map,
      const std::map<std::u16string, std::set<std::u16string>>&
          scoped_resolved_module_map,
      const std::set<std::u16string>& toplevel_resolved_module_set,
      Warn warn);

  // The map as Blink's ImportMap::ToStringForTesting() writes it.
  std::string ToStringForTesting() const;

 private:
  // https://html.spec.whatwg.org/C#concept-import-map-imports
  SpecifierMap imports_;

  // https://html.spec.whatwg.org/C#concept-import-map-scopes
  ScopesMap scopes_;

  // https://html.spec.whatwg.org/C#concept-import-map-integrity
  IntegrityMap integrity_;
};

}  // namespace mitto

#endif  // MITTO_DOM_IMPORT_MAP_H_
