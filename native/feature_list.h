#ifndef MITTO_FEATURE_LIST_H_
#define MITTO_FEATURE_LIST_H_

namespace mitto {

// Sets up base::FeatureList with the state of each feature that Chrome gets
// from a field trial and a source build does not, and of each feature that
// decides what a TLS handshake over TCP sends. The list is per process, so it
// cannot differ per Session or Persona. What has to differ goes through a
// Session's own settings. Call this once, before anything reads a feature.
void InitFeatureList();

}  // namespace mitto

#endif  // MITTO_FEATURE_LIST_H_
