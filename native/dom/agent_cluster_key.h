// What keys an agent cluster in a Page, as Blink's agent cluster IDs and
// Chrome's AgentClusterKey do.
//
// Upstream: content/browser/agent_cluster_key.h

#ifndef MITTO_DOM_AGENT_CLUSTER_KEY_H_
#define MITTO_DOM_AGENT_CLUSTER_KEY_H_

#include "url/origin.h"

namespace mitto {

// An origin, as Chrome keys agent clusters by origin by default, and whether
// Document-Isolation-Policy isolates it, which gives it a
// CrossOriginIsolationKey of that origin and keeps it apart from the agent
// cluster of the same origin without one. One that isolates with
// "isolate-and-require-corp" and one with "isolate-and-credentialless" share
// it, as the key compares neither.
struct AgentClusterKey {
  bool operator==(const AgentClusterKey&) const = default;

  url::Origin origin;
  bool isolated_by_document_policy = false;
};

}  // namespace mitto

#endif  // MITTO_DOM_AGENT_CLUSTER_KEY_H_
