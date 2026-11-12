"""The variations seed of a Chrome Release, and what its field trials switch for most installations of a Persona's Chrome.

Chrome fetches its seed from Google's variations server, which cuts one for each milestone, and applies the field trials
of the seed from its next start. Baselines are recorded with the seed (docs/baselines.md), so a Persona's Chrome runs
with what the seed turns on and off. This reads the seed's protocol buffer, keeps the studies that a Persona's client
takes, as Chrome's study filtering does, and works out which features most installations have switched: those that the
groups of a study switch for more than half of all installations. A study that splits installations, such as one with
a group of 1%, leaves most of them in its default group, as a Baseline is recorded again until it draws that group.
"""

# Upstream: components/variations/proto/variations_seed.proto
# Upstream: components/variations/proto/study.proto
# Upstream: components/variations/proto/layer.proto
# Upstream: components/variations/study_filtering.cc
# Upstream: components/variations/variations_layers.cc
# Upstream: gin/v8_initializer.cc

import base64
import binascii
import datetime
import urllib.parse
from collections.abc import Iterator, Mapping
from dataclasses import dataclass, field
from enum import IntEnum

from tools.release.web import Fetch, WebError

__all__ = [
    "SEED_URL",
    "Client",
    "Experiment",
    "FeatureSwitch",
    "Filter",
    "Layer",
    "SeedError",
    "Study",
    "UndecidedStudy",
    "VariationsSeed",
    "fetch_seed",
    "majority_switches",
    "read_seed",
    "seed_url",
    "serial_version",
    "v8_flags",
]

SEED_URL = "https://clientservices.googleapis.com/chrome-variations/seed"
"""Where Chrome fetches its variations seed. The query names the platform, the channel and the milestone, nothing more."""

# The prefix of a feature whose state gin turns into a V8 flag of the rest of its name, and the parameter that gives the
# flag a value (V8FeatureVisitor in gin/v8_initializer.cc).
_V8_FLAG_PREFIX = "V8Flag_"
_V8_FLAG_PARAM = "V8FlagParam"


class SeedError(Exception):
    """A variations seed cannot be read, or holds what this does not know how to evaluate."""


class Platform(IntEnum):
    """Study.Platform."""

    WINDOWS = 0


class Channel(IntEnum):
    """Study.Channel."""

    STABLE = 3


class FormFactor(IntEnum):
    """Study.FormFactor."""

    DESKTOP = 0


class CpuArchitecture(IntEnum):
    """Study.CpuArchitecture."""

    X86_64 = 0


class _PolicyRestriction(IntEnum):
    """Study.PolicyRestriction."""

    NONE = 0
    CRITICAL = 1
    CRITICAL_ONLY = 2


@dataclass(frozen=True, slots=True)
class Client:
    """What a Persona's Chrome tells its study filtering about itself.

    Attributes:
        version: The build, such as "157.0.8103.39".
        locale: Chrome's language, which --lang sets for the Baselines.
        country: The country of both consistencies, which --variations-override-country sets for the Baselines.
        date: The day the seed is evaluated on, against each study's start and end.
    """

    version: str
    locale: str
    country: str
    date: datetime.date


