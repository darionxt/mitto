// Web Cryptography's Crypto, the window's `crypto`: getRandomValues() and
// randomUUID() from the system's random numbers, as Chrome's renderer takes
// them. `subtle` gives the window's SubtleCrypto, whose members stay Known
// Gaps that throw until the Runtime has WebCrypto (roadmap, M10).
//
// Upstream: third_party/blink/renderer/modules/crypto/crypto.h
// Upstream: third_party/blink/renderer/modules/crypto/crypto.cc
// Upstream: third_party/blink/renderer/platform/wtf/uuid.cc

#ifndef MITTO_DOM_CRYPTO_H_
#define MITTO_DOM_CRYPTO_H_

#include <string>

#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-local-handle.h"

namespace mitto {

class Crypto final : public bindings::ScriptWrappable {
 public:
  static Crypto* Create();

  Crypto();
  ~Crypto() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  v8::Local<v8::ArrayBufferView> GetRandomValues(
      v8::Local<v8::ArrayBufferView> array,
      bindings::ExceptionState& exception_state);
  std::u16string RandomUUID();
};

}  // namespace mitto

#endif  // MITTO_DOM_CRYPTO_H_
