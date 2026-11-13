// A number that changes each time a child is inserted into or removed from
// any node, or an attribute of any element changes, so that a live
// collection can keep the elements it found until the tree changes, as
// Blink's collections keep theirs until their caches are invalidated.
//
// A Page runs one thread, so one number serves every document.

#ifndef MITTO_DOM_DOM_TREE_VERSION_H_
#define MITTO_DOM_DOM_TREE_VERSION_H_

#include <stdint.h>

namespace mitto {

uint64_t DomTreeVersion();
void IncrementDomTreeVersion();

}  // namespace mitto

#endif  // MITTO_DOM_DOM_TREE_VERSION_H_