@dataclass(frozen=True, slots=True)
class Filter:
    """Study.Filter: which clients a study is for. Fields the evaluation cannot decide for a Persona are in undecided.

    Attributes:
        start_date: Seconds since the epoch from which the study applies, or None.
        end_date: Seconds since the epoch up to which it applies, or None.
        min_version: The oldest build it applies to, which may end in ".*", or None.
        max_version: The newest build it applies to, which may end in ".*", or None.
        channels: The channels it applies to, or none for all.
        platforms: The platforms it applies to. A filter that names none applies to none.
        locales: The locales it applies to, or none for all.
        exclude_locales: The locales it does not apply to, read only without locales.
        form_factors: The form factors it applies to, or none for all.
        exclude_form_factors: The form factors it does not apply to, read only without form_factors.
        countries: The countries it applies to, or none for all.
        exclude_countries: The countries it does not apply to, read only without countries.
        cpu_architectures: The processors it applies to, or none for all.
        exclude_cpu_architectures: The processors it does not apply to, read only without cpu_architectures.
        is_low_end_device: Whether it applies only to low-end devices, or only to others, or None for both.
        is_enterprise: Whether it applies only to managed clients, or only to others, or None for both.
        policy_restriction: Which clients by their policy on variations, as Study.PolicyRestriction.
        google_groups: Google's groups it applies to only, which a Persona is never in.
        enterprise_groups: Enterprise groups it applies to only, which a Persona is never in.
        undecided: The names of the fields set that depend on the machine, such as the operating system's version,
            which a Persona does not decide.
    """

    start_date: int | None = None
    end_date: int | None = None
    min_version: str | None = None
    max_version: str | None = None
    channels: tuple[int, ...] = ()
    platforms: tuple[int, ...] = ()
    locales: tuple[str, ...] = ()
    exclude_locales: tuple[str, ...] = ()
    form_factors: tuple[int, ...] = ()
    exclude_form_factors: tuple[int, ...] = ()
    countries: tuple[str, ...] = ()
    exclude_countries: tuple[str, ...] = ()
    cpu_architectures: tuple[int, ...] = ()
    exclude_cpu_architectures: tuple[int, ...] = ()
    is_low_end_device: bool | None = None
    is_enterprise: bool | None = None
    policy_restriction: int = _PolicyRestriction.NONE
    google_groups: tuple[int, ...] = ()
    enterprise_groups: tuple[str, ...] = ()
    undecided: tuple[str, ...] = ()


@dataclass(frozen=True, slots=True)
class Experiment:
    """Study.Experiment, a group of a study, and the features it switches.

    Attributes:
        name: Its name.
        weight: Its share of the study, against the sum of every group's.
        enable_features: The features it turns on.
        disable_features: The features it turns off.
        params: Its parameters, by name.
    """

    name: str
    weight: int
    enable_features: tuple[str, ...] = ()
    disable_features: tuple[str, ...] = ()
    params: Mapping[str, str] = field(default_factory=dict[str, str])


@dataclass(frozen=True, slots=True)
class Study:
    """Study, with the fields that decide which features its groups switch for whom.

    Attributes:
        name: Its name.
        consistency: 0 for session, 1 for permanent.
        experiments: Its groups.
        filter: Which clients it is for, or None for every client.
        layer_id: The layer that it is a member of, or None.
        layer_members: The members of that layer that it is.
    """

    name: str
    consistency: int
    experiments: tuple[Experiment, ...]
    filter: Filter | None
    layer_id: int | None = None
    layer_members: tuple[int, ...] = ()


@dataclass(frozen=True, slots=True)
class Layer:
    """Layer: slots that its members share, so that a client is in at most one member.

    Attributes:
        id: Its ID.
        slots: How many slots it has.
        members: The slots of each member, by the member's ID, as how many of the layer's slots it holds.
    """

    id: int
    slots: int
    members: Mapping[int, int]


@dataclass(frozen=True, slots=True)
class VariationsSeed:
    """VariationsSeed.

    Attributes:
        serial_number: Its serial number.
        version: The version of the seed, such as "20261111-170049.393000-production", which every milestone's cut has.
        studies: Its studies, in order.
        layers: Its layers, by ID.
    """

    serial_number: str
    version: str
    studies: tuple[Study, ...]
    layers: Mapping[int, Layer]


@dataclass(frozen=True, slots=True)
class FeatureSwitch:
    """A feature that most installations of a Persona's Chrome have switched, and the study that switches it.

    Attributes:
        feature: The feature's name.
        enabled: Whether it is on, or else off.
        study: The study that switches it.
        group: The group that most installations draw, or the groups that switch it alike, joined with "+".
        share: The share of installations that have it switched so, from 0 to 1.
        params: The group's parameters, for a feature that is on.
    """

    feature: str
    enabled: bool
    study: str
    group: str
    share: float
    params: Mapping[str, str] = field(default_factory=dict[str, str])


