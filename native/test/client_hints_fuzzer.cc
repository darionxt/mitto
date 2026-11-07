// Feeds any bytes to the code that reads which client hints a server asks for.
// They come from outside the library, in a response's headers and in ALPS.

#include "mitto/client_hints.h"

#include <stddef.h>
#include <stdint.h>

#include <optional>
#include <string>

#include "base/check.h"
#include "base/compiler_specific.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // SAFETY: libFuzzer says `data` holds `size` bytes.
  const std::string header =
      UNSAFE_BUFFERS(std::string(reinterpret_cast<const char*>(data), size));
  const std::optional<mitto::EnabledClientHints> hints =
      mitto::ParseClientHintsHeader(header);
  if (hints) {
    for (const std::string& hint : *hints) {
      CHECK(mitto::IsClientHintHeader(hint));
    }
  }
  return 0;
}
