"""What a Page of each Persona starts with beside the Engine Release's own: Blink's runtime-enabled features to turn on
and off, and V8's flags. docs/persona.md, "What a Page of each Persona turns on and off", says what they are for.

One V8 and one set of bindings ship, the Engine Release's (ADR-0008). A Page has the Engine Release's runtime-enabled
features for stable on Windows unless its start message says otherwise, so a Persona says which to turn on and off:

- Its Chrome Release's own set, worked out from that release's runtime_enabled_features.json5 by the generator's rules
  (native/bindings/bindings_generator/runtime_features.py). A feature that its file does not have is off.
- What the variations seed of its milestone switches for most installations (tools/release/variations_seed.py), through
  the base::Feature whose state Chrome copies into a runtime-enabled feature: the one that the feature's entry names, and
  those of the table in content/child/runtime_features.cc.
- V8's features that the Engine Release's V8 ships and its release's V8 did not, from v8/src/flags/feature-flags.h at the
  V8 that its DEPS pins, and the V8 flags of the seed's V8Flag_ features, as gin sets them.

Each comes from the release's tag alone, through Chromium's Gitiles servers, and the seed from the variations server, as
Chrome fetches it. What cannot be expressed this way, such as a feature that only an older release has, goes to review.
"""

# Upstream: content/child/runtime_features.cc
# Upstream: v8/src/flags/feature-flags.h
# Upstream: gin/v8_initializer.cc

import base64
import binascii
import datetime
import json
import re
import sys
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, cast

import json5

from tools.parity import BASELINES_DIRECTORY
from tools.release.gitiles import CHROMIUM_URL
from tools.release.persona import PAGE_FEATURES_KEY, PERSONAS_DIRECTORY, dumps_persona
from tools.release.variations_seed import (
    Client,
    FeatureSwitch,
    UndecidedStudy,
    fetch_seed,
    majority_switches,
    serial_version,
    v8_flags,
)
from tools.release.web import Fetch, WebError

# The generator is a package of its own, which GN runs from native/bindings, and whose rules this shares.
_GENERATOR = Path(__file__).resolve().parents[2] / "native" / "bindings"
if str(_GENERATOR) not in sys.path:
    sys.path.append(str(_GENERATOR))

from bindings_generator.errors import UnhandledError  # noqa: E402
from bindings_generator.runtime_features import Feature, read_features  # noqa: E402

__all__ = [
    "BASELINE_COUNTRY",
    "V8_URL",
    "PageFeatures",
    "PageFeaturesError",
    "PersonaFeatures",
    "ReleaseSource",
    "fetch_release",
    "persona_features",
    "read_release",
    "release_files",
    "write_page_features",
]

V8_URL = "https://chromium.googlesource.com/v8/v8"

BASELINE_COUNTRY = "us"
"""The country that every start of Chrome for the Baselines fixes, with --variations-override-country (baselines.md)."""

# The line of a Baselines' chrome.txt that names the seed they were recorded with.
_SEED_LINE = "Variations seed: "
# The header line of a Persona's navigation that names its build.
_FULL_VERSION = "sec-ch-ua-full-version: "

RUNTIME_ENABLED_FEATURES = "third_party/blink/renderer/platform/runtime_enabled_features.json5"
CONTENT_RUNTIME_FEATURES = "content/child/runtime_features.cc"
V8_FEATURE_FLAGS = "src/flags/feature-flags.h"
V8_FLAG_DEFINITIONS = "src/flags/flag-definitions.h"

# Files of Chromium that set V8 flags or runtime-enabled features in ways this does not work out, such as from the
# default of a base::Feature. When a release's differs from the Engine Release's, a person reads the difference.
COMPARED_FILES = (
    CONTENT_RUNTIME_FEATURES,
    "gin/gin_features.cc",
    "gin/v8_initializer.cc",
    "content/renderer/render_process_impl.cc",
)

# Keys of runtime_enabled_features.json5 that an older release has and the generator no longer reads, with why they
# leave a feature's state as it is.
_OLDER_KEYS = {
    "is_protected_feature": "whether --enable-blink-features may name it, which a Page's set does not go through",
}

_V8_FLAG_PREFIX = "V8Flag_"
_V8_PIN = re.compile(r"'v8_revision': '([0-9a-f]{40})'")