@dataclass(frozen=True, slots=True)
class UndecidedStudy:
    """A study that would apply to a Persona's Chrome but for a filter that depends on the machine.

    Attributes:
        name: Its name.
        fields: The fields of its filter that depend on the machine, such as "min_os_version".
        features: The features that its groups switch.
    """

    name: str
    fields: tuple[str, ...]
    features: tuple[str, ...]


def seed_url(milestone: int) -> str:
    """Return where Chrome of a milestone on Windows, on the stable channel, fetches its seed."""
    query = urllib.parse.urlencode({"osname": "win", "channel": "stable", "milestone": str(milestone)})
    return f"{SEED_URL}?{query}"


def fetch_seed(milestone: int, fetch: Fetch) -> VariationsSeed:
    """Return the seed that the variations server gives Chrome of a milestone on Windows, today.

    Raises:
        WebError: The server did not answer, or its answer is not a seed.
    """
    url = seed_url(milestone)
    try:
        return read_seed(fetch(url))
    except SeedError as error:
        raise WebError(f"{url} did not give a variations seed: {error}") from error


def read_seed(data: bytes) -> VariationsSeed:
    """Return the seed in a VariationsSeed protocol buffer.

    Raises:
        SeedError: The bytes are not one.
    """
    serial_number = version = ""
    studies: list[Study] = []
    layers: dict[int, Layer] = {}
    for number, value in _fields(data):
        if number == 1:
            serial_number = _text(value)
        elif number == 2:
            studies.append(_study(_bytes(value)))
        elif number == 4:
            version = _text(value)
        elif number == 6:
            layer = _layer(_bytes(value))
            layers[layer.id] = layer
    if not studies:
        raise SeedError("it has no study")
    return VariationsSeed(serial_number=serial_number, version=version, studies=tuple(studies), layers=layers)


def serial_version(serial_number: str) -> str | None:
    """Return the version of the seed that a serial number names, such as "20261111-170049.393000", or None.

    A seed's serial number, which Chrome keeps as variations_seed_serial_number and chrome://version shows, is "SM" and
    the base64 of a message whose first field is the seed's version without its suffix, then "#" and a hash. So two
    serial numbers of one version can differ before the "#" while the studies are the same (baselines.md).
    """
    body = serial_number.split("#", 1)[0]
    if not body.startswith("SM"):
        return None
    try:
        fields = dict(_fields(base64.b64decode(body.removeprefix("SM"), validate=True)))
        return _text(fields[1])
    except (binascii.Error, KeyError, SeedError):
        return None


def majority_switches(seed: VariationsSeed, client: Client) -> tuple[list[FeatureSwitch], list[UndecidedStudy]]:
    """Return the features that most installations of the client's Chrome have switched, and the studies left aside.

    A feature counts when the study that switches it, as Chrome takes the first study of each name that applies, switches
    it alike for more than half of all installations: its groups' share times the share of a layer's slots it holds.

    Returns:
        The switched features, in the order of their studies, and the studies that switch a feature and whose filter
        depends on what a Persona does not decide, such as the version of Windows.
    """
    switches: list[FeatureSwitch] = []
    undecided: list[UndecidedStudy] = []
    taken: set[str] = set()
    for study in seed.studies:
        if study.name in taken or not _applies(study, client, seed.layers):
            continue
        if study.filter is not None and study.filter.undecided:
            features = [
                feature
                for experiment in study.experiments
                for feature in experiment.enable_features + experiment.disable_features
            ]
            if features:
                undecided.append(UndecidedStudy(study.name, study.filter.undecided, tuple(dict.fromkeys(features))))
            continue
        taken.add(study.name)
        switches += _study_switches(study, _layer_share(study, seed.layers))
    return switches, undecided


