"""Blink's runtime-enabled features, and which of them Chrome turns on for stable on Windows.

A Page gets a set of features when it starts, and installs what is behind each one as Blink does, so that M7 can give
an older Persona its own set. The set a Page gets by default is the Engine Release's for stable on Windows, worked out
here from runtime_enabled_features.json5 as Blink's build and Chrome's startup work it out:

- A feature starts on if its status for Windows is "stable" (make_runtime_features.py's feature_states_).
- Chrome then copies the state of the base::Feature of the same name into it, unless the feature has none or copies
  only an override (UpdateStatusFromBaseFeatures() in runtime_enabled_features.cc.tmpl). With no field trial and no
  switch, that turns on a feature whose base::Feature is on by default, which base_feature_status says, or else its
  status.
- When Blink asks for a feature, a feature it depends on that is off turns it off, and a feature that implies it and is
  on turns it on (RuntimeEnabledFeatures::FooEnabled() in the template).

What Chrome's own code turns on beside the file, field trials and switches are not counted here, and M7's Baselines
say where they differ. The exceptions are a feature that Chrome sets from the machine, such as whether it has a touch
screen: a Page starts with the value of a desktop without one, until the Machine decides it (M7); and those that
Chrome's content layer or Chrome itself sets when a renderer starts, which the Parity Checks of the Script Fingerprint
show.
"""

# Upstream: third_party/blink/renderer/build/scripts/make_runtime_features.py
# Upstream: third_party/blink/renderer/build/scripts/templates/runtime_enabled_features.h.tmpl
# Upstream: third_party/blink/renderer/build/scripts/templates/runtime_enabled_features.cc.tmpl
# Upstream: third_party/blink/renderer/build/scripts/templates/features_generated.cc.tmpl
# Upstream: third_party/blink/renderer/core/exported/web_view_impl.cc
# Upstream: content/child/runtime_features.cc
# Upstream: content/browser/web_contents/web_contents_impl.cc
# Upstream: chrome/renderer/chrome_content_renderer_client.cc
# Upstream: chrome/browser/chrome_content_browser_client.cc
# Upstream: services/device/public/cpp/device_features.cc

import dataclasses
from collections.abc import Mapping, Sequence
from typing import Any

from bindings_generator.errors import UnhandledError

__all__ = ["Feature", "read_features"]

# The platform whose status counts: every Persona is Windows Chrome.
PLATFORM = "Win"

# The features that Chrome sets from the machine when a renderer starts, whatever their status, and the value a Page
# starts with: that of a desktop without a touch screen. WebViewImpl::ApplyWebPreferences() sets touch event feature
# detection from whether the machine has a touch screen.
MACHINE_DECIDED = {"TouchEventFeatureDetection": False}

# The features that Chrome's content layer, or Chrome itself, turns on when a renderer starts, which
# runtime_enabled_features.json5 does not say:
# - SetRuntimeFeaturesFromChromiumFeatures() copies network::features::kLocalNetworkAccessChecks, on by default in
#   services/network/public/cpp/features.cc, into LocalNetworkAccessPermissionPolicy, which puts targetAddressSpace on
#   Request, features::kServiceWorkerPaymentApps, on by default in content/public/common/content_features.cc, into
#   PaymentApp, which exposes PaymentManager, and device::features::kSerialPortConnected, on by default but on Android in
#   services/device/public/cpp/device_features.cc, into SerialPortConnected, which puts connected on SerialPort.
# - WebContentsImpl::ComputeWebPreferences() sets payment_request_enabled from features::kWebPayments, on by default in
#   content/public/common/content_features.cc, and WebViewImpl::ApplyWebPreferences() copies it into PaymentRequest,
#   which exposes PaymentRequest and the interfaces that go with it.
# - ChromeContentBrowserClient::OverrideWebPreferences() sets translate_service_available from
#   TranslateService::IsAvailable(), true with Chrome's default preferences, and WebViewImpl::ApplyWebPreferences()
#   copies it into TranslateService, on which HrefTranslate depends, which puts hrefTranslate on HTMLAnchorElement.
# - ChromeContentRendererClient::SetRuntimeFeaturesDefaultsBeforeBlinkInitialization() turns on WebShare on Windows,
#   which puts share() and canShare() on Navigator.
# Each release's listing shows where a new one appears, and the Parity Checks of the Script Fingerprint where a Page and
# Chrome differ.
CONTENT_ENABLED = frozenset(
    {
        "LocalNetworkAccessPermissionPolicy",
        "PaymentApp",
        "PaymentRequest",
        "SerialPortConnected",
        "TranslateService",
        "WebShare",
    }
)

