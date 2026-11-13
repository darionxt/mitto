"""What the variations seed of a Persona's Chrome Release says of kDeprecateUnload, which takes "unload" out of the
permissions policy of the pages it covers, so that document.featurePolicy.allowedFeatures() leaves it out there.

The seed switches three base::Features of the network service, which no runtime-enabled feature takes: kDeprecateUnload
itself, kDeprecateUnloadByAllowList, which limits it to the hosts of a list, and kDeprecateUnloadByBucket, which limits
it to a share of hosts that each installation's bucket picks. A Persona carries their states and parameters, and a Page
applies them to its document's host as permissions_policy_features_internal.cc does. The hosts of the allow list are in
it only when they change what a page sees: while the buckets take every host, the list is null, and once they do not, it
goes to review (the owner's call, 2026-11-13). docs/persona.md, "What a Page of each Persona turns on and off", says what
the key is for.
"""

# Upstream: services/network/public/cpp/features.cc
# Upstream: services/network/public/cpp/permissions_policy/permissions_policy_features_internal.cc

from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass

from mitto._native import DeprecatedUnload, UnloadBuckets
from tools.release.variations_seed import FeatureSwitch, UndecidedStudy

__all__ = ["DEPRECATE_UNLOAD_FEATURES", "UnloadFromSeed", "deprecated_unload"]

DEPRECATE_UNLOAD = "DeprecateUnload"
BY_ALLOW_LIST = "DeprecateUnloadByAllowList"
BY_BUCKET = "DeprecateUnloadByBucket"
DEPRECATE_UNLOAD_FEATURES = (DEPRECATE_UNLOAD, BY_ALLOW_LIST, BY_BUCKET)

# The features' parameters, with the defaults that features.cc gives them.
_PERCENT = "rollout_percent"
_DEFAULT_PERCENT = 100
_ALLOW_LIST = "allowlist"
_BUCKET = "rollout_bucket"
_DEFAULT_BUCKET = 0


@dataclass(frozen=True, slots=True)
class UnloadFromSeed:
    """What the seed says of kDeprecateUnload for a Persona, and what a person reads of it.

    Attributes:
        unload: What a Page of the Persona applies.
        switched: What the seed switches of the three features, one line each.
        review: What this cannot decide, one line each.
    """

    unload: DeprecatedUnload
    switched: tuple[str, ...] = ()
    review: tuple[str, ...] = ()


def deprecated_unload(switches: Sequence[FeatureSwitch], undecided: Iterable[UndecidedStudy] = ()) -> UnloadFromSeed:
    """Return what most installations of a Persona's Chrome have of kDeprecateUnload, from what the seed switches.

    A feature that the seed does not switch is off, as features.cc declares all three. A study whose filter depends on
    the machine goes to review, as do parameters that most installations do not share, but a bucket of each
    installation's own, which only a rollout to part of the hosts needs.
    """
    first: dict[str, FeatureSwitch] = {}
    for switch in switches:
        if switch.feature in DEPRECATE_UNLOAD_FEATURES:
            # A feature keeps its first override, as in FeatureList::RegisterOverride().
            first.setdefault(switch.feature, switch)
    switched = tuple(
        f"{switch.feature} {'on' if switch.enabled else 'off'}: {switch.study}/{switch.group},"
        f" {switch.share:.0%} of installations"
        for switch in first.values()
    )
    review = [
        f"The seed's study {study.name} switches {', '.join(study.features)} by {', '.join(study.fields)}, which"
        " follows the machine, so the Persona leaves it aside"
        for study in undecided
        if any(feature in DEPRECATE_UNLOAD_FEATURES for feature in study.features)
    ]
    main = first.get(DEPRECATE_UNLOAD)
    if main is None or not main.enabled:
        return UnloadFromSeed(DeprecatedUnload(), switched, tuple(review))
    allow_list: tuple[str, ...] | None = None
    by_allow_list = first.get(BY_ALLOW_LIST)
    if by_allow_list is not None and by_allow_list.enabled:
        review += _unshared(by_allow_list)
        # UnloadDeprecationAllowedHosts(): split on commas, trimmed, with no empty host.
        hosts = (host.strip() for host in by_allow_list.params.get(_ALLOW_LIST, "").split(","))
        allow_list = tuple(host for host in hosts if host)
    by_bucket: UnloadBuckets | None = None
    bucket_switch = first.get(BY_BUCKET)
    if bucket_switch is not None and bucket_switch.enabled:
        review += _unshared(main)
        percent = _int_param(main.params, _PERCENT, _DEFAULT_PERCENT)
        bucket = _int_param(bucket_switch.params, _BUCKET, _DEFAULT_BUCKET) if bucket_switch.params_agreed else None
        if bucket is None and 0 < percent < 100:
            review.append(
                f"The seed deprecates unload on {percent}% of hosts with a bucket of each installation's own, so the"
                " Persona has no bucket and a Page deprecates it on no host that the allow list leaves out"
            )
        by_bucket = UnloadBuckets(percent, bucket)
    if by_bucket is not None and by_bucket.percent == 100:
        # The buckets take every host of http and https, so a host on the list changes nothing that a page sees.
        allow_list = None
    elif allow_list:
        review.append(
            f"The seed deprecates unload on the {len(allow_list)} hosts of {BY_ALLOW_LIST}'s list"
            f"{' and a share of the others' if by_bucket is not None else ''}, so the Persona holds the list: read it"
            " before it goes into the Persona's file"
        )
    return UnloadFromSeed(DeprecatedUnload(True, allow_list, by_bucket), switched, tuple(review))


def _unshared(switch: FeatureSwitch) -> list[str]:
    """Return a line for review when most installations do not share a feature's parameters, which then are those of
    the most."""
    if switch.params_agreed:
        return []
    return [
        f"The seed turns {switch.feature} on for {switch.share:.0%} of installations in {switch.study}, but no"
        " parameters for more than half of them, so the Persona has those of the most"
    ]


def _int_param(params: Mapping[str, str], name: str, default: int) -> int:
    """Return an integer parameter as base::FeatureParam<int> reads it: its default when it is missing or no integer."""
    try:
        return int(params[name])
    except (KeyError, ValueError):
        return default
