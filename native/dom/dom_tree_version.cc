#include "mitto/dom/dom_tree_version.h"

namespace mitto {

namespace {

constinit uint64_t g_dom_tree_version = 0;

}  // namespace

uint64_t DomTreeVersion() {
  return g_dom_tree_version;
}

void IncrementDomTreeVersion() {
  ++g_dom_tree_version;
}

}  // namespace mitto