def v8_flags(switches: list[FeatureSwitch]) -> list[str]:
    """Return the V8 flags that gin sets for the V8Flag_ features among switches, in order, as gin writes them."""
    flags: list[str] = []
    for switch in switches:
        if not switch.feature.startswith(_V8_FLAG_PREFIX):
            continue
        name = switch.feature.removeprefix(_V8_FLAG_PREFIX)
        if not switch.enabled:
            flags.append(f"--no-{name}")
            continue
        # FieldTrialParams is a std::map, so its parameters come in the order of their names' bytes.
        params = sorted(switch.params.items())
        if params and params[0][0] == _V8_FLAG_PARAM:
            flags.append(f"--{name}={params.pop(0)[1]}")
        else:
            flags.append(f"--{name}")
        flags += [f"--{param}={value}" for param, value in params]
    return flags


def _applies(study: Study, client: Client, layers: Mapping[int, Layer]) -> bool:
    """Return whether a study can apply to the client's Chrome, as ShouldAddStudy() decides it but for the machine."""
    if study.layer_id is not None and _layer_share(study, layers) == 0:
        return False
    selected = study.filter
    if selected is None:
        return True
    if selected.policy_restriction == _PolicyRestriction.CRITICAL_ONLY:
        return False
    timestamp = int(datetime.datetime.combine(client.date, datetime.time(), datetime.UTC).timestamp())
    checks = (
        not selected.channels or Channel.STABLE in selected.channels,
        _included(FormFactor.DESKTOP, selected.form_factors, selected.exclude_form_factors),
        _included(CpuArchitecture.X86_64, selected.cpu_architectures, selected.exclude_cpu_architectures),
        _included(client.locale, selected.locales, selected.exclude_locales),
        Platform.WINDOWS in selected.platforms,
        selected.min_version is None or _compare_version(client.version, selected.min_version) >= 0,
        selected.max_version is None or _compare_version(client.version, selected.max_version) <= 0,
        selected.start_date is None or timestamp >= selected.start_date,
        selected.end_date is None or selected.end_date >= timestamp,
        selected.is_low_end_device in (None, False),
        _included(client.country, selected.countries, selected.exclude_countries),
        selected.is_enterprise in (None, False),
        not selected.google_groups,
        not selected.enterprise_groups,
    )
    return all(checks)


def _included[T](value: T, included: tuple[T, ...], excluded: tuple[T, ...]) -> bool:
    if included:
        return value in included
    return value not in excluded


def _layer_share(study: Study, layers: Mapping[int, Layer]) -> float:
    """Return the share of installations in the layer members that a study is, or 1 for a study in no layer."""
    if study.layer_id is None:
        return 1.0
    layer = layers.get(study.layer_id)
    if layer is None or layer.slots == 0:
        return 0.0
    held = sum(layer.members.get(member, 0) for member in study.layer_members)
    return min(held, layer.slots) / layer.slots


def _study_switches(study: Study, share: float) -> list[FeatureSwitch]:
    """Return what a study switches for more than half of all installations, of which `share` take it."""
    total = sum(experiment.weight for experiment in study.experiments)
    if total == 0 or share == 0:
        return []
    states: dict[tuple[str, bool], list[Experiment]] = {}
    for experiment in study.experiments:
        if experiment.weight == 0:
            continue
        for feature in experiment.enable_features:
            states.setdefault((feature, True), []).append(experiment)
        for feature in experiment.disable_features:
            states.setdefault((feature, False), []).append(experiment)
    switches: list[FeatureSwitch] = []
    for (feature, enabled), experiments in states.items():
        feature_share = share * sum(experiment.weight for experiment in experiments) / total
        if feature_share <= 0.5:
            continue
        largest = max(experiments, key=lambda experiment: experiment.weight)
        switches.append(
            FeatureSwitch(
                feature=feature,
                enabled=enabled,
                study=study.name,
                group="+".join(experiment.name for experiment in experiments),
                share=feature_share,
                params=dict(largest.params) if enabled else {},
            )
        )
    return switches


