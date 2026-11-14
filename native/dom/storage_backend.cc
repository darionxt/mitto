#include "mitto/dom/storage_backend.h"

namespace mitto {

namespace {

StorageBackend* g_backend = nullptr;

}  // namespace

// static
StorageBackend* StorageBackend::Get() {
  return g_backend;
}

// static
void StorageBackend::Set(StorageBackend* backend) {
  g_backend = backend;
}

}  // namespace mitto
