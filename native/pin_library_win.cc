#include "mitto/pin_library.h"

#include <windows.h>

#include "base/check.h"

namespace mitto {

void PinLibrary() {
  HMODULE module;
  // Any address in the library names it.
  CHECK(::GetModuleHandleExW(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
      reinterpret_cast<LPCWSTR>(&PinLibrary), &module));
}

}  // namespace mitto