# The stages of V8's features in feature-flags.h, and the one whose features are on by default.
_V8_STAGES = {"EXPERIMENTAL": "experimental", "PRE_STAGED": "pre-staged", "STAGED": "staged", "SHIPPED": "shipped"}
_SHIPPED = "shipped"
# A staged feature is a plain flag, which a Page can turn on. Experimental and pre-staged ones need V8's --experimental.
_CAN_TURN_ON = frozenset({"staged", _SHIPPED})
_V8_SECTION = re.compile(r"#define FOREACH_(\w+?)_FEATURE_FLAG\((.*?)(?=\n#define |\n#endif)", re.DOTALL)
_V8_FEATURE = re.compile(r"\b(JS_FEATURE|WASM_FEATURE|INTERNAL_FEATURE)\s*(?:\(\s*|,\s*)(\w+)\s*,")
_V8_FLAG_DEFINITION = re.compile(r"\bDEFINE_[A-Z_]+\(\s*(\w+)\s*,")

# An entry of the tables in SetRuntimeFeaturesFromChromiumFeatures(): a runtime-enabled feature, by its enabler or its
# name, then the base::Feature whose state it takes, as a constant kName, then an option. Either option copies the
# state of a base::Feature that a field trial overrides.
_CONTENT_ENTRY = re.compile(
    r"\{\s*(?:wf::Enable(?P<enabler>\w+)|\"(?P<name>\w+)\")\s*,\s*(?:raw_ref\()?\s*(?:\w+::)*k(?P<base>\w+)\)?"
    r"\s*(?:,\s*(?P<option>\w+))?\s*\}"
)
_CONTENT_OPTIONS = frozenset({None, "kDefault", "kSetOnlyIfOverridden"})


class PageFeaturesError(Exception):
    """A release's files are not what this reads."""


@dataclass(frozen=True, slots=True)
class ReleaseSource:
    """What one build of Chrome says about the features of its renderers, from its tag.

    Attributes:
        build: The build, such as "157.0.8103.39".
        features: Its runtime-enabled features, in the order of its file, with their state for stable on Windows.
        copies: Each copy of the state of a base::Feature that a field trial overrides into a runtime-enabled
            feature, as (base::Feature, runtime-enabled feature), in the order Chrome makes them: content's tables
            first, then each feature's entry, so that a later copy wins.
        v8_features: The stage of each of its V8's features, by the feature's flag, such as "js_iterator_join".
        v8_flags: The name of every flag of its V8.
        compared: The text of each of COMPARED_FILES, by path.
    """

    build: str
    features: tuple[Feature, ...]
    copies: tuple[tuple[str, str], ...]
    v8_features: Mapping[str, str]
    v8_flags: frozenset[str]
    compared: Mapping[str, str]

    @property
    def copied(self) -> frozenset[str]:
        """The base::Features whose state a runtime-enabled feature takes when a field trial overrides it."""
        return frozenset(base for base, _ in self.copies)


@dataclass(frozen=True, slots=True)
class PageFeatures:
    """What a Page of a Persona starts with beside the Engine Release's runtime-enabled features and V8's defaults.

    Attributes:
        enable_blink_features: The runtime-enabled features to turn on, in the order of the Engine Release's file.
        disable_blink_features: Those to turn off, in the same order.
        v8_flags: V8's flags, in the order to set them.
    """

    enable_blink_features: tuple[str, ...] = ()
    disable_blink_features: tuple[str, ...] = ()
    v8_flags: tuple[str, ...] = ()


@dataclass(frozen=True, slots=True)
class PersonaFeatures:
    """What a Persona's Page starts with, and what a person reviews of it.

    Attributes:
        build: The build of the Persona's Chrome Release.
        seed: The version of the variations seed that was evaluated.
        page: What its Page starts with.
        switched: What the seed switches, one line each.
        review: What cannot be expressed or worked out here, one line each, for a person to decide.
    """

    build: str
    seed: str
    page: PageFeatures
    switched: tuple[str, ...] = ()
    review: tuple[str, ...] = field(default_factory=tuple[str, ...])


