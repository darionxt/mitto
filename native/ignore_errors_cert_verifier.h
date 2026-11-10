// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file of Chromium's source tree.
//
// Ported from services/network/ignore_errors_cert_verifier.h, whose build
// target brings in the whole network service. SPKIHashSet comes from
// services/network/public/cpp/spki_hash_set.h. Reading the allowlist from the
// command line and the hook for testing are left out.
//
// Upstream: services/network/ignore_errors_cert_verifier.h
// Upstream: services/network/public/cpp/spki_hash_set.h

#ifndef MITTO_IGNORE_ERRORS_CERT_VERIFIER_H_
#define MITTO_IGNORE_ERRORS_CERT_VERIFIER_H_

#include <memory>
#include <string>

#include "base/containers/flat_set.h"
#include "base/functional/callback_forward.h"
#include "base/memory/scoped_refptr.h"
#include "net/base/completion_once_callback.h"
#include "net/base/hash_value.h"
#include "net/cert/cert_verifier.h"

namespace net {
class CertVerifyResult;
class NetLogWithSource;
class X509Certificate;
}  // namespace net

namespace mitto {

// SPKIHashSet is a set of SHA-256 SPKI fingerprints (RFC 7469, Section 2.4).
using SPKIHashSet = base::flat_set<net::SHA256HashValue>;

// IgnoreErrorsCertVerifier wraps another CertVerifier in order to ignore
// verification errors from certificate chains that match a allowlist of SPKI
// fingerprints.
class IgnoreErrorsCertVerifier : public net::CertVerifier {
 public:
  IgnoreErrorsCertVerifier(std::unique_ptr<net::CertVerifier> verifier,
                           SPKIHashSet allowlist);

  IgnoreErrorsCertVerifier(const IgnoreErrorsCertVerifier&) = delete;
  IgnoreErrorsCertVerifier& operator=(const IgnoreErrorsCertVerifier&) = delete;

  ~IgnoreErrorsCertVerifier() override;

  // Verify skips certificate verification and returns OK if any of the
  // certificates from the chain in |params| match one of the SPKI fingerprints
  // from the allowlist. Otherwise, it invokes Verify on the wrapped verifier
  // and returns the result.
  int Verify(const RequestParams& params,
             net::CertVerifyResult* verify_result,
             net::CompletionOnceCallback callback,
             std::unique_ptr<Request>* out_req,
             const net::NetLogWithSource& net_log) override;
  void Verify2QwacBinding(
      const std::string& binding,
      const std::string& hostname,
      const scoped_refptr<net::X509Certificate>& tls_cert,
      base::OnceCallback<void(const scoped_refptr<net::X509Certificate>&)>
          callback,
      const net::NetLogWithSource& net_log) override;
  void SetConfig(const Config& config) override;
  void AddObserver(Observer* observer) override;
  void RemoveObserver(Observer* observer) override;

 private:
  std::unique_ptr<net::CertVerifier> verifier_;
  SPKIHashSet allowlist_;
};

}  // namespace mitto

#endif  // MITTO_IGNORE_ERRORS_CERT_VERIFIER_H_
