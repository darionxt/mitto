// The Fetch Standard's Headers: a header list and its guard, as Blink keeps
// them. Iterating over it goes over the list sorted and combined, which an
// iterator takes again after each change, keeping its place, as Blink's
// HeadersIterationSource does.
//
// Upstream: third_party/blink/renderer/core/fetch/headers.h
// Upstream: third_party/blink/renderer/core/fetch/headers.cc
// Upstream: third_party/blink/renderer/core/fetch/fetch_header_list.cc

#ifndef MITTO_DOM_HEADERS_H_
#define MITTO_DOM_HEADERS_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/pair_iterable.h"
#include "mitto/bindings/script_wrappable.h"

namespace mitto {

class Headers final : public bindings::ScriptWrappable {
 public:
  enum class Guard {
    kNone,
    kRequest,
    // A request of mode no-cors, which takes only no-CORS-safelisted lines.
    kRequestNoCors,
    kResponse,
    kImmutable,
  };

  // `new Headers(init)`.
  static Headers* Create(
      const std::optional<
          bindings::ByteStringSequenceSequenceOrByteStringByteStringRecord>&
          init,
      bindings::ExceptionState& exception_state);
  // Empty, with `guard`.
  static Headers* Create(Guard guard);

  explicit Headers(Guard guard);

  // A copy of its lines and its guard, as Blink's FetchHeaderList::Clone()
  // copies them.
  Headers* Clone() const;
  ~Headers() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  void Append(const std::string& name,
              const std::string& value,
              bindings::ExceptionState& exception_state);
  void Remove(const std::string& name,
              bindings::ExceptionState& exception_state);
  std::optional<std::string> Get(const std::string& name,
                                 bindings::ExceptionState& exception_state);
  std::vector<std::string> GetSetCookie() const;
  bool Has(const std::string& name, bindings::ExceptionState& exception_state);
  void Set(const std::string& name,
           const std::string& value,
           bindings::ExceptionState& exception_state);
  bindings::PairIterationSource* CreateIterationSource();

  // Fills an empty Headers from `init`, as its constructor does.
  void FillWith(
      const bindings::ByteStringSequenceSequenceOrByteStringByteStringRecord&
          init,
      bindings::ExceptionState& exception_state);

  // What the Page uses.
  Guard guard() const { return guard_; }
  void set_guard(Guard guard) { guard_ = guard; }
  // Appends a line as it is, as a response's lines go in.
  void AppendLine(std::string name, std::string value);
  // The value of `name`, its values joined with ", ", if it has one.
  std::optional<std::string> GetValue(std::string_view name) const;
  // The list in order, as the lines a request sends: each "name: value" and a
  // line feed.
  std::string ToLines() const;
  const std::vector<std::pair<std::string, std::string>>& list() const {
    return list_;
  }
  // The Fetch Standard's sort and combine: each name lowercased, in order,
  // with its values joined by ", ", but each of Set-Cookie on its own.
  std::vector<std::pair<std::string, std::string>> SortAndCombine() const;

 private:
  class IterationSource;

  // Each change to the list, which tells an iterator to sort and combine
  // again.
  void DidChangeList() { ++changes_; }

  std::vector<std::pair<std::string, std::string>> list_;
  Guard guard_;
  uint64_t changes_ = 0;
};

}  // namespace mitto

#endif  // MITTO_DOM_HEADERS_H_