def release_files(build: str, fetch: Fetch) -> dict[str, str]:
    """Return the files of a build that read_release() reads, by path, fetched from its tag and its V8's pin.

    Raises:
        WebError: A server did not answer, or its answer is not a file.
    """
    deps_url = f"{CHROMIUM_URL}/+/{build}/DEPS?format=TEXT"
    pin = _V8_PIN.search(_gitiles_file(fetch, deps_url))
    if pin is None:
        raise WebError(f"{deps_url} pins no V8")
    files = {
        path: _gitiles_file(fetch, f"{CHROMIUM_URL}/+/{build}/{path}?format=TEXT")
        for path in (RUNTIME_ENABLED_FEATURES, *COMPARED_FILES)
    }
    for path in (V8_FEATURE_FLAGS, V8_FLAG_DEFINITIONS):
        files[f"v8/{path}"] = _gitiles_file(fetch, f"{V8_URL}/+/{pin[1]}/{path}?format=TEXT")
    return files


def fetch_release(build: str, fetch: Fetch) -> ReleaseSource:
    """Return what a build says about the features of its renderers, from its tag.

    Raises:
        WebError: A server did not answer, or its answer is not a file.
        PageFeaturesError: A file is not what read_release() reads.
    """
    return read_release(build, release_files(build, fetch))


def read_release(build: str, files: Mapping[str, str]) -> ReleaseSource:
    """Return what a build says about the features of its renderers, from its files, by their paths in release_files().

    Raises:
        PageFeaturesError: A file is not what this reads, such as a feature with a key that neither the generator nor
            this knows.
    """
    entries = _feature_entries(files[RUNTIME_ENABLED_FEATURES], build)
    try:
        features = tuple(read_features(entries))
    except UnhandledError as error:
        raise PageFeaturesError(f"{build}: {error}") from error
    v8_features = _v8_features(files[f"v8/{V8_FEATURE_FLAGS}"], build)
    v8_defined = set(_V8_FLAG_DEFINITION.findall(files[f"v8/{V8_FLAG_DEFINITIONS}"]))
    return ReleaseSource(
        build=build,
        features=features,
        copies=_copies(entries, files[CONTENT_RUNTIME_FEATURES], {feature.name for feature in features}),
        v8_features=v8_features,
        v8_flags=frozenset(v8_defined | v8_features.keys()),
        compared={path: files[path] for path in COMPARED_FILES},
    )


def persona_features(
    release: ReleaseSource,
    engine: ReleaseSource,
    seed: str,
    switches: Sequence[FeatureSwitch],
    undecided: Iterable[UndecidedStudy] = (),
    baseline_seed: str | None = None,
) -> PersonaFeatures:
    """Return what a Page of a Persona starts with beside the Engine Release's features and V8's defaults.

    Args:
        release: The Persona's Chrome Release, at the build of its Baselines.
        engine: The Engine Release, at the build that the engine is built from, whose set a Page has by default.
        seed: The version of the variations seed that `switches` come from.
        switches: What the seed switches for most installations of the Persona's Chrome.
        undecided: The studies of the seed that depend on the machine. Those that switch a runtime-enabled feature or a
            V8 flag go to review.
        baseline_seed: The version of the seed that the release's Baselines were recorded with, or None if they name
            none. Another version than `seed` goes to review, since the Persona's data and its Baselines should come
            from one seed.
    """
    review: list[str] = []
    if baseline_seed is None or not seed.startswith(baseline_seed):
        review.append(
            f"The seed is {seed}, and Chrome {release.build}'s Baselines were recorded with"
            f" {baseline_seed or 'no seed that their chrome.txt names'}: work this out on the day the Baselines are"
            " recorded"
        )
    review += [
        f"The seed's study {study.name} switches {', '.join(study.features)} by {', '.join(study.fields)}, which"
        " follows the machine, so the Persona leaves it aside"
        for study in undecided
        if any(feature in release.copied or feature.startswith(_V8_FLAG_PREFIX) for feature in study.features)
    ]
    states, switched = _release_states(release, switches)
    enable, disable = _blink_switches(states, engine)
    review += _blink_review(release, engine, states)
    flags, flag_review = _v8_flags(release, engine, switches)
    review += flag_review
    review += [
        f"{path} differs from the Engine Release's: read the difference for a feature or V8 flag that it changes"
        for path, text in release.compared.items()
        if engine.compared.get(path) != text
    ]
    return PersonaFeatures(
        build=release.build,
        seed=seed,
        page=PageFeatures(enable_blink_features=enable, disable_blink_features=disable, v8_flags=flags),
        switched=tuple(switched),
        review=tuple(review),
    )