def _compare_version(version: str, other: str) -> int:
    """Compare a build with a version that may end in ".*", as base::Version::CompareToWildcardString() does."""
    wildcard = other.endswith(".*")
    theirs = [int(part) for part in other.removesuffix(".*").split(".")]
    ours = [int(part) for part in version.split(".")]
    if wildcard:
        ours = ours[: len(theirs)] + [0] * (len(theirs) - len(ours))
    length = max(len(ours), len(theirs))
    ours += [0] * (length - len(ours))
    theirs += [0] * (length - len(theirs))
    return (ours > theirs) - (ours < theirs)


# The fields of Study.Filter that depend on the machine rather than on the Persona.
_UNDECIDED_FILTER_FIELDS = {
    8: "hardware_class",
    9: "exclude_hardware_class",
    16: "min_os_version",
    17: "max_os_version",
    24: "hardware_manufacturer",
    25: "exclude_hardware_manufacturer",
}

# Fields of Study.Filter that this reads or knows to leave aside: exclude_google_group and exclude_enterprise_group,
# which a Persona is never in.
_READ_FILTER_FIELDS = frozenset({1, 2, 3, 4, 5, 6, 7, 10, 11, 12, 13, 14, 15, 18, 19, 20, 21, 22, 23, 26, 27})


def _study(data: bytes) -> Study:
    name = ""
    consistency = 0
    experiments: list[Experiment] = []
    selected: Filter | None = None
    layer_id: int | None = None
    layer_members: list[int] = []
    for number, value in _fields(data):
        if number == 1:
            name = _text(value)
        elif number == 7:
            consistency = _number(value)
        elif number == 9:
            experiments.append(_experiment(_bytes(value)))
        elif number == 10:
            selected = _filter(_bytes(value))
        elif number == 16:
            layer_id, layer_members = _layer_reference(_bytes(value))
    return Study(
        name=name,
        consistency=consistency,
        experiments=tuple(experiments),
        filter=selected,
        layer_id=layer_id,
        layer_members=tuple(layer_members),
    )


def _experiment(data: bytes) -> Experiment:
    name = ""
    weight = 0
    enable: list[str] = []
    disable: list[str] = []
    params: dict[str, str] = {}
    for number, value in _fields(data):
        if number == 1:
            name = _text(value)
        elif number == 2:
            weight = _number(value)
        elif number == 6:
            param = {key: _text(item) for key, item in _fields(_bytes(value))}
            params[param.get(1, "")] = param.get(2, "")
        elif number == 12:
            for key, item in _fields(_bytes(value)):
                if key == 1:
                    enable.append(_text(item))
                elif key == 2:
                    disable.append(_text(item))
    return Experiment(
        name=name, weight=weight, enable_features=tuple(enable), disable_features=tuple(disable), params=params
    )


def _filter(data: bytes) -> Filter:
    texts: dict[int, list[str]] = {}
    numbers: dict[int, list[int]] = {}
    undecided: list[str] = []
    for number, value in _fields(data):
        if number in _UNDECIDED_FILTER_FIELDS:
            undecided.append(_UNDECIDED_FILTER_FIELDS[number])
        elif number not in _READ_FILTER_FIELDS:
            raise SeedError(f"a study's filter has field {number}, which this does not know")
        elif number in (2, 3, 6, 10, 11, 12, 26, 27):
            texts.setdefault(number, []).append(_text(value))
        elif isinstance(value, bytes):
            # A packed repeated field of numbers.
            numbers.setdefault(number, []).extend(_packed(value))
        else:
            numbers.setdefault(number, []).append(value)

    def single(number: int) -> int | None:
        return numbers[number][-1] if number in numbers else None

    def flag(number: int) -> bool | None:
        value = single(number)
        return None if value is None else bool(value)

    return Filter(
        start_date=single(1),
        end_date=single(13),
        min_version=texts[2][-1] if 2 in texts else None,
        max_version=texts[3][-1] if 3 in texts else None,
        channels=tuple(_signed(value) for value in numbers.get(4, ())),
        platforms=tuple(numbers.get(5, ())),
        locales=tuple(texts.get(6, ())),
        exclude_locales=tuple(texts.get(12, ())),
        form_factors=tuple(numbers.get(7, ())),
        exclude_form_factors=tuple(numbers.get(14, ())),
        countries=tuple(texts.get(10, ())),
        exclude_countries=tuple(texts.get(11, ())),
        cpu_architectures=tuple(numbers.get(20, ())),
        exclude_cpu_architectures=tuple(numbers.get(21, ())),
        is_low_end_device=flag(15),
        is_enterprise=flag(18),
        policy_restriction=single(19) or 0,
        google_groups=tuple(numbers.get(22, ())),
        enterprise_groups=tuple(texts.get(26, ())),
        undecided=tuple(dict.fromkeys(undecided)),
    )


