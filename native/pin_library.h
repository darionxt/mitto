#ifndef MITTO_PIN_LIBRARY_H_
#define MITTO_PIN_LIBRARY_H_

namespace mitto {

// Keeps the library loaded until the process exits, because its network thread
// never stops: FreeLibrary() must not unload its code. On Linux, -z nodelete
// does this, and on macOS dyld never unloads the library. Windows only.
void PinLibrary();

}  // namespace mitto

#endif  // MITTO_PIN_LIBRARY_H_