def write_page_features(
    engine_build: str,
    today: datetime.date,
    fetch: Fetch,
    personas_directory: Path = PERSONAS_DIRECTORY,
    baselines_directory: Path = BASELINES_DIRECTORY,
) -> str:
    """Work out what a Page of each Persona starts with, write it into each Persona's file, and return a report for review.

    Args:
        engine_build: The build that the engine is built from, as tools/workspace/chromium_tag names it.
        today: The day that the seed of each Chrome Release is evaluated on.
        fetch: What asks Chromium's Gitiles servers and the variations server.
        personas_directory: The directory of the Personas' files.
        baselines_directory: The directory of the Baselines, whose chrome.txt of each Chrome Release names the seed
            that they were recorded with.

    Raises:
        WebError: A server did not answer, or its answer is not what this reads.
        PageFeaturesError: A release's files are not what this reads.
    """
    engine = fetch_release(engine_build, fetch)
    sections: list[str] = []
    for path in sorted(personas_directory.glob("chrome_*_*.json")):
        data = json.loads(path.read_text(encoding="utf-8"))
        build = _persona_build(data, path)
        release = engine if build == engine_build else fetch_release(build, fetch)
        seed = fetch_seed(int(build.split(".", 1)[0]), fetch)
        locale = str(data["accept_language"]).split(",", 1)[0].split(";", 1)[0].strip()
        switches, undecided = majority_switches(seed, Client(build, locale, BASELINE_COUNTRY, today))
        baseline_seed = _baseline_seed(baselines_directory / str(data["release"]) / "chrome.txt")
        features = persona_features(release, engine, seed.version, switches, undecided, baseline_seed)
        before = data.get(PAGE_FEATURES_KEY, {})
        data[PAGE_FEATURES_KEY] = {
            "seed": features.seed,
            "enable_blink_features": list(features.page.enable_blink_features),
            "disable_blink_features": list(features.page.disable_blink_features),
            "v8_flags": list(features.page.v8_flags),
        }
        path.write_text(dumps_persona(data), encoding="utf-8", newline="\n")
        sections.append(_report(path.name, features, before))
    return "\n".join(sections)


def _baseline_seed(chrome_txt: Path) -> str | None:
    """Return the version of the seed that a chrome.txt of the Baselines names on its "Variations seed:" line, or None."""
    if not chrome_txt.is_file():
        return None
    for line in chrome_txt.read_text(encoding="utf-8").splitlines():
        if line.startswith(_SEED_LINE):
            return serial_version(line.removeprefix(_SEED_LINE).strip())
    return None


def _persona_build(data: Mapping[str, Any], path: Path) -> str:
    """Return the build of a Persona's Chrome Release, which its navigation's sec-ch-ua-full-version names."""
    for line in data["kinds"]["navigation"]["headers"]:
        if str(line).startswith(_FULL_VERSION):
            return str(line).removeprefix(_FULL_VERSION).strip('"')
    raise PageFeaturesError(f"{path} names no build in a {_FULL_VERSION.strip(': ')} line")


def _report(name: str, features: PersonaFeatures, before: Mapping[str, Any]) -> str:
    """Return what a Persona's Page starts with, what changed in its file, and what a person reviews, as lines."""
    page = features.page
    lines = [
        f"{name}: Chrome {features.build}, seed {features.seed}",
        f"  {len(page.enable_blink_features)} Blink features on, {len(page.disable_blink_features)} off,"
        f" {len(page.v8_flags)} V8 flags",
    ]
    for key, now in (
        ("enable_blink_features", page.enable_blink_features),
        ("disable_blink_features", page.disable_blink_features),
        ("v8_flags", page.v8_flags),
    ):
        was = [str(item) for item in before.get(key, [])]
        added = [item for item in now if item not in was]
        gone = [item for item in was if item not in now]
        if added:
            lines.append(f"  {key} gains: {', '.join(added)}")
        if gone:
            lines.append(f"  {key} loses: {', '.join(gone)}")
    lines += ["  The seed switches:", *(f"    {line}" for line in features.switched)] if features.switched else []
    lines += ["  To review:", *(f"    {line}" for line in features.review)] if features.review else []
    return "\n".join(lines)