# The features that Chrome's content layer turns off when a renderer starts, whatever their status: at the end of
# SetRuntimeFeaturesFromCommandLine(), LegacyAbstractRange is the inverse of OpaqueRange, which is stable in Chrome 155
# to 157, so that AbstractRange.prototype lacks startContainer and its other containers.
CONTENT_DISABLED = frozenset({"LegacyAbstractRange"})

# The keys of a feature that the generator reads or knows to leave aside, with why.
_KEYS = {
    "name": "its name",
    "status": "whether it is on for stable",
    "base_feature": "the base::Feature whose state Chrome copies into it",
    "base_feature_status": "whether that base::Feature is on by default",
    "copied_from_base_feature_if": "when Chrome copies the base::Feature's state",
    "depends_on": "features that must be on for it to be on",
    "implied_by": "features that turn it on",
    "origin_trial_feature_name": "the origin trial that can turn it on in a context, which a Page never has",
    "origin_trial_os": "where its origin trial runs",
    "origin_trial_type": "its kind of origin trial",
    "origin_trial_allows_insecure": "whether its origin trial runs on insecure origins",
    "origin_trial_allows_third_party": "whether its origin trial takes third-party tokens",
    "settable_from_internals": "whether Blink's tests can set it",
    "public": "whether Chrome's code outside Blink can set it",
    "feature_policy": "the permissions policy feature that goes with it",
    "browser_process_read_access": "whether the browser reads its state",
    "browser_process_read_write_access": "whether the browser writes its state",
    "custom_enable_check": "a check that Blink runs beside the flag, for features no Persona has on",
}


@dataclasses.dataclass(frozen=True)
class Feature:
    """One runtime-enabled feature.

    Attributes:
        name: its name, such as "HasUAVisualTransition".
        stable: whether a Page has it on by default, before depends_on and implied_by.
        depends_on: features that must be on for it to be on.
        implied_by: features that turn it on.
    """

    name: str
    stable: bool
    depends_on: tuple[str, ...]
    implied_by: tuple[str, ...]


def _is_stable_on_windows(status: object) -> bool:
    if isinstance(status, Mapping):
        statuses: Mapping[str, Any] = status  # pyright: ignore[reportUnknownVariableType]
        return statuses.get(PLATFORM, statuses.get("default", "")) == "stable"
    return status == "stable"


def read_features(entries: Sequence[Mapping[str, Any]]) -> list[Feature]:
    """Reads the features of runtime_enabled_features.json5's `data`, in its order.

    Raises:
        UnhandledError: if a feature has a key the generator does not know, or names a feature that is not there.
    """
    features: list[Feature] = []
    for entry in entries:
        name = str(entry["name"])
        for key in entry:
            if key not in _KEYS:
                raise UnhandledError(f"runtime-enabled feature {name}: the generator does not handle the key {key}")
        stable = _is_stable_on_windows(entry.get("status", ""))
        base_feature = entry.get("base_feature", "")
        copies_default = entry.get("copied_from_base_feature_if", "enabled_or_overridden") == "enabled_or_overridden"
        if base_feature != "none" and copies_default and entry.get("base_feature_status", "") == "enabled":
            stable = True
        stable = MACHINE_DECIDED.get(name, (stable or name in CONTENT_ENABLED) and name not in CONTENT_DISABLED)
        features.append(
            Feature(
                name=name,
                stable=stable,
                depends_on=tuple(str(item) for item in entry.get("depends_on", ())),
                implied_by=tuple(str(item) for item in entry.get("implied_by", ())),
            )
        )
    names = {feature.name for feature in features}
    for feature in features:
        for other in feature.depends_on + feature.implied_by:
            if other not in names:
                raise UnhandledError(f"runtime-enabled feature {feature.name} names {other}, which is not a feature")
    return features
