// The URL Standard's URLSearchParams: a list of names and values, from a query
// or from what its constructor takes, which writes itself back into its URL
// object's query on each change, as Blink's does. Iterating over it is a Known
// Gap until the Runtime has pair iterators.
//
// Upstream: third_party/blink/renderer/core/url/url_search_params.h
// Upstream: third_party/blink/renderer/core/url/url_search_params.cc
// Upstream: third_party/blink/renderer/platform/network/form_data_encoder.cc

#ifndef MITTO_DOM_URL_SEARCH_PARAMS_H_
#define MITTO_DOM_URL_SEARCH_PARAMS_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class URL;

class URLSearchParams final : public bindings::ScriptWrappable {
 public:
  // `new URLSearchParams(init)`.
  static URLSearchParams* Create(
      const bindings::
          USVStringSequenceSequenceOrUSVStringUSVStringRecordOrUSVString& init,
      bindings::ExceptionState& exception_state);
  // The parameters of `query`, which has no "?", of `url_object` if any.
  static URLSearchParams* Create(const std::u16string& query, URL* url_object);

  URLSearchParams(const std::u16string& query, URL* url_object);
  ~URLSearchParams() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  uint32_t Size() const;
  void Append(const std::u16string& name, const std::u16string& value);
  // delete(), which removes those of `name`, and with a value only those that
  // have it, since URLSearchParamsHasAndDeleteMultipleArgs is on for stable.
  void DeleteAllWithNameOrTuple(const std::u16string& name,
                                const std::optional<std::u16string>& value);
  std::optional<std::u16string> Get(const std::u16string& name) const;
  std::vector<std::u16string> GetAll(const std::u16string& name) const;
  bool Has(const std::u16string& name,
           const std::optional<std::u16string>& value) const;
  void Set(const std::u16string& name, const std::u16string& value);
  void Sort();
  // The list as application/x-www-form-urlencoded.
  std::u16string ToString() const;

  // Replaces the list with the parameters of `query`, which has no "?",
  // without writing it back into the URL object, as its URL object does when
  // its query changes.
  void SetInputWithoutUpdate(std::u16string_view query);

 private:
  void AppendWithoutUpdate(const std::u16string& name,
                           const std::u16string& value);
  // The URL Standard's update steps: the URL object's query becomes the
  // list's serialization.
  void RunUpdateSteps();

  std::vector<std::pair<std::u16string, std::u16string>> params_;
  cppgc::Member<URL> url_object_;
};

}  // namespace mitto

#endif  // MITTO_DOM_URL_SEARCH_PARAMS_H_