def _release_states(release: ReleaseSource, switches: Sequence[FeatureSwitch]) -> tuple[dict[str, bool], list[str]]:
    """Return the state of each runtime-enabled feature of a release with what its seed switches, and lines that say what
    the seed switched."""
    states = {feature.name: feature.stable for feature in release.features}
    # A base::Feature takes part in one field trial only: the first that switches it.
    overrides: dict[str, FeatureSwitch] = {}
    for switch in switches:
        overrides.setdefault(switch.feature, switch)
    switched: list[str] = []
    for base, name in release.copies:
        switch = overrides.get(base)
        if switch is None:
            continue
        states[name] = switch.enabled
        state = "on" if switch.enabled else "off"
        switched.append(
            f"{name} {state}: {switch.feature} in {switch.study}/{switch.group}, {switch.share:.0%} of installations"
        )
    return states, switched


def _blink_switches(states: Mapping[str, bool], engine: ReleaseSource) -> tuple[tuple[str, ...], tuple[str, ...]]:
    """Return the Engine Release's features to turn on and off to give a Page these states, in the order of its file.

    A feature that the release does not have is off."""
    enable = tuple(f.name for f in engine.features if states.get(f.name, False) and not f.stable)
    disable = tuple(f.name for f in engine.features if not states.get(f.name, False) and f.stable)
    return enable, disable


def _blink_review(release: ReleaseSource, engine: ReleaseSource, states: Mapping[str, bool]) -> list[str]:
    """Return what the Engine Release's features cannot match of a release's: a feature whose dependencies give it another
    state there, and the features that only the release has and has on."""
    wanted = _effective(release.features, states)
    engine_states = {f.name: states.get(f.name, False) for f in engine.features}
    got = _effective(engine.features, engine_states)
    review = [
        f"{name} is {'on' if on else 'off'} in Chrome {release.build}, and the Engine Release's dependencies of its"
        f" features turn it {'off' if on else 'on'}"
        for name, on in wanted.items()
        if name in got and got[name] != on
    ]
    only_here = [name for name, on in wanted.items() if on and name not in got]
    if only_here:
        review.append(
            f"On in Chrome {release.build} and not in the Engine Release's file, so shipped for good or gone, which is a"
            f" Known Gap if a page can see it: {', '.join(only_here)}"
        )
    return review


def _effective(features: Iterable[Feature], states: Mapping[str, bool]) -> dict[str, bool]:
    """Return whether each feature is on, as RuntimeFeatureSet::IsEnabled() and Blink's FooEnabled() decide it: off if a
    feature it depends on is off, on if a feature that implies it is on, and otherwise its own state."""
    by_name = {feature.name: feature for feature in features}
    found: dict[str, bool] = {}

    def enabled(name: str) -> bool:
        if name not in found:
            found[name] = False  # Against a cycle, which Blink's build refuses.
            feature = by_name[name]
            if not all(enabled(other) for other in feature.depends_on):
                found[name] = False
            elif any(enabled(other) for other in feature.implied_by):
                found[name] = True
            else:
                found[name] = states.get(name, False)
        return found[name]

    return {name: enabled(name) for name in by_name}


