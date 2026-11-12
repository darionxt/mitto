// The Fetch Standard's Headers: a header list and its guard, as Blink keeps
// them. Iterating over it is a Known Gap.
//
// Upstream: third_party/blink/renderer/core/fetch/headers.h
// Upstream: third_party/blink/renderer/core/fetch/headers.cc
// Upstream: third_party/blink/renderer/core/fetch/fetch_header_list.cc

#ifndef MITTO_DOM_HEADERS_H_
#define MITTO_DOM_HEADERS_H_

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"

namespace mitto {

class Headers final : public bindings::ScriptWrappable {
 public:
  enum class Guard {
    kNone,
    kRequest,
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

 private:
  std::vector<std::pair<std::string, std::string>> list_;
  Guard guard_;
};

}  // namespace mitto

#endif  // MITTO_DOM_HEADERS_H_
