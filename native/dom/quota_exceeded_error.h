// Web IDL's QuotaExceededError, the DOMException that says how much was
// asked for and how much there is, which Chrome throws once
// QuotaExceededErrorUpdate is on, as it is for stable, where a DOMException
// named "QuotaExceededError" went before.
//
// Upstream: third_party/blink/renderer/core/dom/quota_exceeded_error.h
// Upstream: third_party/blink/renderer/core/dom/quota_exceeded_error.cc

#ifndef MITTO_DOM_QUOTA_EXCEEDED_ERROR_H_
#define MITTO_DOM_QUOTA_EXCEEDED_ERROR_H_

#include <optional>
#include <string>
#include <string_view>

#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/dom/dom_exception.h"

namespace mitto {

class QuotaExceededError final : public DOMException {
 public:
  // `new QuotaExceededError(message, options)`.
  static QuotaExceededError* Create(
      const std::u16string& message,
      const bindings::QuotaExceededErrorOptions& options);
  static QuotaExceededError* Create(const std::u16string& message,
                                    std::optional<double> quota,
                                    std::optional<double> requested);

  // Throws a QuotaExceededError with the stack of where it is thrown, or a
  // DOMException named "QuotaExceededError" in a context without
  // QuotaExceededErrorUpdate, as Blink's QuotaExceededError::Throw() does.
  static void Throw(bindings::ExceptionState& exception_state,
                    std::string_view message,
                    std::optional<double> quota = std::nullopt,
                    std::optional<double> requested = std::nullopt);

  QuotaExceededError(const std::u16string& message,
                     std::optional<double> quota,
                     std::optional<double> requested);
  ~QuotaExceededError() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  std::optional<double> Quota() const { return quota_; }
  std::optional<double> Requested() const { return requested_; }

 private:
  const std::optional<double> quota_;
  const std::optional<double> requested_;
};

}  // namespace mitto

#endif  // MITTO_DOM_QUOTA_EXCEEDED_ERROR_H_