def _v8_flags(
    release: ReleaseSource, engine: ReleaseSource, switches: Sequence[FeatureSwitch]
) -> tuple[tuple[str, ...], list[str]]:
    """Return the V8 flags of a Persona's Page, and what goes to review: the Engine Release's V8 features that its release
    did not ship off and those it shipped and the Engine Release does not on, then the seed's flags, which win."""
    flags: dict[str, str] = {}
    review: list[str] = []
    for flag, stage in engine.v8_features.items():
        if stage == _SHIPPED and release.v8_features.get(flag) != _SHIPPED:
            flags[flag] = f"--no-{flag}"
    for flag, stage in release.v8_features.items():
        if stage != _SHIPPED or engine.v8_features.get(flag) == _SHIPPED:
            continue
        if engine.v8_features.get(flag) in _CAN_TURN_ON:
            flags[flag] = f"--{flag}"
        else:
            review.append(
                f"V8 of Chrome {release.build} ships {flag}, which the Engine Release's V8 has"
                f" {engine.v8_features.get(flag, 'not')} and cannot turn on: a Known Gap if a page can see it"
            )
    agreed: list[FeatureSwitch] = []
    for switch in switches:
        if switch.feature.startswith(_V8_FLAG_PREFIX) and switch.enabled and not switch.params_agreed:
            review.append(
                f"The seed turns {switch.feature} on for {switch.share:.0%} of installations in {switch.study}, but no"
                " parameters for more than half of them, so the Page sets no flag for it"
            )
        else:
            agreed.append(switch)
    for flag in v8_flags(agreed):
        # gin writes --NAME, --NAME=VALUE or --no-NAME, and V8 reads - and _ in a name alike.
        key = flag.removeprefix("--").split("=", 1)[0].removeprefix("no-").replace("-", "_")
        if key not in engine.v8_flags:
            review.append(
                f"The seed sets {flag}, which the Engine Release's V8 does not have, so the Page leaves it out"
            )
            continue
        flags.pop(key, None)
        flags[key] = flag
    return tuple(flags.values()), review


def _feature_entries(text: str, build: str) -> list[Mapping[str, Any]]:
    """Return the features of a runtime_enabled_features.json5, without the keys that only older releases have."""
    try:
        data = json5.loads(text)
    except ValueError as error:
        raise PageFeaturesError(f"{build}: {RUNTIME_ENABLED_FEATURES} is not JSON5: {error}") from error
    entries = cast(dict[str, Any], data).get("data") if isinstance(data, dict) else None
    if not isinstance(entries, list):
        raise PageFeaturesError(f"{build}: {RUNTIME_ENABLED_FEATURES} has no list of features")
    return [
        {key: value for key, value in cast(dict[str, Any], entry).items() if key not in _OLDER_KEYS}
        for entry in cast(list[Any], entries)
    ]


def _copies(entries: Iterable[Mapping[str, Any]], content: str, names: set[str]) -> tuple[tuple[str, str], ...]:
    """Return each copy of the state of a base::Feature that a field trial overrides into a runtime-enabled feature, as
    (base::Feature, runtime-enabled feature), in the order Chrome makes them.

    Chrome copies them in SetRuntimeFeaturesFromChromiumFeatures() first, from its tables, and then in
    UpdateStatusFromBaseFeatures(), from the base_feature of each feature's entry, which is the feature's own name unless
    the entry names another or "none". The table names a base::Feature by its constant, kName, whose name it takes to be
    the name of the feature.
    """
    copies: list[tuple[str, str]] = []
    for match in _CONTENT_ENTRY.finditer(content):
        name = match["enabler"] or match["name"]
        if match["option"] not in _CONTENT_OPTIONS:
            raise PageFeaturesError(f"{CONTENT_RUNTIME_FEATURES} copies {name} with {match['option']}, unknown here")
        if name in names:
            copies.append((match["base"], name))
    for entry in entries:
        base = str(entry.get("base_feature", entry["name"]))
        if base != "none":
            copies.append((base, str(entry["name"])))
    return tuple(copies)


def _v8_features(text: str, build: str) -> dict[str, str]:
    """Return the stage of each V8 feature in feature-flags.h, by its flag: its name, after wasm_ for a Wasm feature."""
    stages: dict[str, str] = {}
    for section, body in _V8_SECTION.findall(text):
        stage = _V8_STAGES.get(section)
        if stage is None:
            raise PageFeaturesError(f"{build}: {V8_FEATURE_FLAGS} has the stage {section}, unknown here")
        for kind, name in _V8_FEATURE.findall(body):
            stages[f"wasm_{name}" if kind == "WASM_FEATURE" else name] = stage
    if _SHIPPED not in stages.values():
        raise PageFeaturesError(f"{build}: {V8_FEATURE_FLAGS} lists no shipped feature")
    return stages


def _gitiles_file(fetch: Fetch, url: str) -> str:
    """Return the text of a file that a Gitiles server gives in base64, as it does with format=TEXT."""
    try:
        return base64.b64decode(fetch(url), validate=False).decode("utf-8-sig")
    except (binascii.Error, UnicodeDecodeError) as error:
        raise WebError(f"{url} did not give a file: {error}") from error