def _layer_reference(data: bytes) -> tuple[int, list[int]]:
    layer_id = 0
    members: list[int] = []
    for number, value in _fields(data):
        if number == 1:
            layer_id = _number(value)
        elif number in (2, 3):
            members.extend(_packed(value) if isinstance(value, bytes) else [value])
    return layer_id, members


def _layer(data: bytes) -> Layer:
    layer_id = slots = 0
    members: dict[int, int] = {}
    for number, value in _fields(data):
        if number == 1:
            layer_id = _number(value)
        elif number == 2:
            slots = _number(value)
        elif number == 3:
            member_id, held = 0, 0
            for key, item in _fields(_bytes(value)):
                if key == 1:
                    member_id = _number(item)
                elif key == 2:
                    ends = {part: _number(end) for part, end in _fields(_bytes(item))}
                    held += ends.get(2, 0) - ends.get(1, 0) + 1
            members[member_id] = members.get(member_id, 0) + held
    return Layer(id=layer_id, slots=slots, members=members)


def _fields(data: bytes) -> Iterator[tuple[int, int | bytes]]:
    """Yield each field of a protocol buffer: its number, and a number or the bytes of a length-delimited value."""
    position = 0
    while position < len(data):
        key, position = _varint(data, position)
        number, wire_type = key >> 3, key & 7
        if wire_type == 0:
            value, position = _varint(data, position)
            yield number, value
        elif wire_type == 2:
            size, position = _varint(data, position)
            if position + size > len(data):
                raise SeedError("a field runs past the end")
            yield number, data[position : position + size]
            position += size
        elif wire_type == 1:
            yield number, int.from_bytes(data[position : position + 8], "little")
            position += 8
        elif wire_type == 5:
            yield number, int.from_bytes(data[position : position + 4], "little")
            position += 4
        else:
            raise SeedError(f"field {number} has the wire type {wire_type}, which this does not read")


def _varint(data: bytes, position: int) -> tuple[int, int]:
    result = shift = 0
    while True:
        if position >= len(data) or shift > 63:
            raise SeedError("a number runs past the end")
        byte = data[position]
        position += 1
        result |= (byte & 0x7F) << shift
        shift += 7
        if byte < 0x80:
            return result, position


def _packed(data: bytes) -> list[int]:
    values: list[int] = []
    position = 0
    while position < len(data):
        value, position = _varint(data, position)
        values.append(value)
    return values


def _signed(value: int) -> int:
    """Return a 64-bit varint as the signed number that an enum such as Channel.UNKNOWN, -1, writes."""
    return value - (1 << 64) if value >= 1 << 63 else value


def _number(value: int | bytes) -> int:
    if isinstance(value, bytes):
        raise SeedError("a field holds bytes where a number belongs")
    return value


def _bytes(value: int | bytes) -> bytes:
    if not isinstance(value, bytes):
        raise SeedError("a field holds a number where a message belongs")
    return value


def _text(value: int | bytes) -> str:
    try:
        return _bytes(value).decode()
    except UnicodeDecodeError as error:
        raise SeedError(f"a field is not UTF-8: {error}") from error
