"""Tests of working out what a Page of each Persona turns on and off, on release files and seeds written here. They use no
network and no native library."""

import base64
import datetime
import json
import shutil
from collections.abc import Mapping
from pathlib import Path

import pytest

from mitto._persona import WINDOWS_PERSONAS
from tests.release.test_variations_seed import group, message, number, seed, study, windows_stable
from tools.release.gitiles import CHROMIUM_URL
from tools.release.page_features import (
    COMPARED_FILES,
    CONTENT_RUNTIME_FEATURES,
    RUNTIME_ENABLED_FEATURES,
    V8_FEATURE_FLAGS,
    V8_FLAG_DEFINITIONS,
    V8_URL,
    PageFeatures,
    PageFeaturesError,
    ReleaseSource,
    persona_features,
    read_release,
    write_page_features,
)
from tools.release.persona import PERSONAS_DIRECTORY
from tools.release.variations_seed import FeatureSwitch, UndecidedStudy, seed_url
from tools.release.web import WebError

# The oldest Persona and the Engine Release's, with builds of their milestones that this file's answers stand for.
OLDER_RELEASE = min(WINDOWS_PERSONAS, key=int)
ENGINE_RELEASE = max(WINDOWS_PERSONAS, key=int)
OLDER = f"{OLDER_RELEASE}.0.8000.10"
ENGINE = f"{ENGINE_RELEASE}.0.8100.20"
OLDER_FILE = f"chrome_{OLDER_RELEASE}_windows.json"
ENGINE_FILE = f"chrome_{ENGINE_RELEASE}_windows.json"
SEED = "20261111-170049.393000-production"

ENGINE_FEATURES = """{
  // Comments, unquoted keys and trailing commas, as Blink's file has them.
  parameters: {},
  data: [
    {name: "Shared", status: "stable"},
    {name: "NewInEngine", status: "stable"},
    {name: "StableLater", status: "stable"},
    {name: "OnlyInOlderStable", status: "experimental"},
    {name: "Copied", status: "stable", base_feature: "none"},
    {name: "Named", status: "stable", base_feature: "OtherName"},
    {name: "Plain", status: "stable"},
    {name: "Dependent", status: "stable", depends_on: ["Shared"]},
    {name: "Implied", status: "experimental", implied_by: ["Shared"]},
  ],
}"""

OLDER_FEATURES = """{
  data: [
    {name: "Shared", status: "stable"},
    {name: "StableLater", status: "experimental"},
    {name: "OnlyInOlderStable", status: {"Win": "stable", "default": "experimental"}},
    {name: "Copied", status: "stable", base_feature: "none"},
    {name: "Named", status: "stable", base_feature: "OtherName"},
    {name: "Plain", status: "stable"},
    {name: "Dependent", status: "stable", depends_on: ["Shared"]},
    {name: "Implied", status: "experimental", implied_by: ["Shared"]},
    {name: "Shipped", status: "stable", is_protected_feature: true},
  ],
}"""

CONTENT = """
  const RuntimeFeatureToChromiumFeatureMap<void (*)(bool)> mapping[] = {
      {wf::EnableCopied, raw_ref(features::kConversionMeasurement)},
      {wf::EnableNotAFeature, features::kSomething, kSetOnlyIfOverridden},
  };
  const RuntimeFeatureByNameToChromiumFeatureMap<const char*> by_name[] = {
      {"Plain", network::features::kBrowsingTopics, kSetOnlyIfOverridden},
  };
"""

ENGINE_V8_FEATURES = """
#define FOREACH_EXPERIMENTAL_FEATURE_FLAG(JS_FEATURE, WASM_FEATURE, INTERNAL_FEATURE) \\
  JS_FEATURE(js_experiment, "experiment")
#define FOREACH_STAGED_FEATURE_FLAG(JS_FEATURE, WASM_FEATURE, INTERNAL_FEATURE) \\
  JS_FEATURE(js_staged_back, "staged back")
#define FOREACH_SHIPPED_FEATURE_FLAG(JS_FEATURE, WASM_FEATURE, INTERNAL_FEATURE) \\
  JS_FEATURE(js_old, "old")                                                      \\
  JS_FEATURE(js_new, "new")                                                      \\
  WASM_FEATURE(new_wasm, "new wasm")                                             \\
  IF_INTL_ENABLED(JS_FEATURE, js_intl_new, "intl")                               \\
  INTERNAL_FEATURE(homomorphic_ic, "ICs")
#endif
"""

OLDER_V8_FEATURES = """
#define FOREACH_EXPERIMENTAL_FEATURE_FLAG(JS_FEATURE, WASM_FEATURE, INTERNAL_FEATURE) \\
  JS_FEATURE(js_new, "new") \\
  WASM_FEATURE(new_wasm, "new wasm")
#define FOREACH_STAGED_FEATURE_FLAG(JS_FEATURE, WASM_FEATURE, INTERNAL_FEATURE) \\
  INTERNAL_FEATURE(homomorphic_ic, "ICs")
#define FOREACH_SHIPPED_FEATURE_FLAG(JS_FEATURE, WASM_FEATURE, INTERNAL_FEATURE) \\
  JS_FEATURE(js_old, "old")                                                      \\
  JS_FEATURE(js_staged_back, "staged back")                                      \\
  JS_FEATURE(js_experiment, "experiment")                                        \\
  JS_FEATURE(js_removed, "removed")                                              \\
  IF_INTL_ENABLED(JS_FEATURE, js_intl_new, "intl")
#endif
"""

FLAG_DEFINITIONS = """
DEFINE_BOOL(sqrt_allocation_limits, false, "sqrt")
DEFINE_INT(sqrt_allocation_limits_factor, 1, "factor")
DEFINE_IMPLICATION(sqrt_allocation_limits, sqrt_allocation_limits_factor)
"""


def files(features: str, v8_features: str, **changed: str) -> dict[str, str]:
    """Return a release's files, by their paths in release_files()."""
    return {
        RUNTIME_ENABLED_FEATURES: features,
        CONTENT_RUNTIME_FEATURES: CONTENT,
        f"v8/{V8_FEATURE_FLAGS}": v8_features,
        f"v8/{V8_FLAG_DEFINITIONS}": FLAG_DEFINITIONS,
        **{
            path: changed.get(Path(path).name, f"// {path}")
            for path in COMPARED_FILES
            if path != CONTENT_RUNTIME_FEATURES
        },
    }


ENGINE_SOURCE = read_release(ENGINE, files(ENGINE_FEATURES, ENGINE_V8_FEATURES))
OLDER_SOURCE = read_release(OLDER, files(OLDER_FEATURES, OLDER_V8_FEATURES))


def switch(feature: str, enabled: bool, **params: str) -> FeatureSwitch:
    return FeatureSwitch(feature, enabled, "Study", "Group", 1.0, params)


def features_of(release: ReleaseSource = OLDER_SOURCE, *switches: FeatureSwitch) -> PageFeatures:
    return persona_features(release, ENGINE_SOURCE, SEED, switches, baseline_seed=SEED).page


def test_the_engine_releases_own_persona_without_a_seed_changes_nothing() -> None:
    assert features_of(ENGINE_SOURCE) == PageFeatures()


def test_an_older_release_lacks_what_it_does_not_have_and_what_it_had_not_on_yet() -> None:
    page = features_of()

    assert page.disable_blink_features == ("NewInEngine", "StableLater")


def test_an_older_release_has_what_it_had_on_and_the_engine_release_has_not() -> None:
    assert features_of().enable_blink_features == ("OnlyInOlderStable",)


def test_a_feature_that_only_the_older_release_has_on_goes_to_review() -> None:
    review = persona_features(OLDER_SOURCE, ENGINE_SOURCE, SEED, (), baseline_seed=SEED).review

    assert any("not in the Engine Release's file" in line and "Shipped" in line for line in review)


@pytest.mark.parametrize(
    ("base_feature", "feature"),
    [
        pytest.param("Shared", "Shared", id="its-own-name"),
        pytest.param("OtherName", "Named", id="named-in-its-entry"),
        pytest.param("ConversionMeasurement", "Copied", id="content-enabler"),
        pytest.param("BrowsingTopics", "Plain", id="content-by-name"),
    ],
)
def test_what_the_seed_turns_off_is_off(base_feature: str, feature: str) -> None:
    assert feature in features_of(OLDER_SOURCE, switch(base_feature, False)).disable_blink_features


def test_what_the_seed_turns_off_of_an_older_releases_own_feature_stays_off() -> None:
    page = features_of(OLDER_SOURCE, switch("OnlyInOlderStable", False))

    assert "OnlyInOlderStable" not in page.enable_blink_features + page.disable_blink_features


def test_what_the_seed_turns_on_is_on_and_said() -> None:
    result = persona_features(ENGINE_SOURCE, ENGINE_SOURCE, SEED, [switch("Implied", True)], baseline_seed=SEED)

    assert result.page.enable_blink_features == ("Implied",)
    assert result.switched == ("Implied on: Implied in Study/Group, 100% of installations",)


def test_a_base_feature_that_names_no_runtime_feature_changes_nothing() -> None:
    assert features_of(ENGINE_SOURCE, switch("DeprecateUnload", True), switch("Something", True)) == PageFeatures()


def test_a_state_that_the_engine_releases_dependencies_cannot_give_goes_to_review() -> None:
    # The seed turns Shared off: then Dependent is off in both, but Implied, which Shared no longer implies, is off.
    off = persona_features(ENGINE_SOURCE, ENGINE_SOURCE, SEED, [switch("Shared", False)], baseline_seed=SEED)
    assert "Shared" in off.page.disable_blink_features
    assert not any("dependencies" in line for line in off.review)

    graph = read_release(OLDER, files(OLDER_FEATURES.replace('depends_on: ["Shared"]', ""), OLDER_V8_FEATURES))
    differs = persona_features(graph, ENGINE_SOURCE, SEED, [switch("Shared", False)], baseline_seed=SEED)
    assert any(line.startswith("Dependent is on in Chrome") for line in differs.review)


def test_v8_features_that_the_older_release_did_not_ship_are_turned_off() -> None:
    flags = features_of().v8_flags

    assert flags[:3] == ("--no-js_new", "--no-wasm_new_wasm", "--no-homomorphic_ic")


def test_a_v8_feature_that_only_the_older_release_ships_is_turned_on_if_the_engine_release_has_it_staged() -> None:
    result = persona_features(OLDER_SOURCE, ENGINE_SOURCE, SEED, (), baseline_seed=SEED)

    assert "--js_staged_back" in result.page.v8_flags
    assert not any("experiment" in flag or "removed" in flag for flag in result.page.v8_flags)
    review = [line for line in result.review if line.startswith("V8 of Chrome")]
    assert len(review) == 2
    assert "js_experiment, which the Engine Release's V8 has experimental" in review[0]
    assert "js_removed, which the Engine Release's V8 has not" in review[1]


def test_the_seeds_v8_flags_come_last_and_win_over_the_releases() -> None:
    page = features_of(
        OLDER_SOURCE,
        switch("V8Flag_homomorphic_ic", True),
        switch("V8Flag_sqrt_allocation_limits", True, sqrt_allocation_limits_factor="80"),
    )

    assert page.v8_flags == (
        "--no-js_new",
        "--no-wasm_new_wasm",
        "--js_staged_back",
        "--homomorphic_ic",
        "--sqrt_allocation_limits",
        "--sqrt_allocation_limits_factor=80",
    )


def test_a_seed_flag_that_the_engine_releases_v8_lacks_is_left_out_and_said() -> None:
    result = persona_features(
        ENGINE_SOURCE, ENGINE_SOURCE, SEED, [switch("V8Flag_gone_flag", False)], baseline_seed=SEED
    )

    assert result.page.v8_flags == ()
    assert result.review == (
        "The seed sets --no-gone_flag, which the Engine Release's V8 does not have, so the Page leaves it out",
    )


def test_a_file_that_sets_v8_flags_and_differs_from_the_engine_releases_goes_to_review() -> None:
    changed = read_release(OLDER, files(OLDER_FEATURES, OLDER_V8_FEATURES, **{"gin_features.cc": "// changed"}))

    review = persona_features(changed, ENGINE_SOURCE, SEED, (), baseline_seed=SEED).review

    assert "gin/gin_features.cc differs from the Engine Release's" in "\n".join(review)


def test_an_undecided_study_goes_to_review_only_if_it_switches_what_a_page_starts_with() -> None:
    undecided = [
        UndecidedStudy("Win24H2", ("min_os_version",), ("Plain", "Unrelated")),
        UndecidedStudy("Browser", ("hardware_class",), ("DefaultBrowserPrompt",)),
        UndecidedStudy("V8", ("max_os_version",), ("V8Flag_maglev",)),
    ]

    review = persona_features(ENGINE_SOURCE, ENGINE_SOURCE, SEED, (), undecided, baseline_seed=SEED).review

    assert [line.split()[3] for line in review] == ["Win24H2", "V8"]


def test_a_key_that_neither_the_generator_nor_this_knows_is_an_error() -> None:
    with pytest.raises(PageFeaturesError, match="does not handle the key future_key"):
        read_release(OLDER, files(OLDER_FEATURES.replace("is_protected_feature", "future_key"), OLDER_V8_FEATURES))


@pytest.mark.parametrize(
    ("path", "text", "message"),
    [
        pytest.param(RUNTIME_ENABLED_FEATURES, "{data: [", "is not JSON5", id="not-json5"),
        pytest.param(RUNTIME_ENABLED_FEATURES, "{parameters: {}}", "has no list of features", id="no-features"),
        pytest.param(f"v8/{V8_FEATURE_FLAGS}", "// nothing", "lists no shipped feature", id="no-v8-features"),
        pytest.param(
            f"v8/{V8_FEATURE_FLAGS}",
            ENGINE_V8_FEATURES.replace("STAGED_FEATURE", "BAKING_FEATURE"),
            "has the stage BAKING",
            id="unknown-v8-stage",
        ),
        pytest.param(CONTENT_RUNTIME_FEATURES, '{"Plain", features::kThing, kEnableOnly}', "kEnableOnly", id="option"),
    ],
)
def test_files_that_are_not_what_this_reads_are_an_error(path: str, text: str, message: str) -> None:
    with pytest.raises(PageFeaturesError, match=message):
        read_release(OLDER, {**files(OLDER_FEATURES, OLDER_V8_FEATURES), path: text})


def gitiles(text: str) -> bytes:
    return base64.b64encode(text.encode())


def answers(builds: Mapping[str, dict[str, str]], seeds: Mapping[int, bytes]) -> dict[str, bytes]:
    """Return what Gitiles and the variations server answer for each build's files and each milestone's seed."""
    found: dict[str, bytes] = {}
    for build, build_files in builds.items():
        pin = build.replace(".", "")[:6].ljust(40, "0")
        found[f"{CHROMIUM_URL}/+/{build}/DEPS?format=TEXT"] = gitiles(f"vars = {{\n  'v8_revision': '{pin}',\n}}")
        for path, text in build_files.items():
            if path.startswith("v8/"):
                found[f"{V8_URL}/+/{pin}/{path.removeprefix('v8/')}?format=TEXT"] = gitiles(text)
            else:
                found[f"{CHROMIUM_URL}/+/{build}/{path}?format=TEXT"] = gitiles(text)
    for milestone, data in seeds.items():
        found[seed_url(milestone)] = data
    return found


def serial(version: str) -> str:
    """Return a seed's serial number of this version, in the form Chrome shows it."""
    return "SM" + base64.b64encode(message(1, version.removesuffix("-production"))).decode() + "#hash"


def baselines_with(tmp_path: Path, seed_version: str = SEED) -> Path:
    """Return a directory of Baselines whose chrome.txt of each Persona names a seed of this version."""
    directory = tmp_path / "baselines"
    for release in WINDOWS_PERSONAS:
        (directory / release).mkdir(parents=True, exist_ok=True)
        (directory / release / "chrome.txt").write_text(
            f"Chrome: {release}\nVariations seed: {serial(seed_version)}\n", encoding="utf-8"
        )
    return directory


def personas_with(tmp_path: Path, *releases: str) -> Path:
    directory = tmp_path / "personas"
    directory.mkdir()
    for release in releases:
        shutil.copy(PERSONAS_DIRECTORY / f"chrome_{release}_windows.json", directory)
        path = directory / f"chrome_{release}_windows.json"
        data = json.loads(path.read_text(encoding="utf-8"))
        build = {OLDER_RELEASE: OLDER, ENGINE_RELEASE: ENGINE}[release]
        data["kinds"]["navigation"]["headers"] = [
            f'sec-ch-ua-full-version: "{build}"' if line.startswith("sec-ch-ua-full-version:") else line
            for line in data["kinds"]["navigation"]["headers"]
        ]
        data.pop("page_features", None)
        path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    return directory


OLDER_SEED = seed(
    study(
        "Deprecation",
        group("Disabled", 100, disable=("ConversionMeasurement",)),
        filter=windows_stable(message(3, f"{ENGINE_RELEASE}.0.8083.0")),
    ),
    study("V8", group("EnabledLaunch", 100, enable=("V8Flag_sqrt_allocation_limits",)), filter=windows_stable()),
)
ENGINE_SEED = seed(study("Other", group("Enabled", 100, enable=("Unrelated",)), filter=number(4, 3) + number(5, 0)))


def test_writing_puts_what_each_page_starts_with_into_its_personas_file_and_reports_it(tmp_path: Path) -> None:
    directory = personas_with(tmp_path, OLDER_RELEASE, ENGINE_RELEASE)
    found = answers(
        {ENGINE: files(ENGINE_FEATURES, ENGINE_V8_FEATURES), OLDER: files(OLDER_FEATURES, OLDER_V8_FEATURES)},
        {int(OLDER_RELEASE): OLDER_SEED, int(ENGINE_RELEASE): ENGINE_SEED},
    )
    before = json.loads((directory / OLDER_FILE).read_text(encoding="utf-8"))

    report = write_page_features(
        ENGINE, datetime.date(2026, 11, 12), found.__getitem__, directory, baselines_with(tmp_path)
    )

    written = json.loads((directory / OLDER_FILE).read_text(encoding="utf-8"))
    assert written.pop("page_features") == {
        "seed": SEED,
        "enable_blink_features": ["OnlyInOlderStable"],
        "disable_blink_features": ["NewInEngine", "StableLater", "Copied"],
        "v8_flags": [
            "--no-js_new",
            "--no-wasm_new_wasm",
            "--no-homomorphic_ic",
            "--js_staged_back",
            "--sqrt_allocation_limits",
        ],
    }
    assert written == before
    engine = json.loads((directory / ENGINE_FILE).read_text(encoding="utf-8"))["page_features"]
    assert engine == {"seed": SEED, "enable_blink_features": [], "disable_blink_features": [], "v8_flags": []}
    assert report.startswith(f"{OLDER_FILE}: Chrome {OLDER}, seed {SEED}\n")
    assert "  disable_blink_features gains: NewInEngine, StableLater, Copied\n" in report
    assert "    Copied off: ConversionMeasurement in Deprecation/Disabled, 100% of installations\n" in report
    assert f"{ENGINE_FILE}: Chrome {ENGINE}, seed {SEED}\n" in report


def test_writing_again_reports_what_changed_only(tmp_path: Path) -> None:
    directory = personas_with(tmp_path, ENGINE_RELEASE)
    found = answers({ENGINE: files(ENGINE_FEATURES, ENGINE_V8_FEATURES)}, {int(ENGINE_RELEASE): ENGINE_SEED})
    write_page_features(ENGINE, datetime.date(2026, 11, 12), found.__getitem__, directory, baselines_with(tmp_path))
    path = directory / ENGINE_FILE
    data = json.loads(path.read_text(encoding="utf-8"))
    data["page_features"]["disable_blink_features"] = ["Stale"]
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")

    report = write_page_features(
        ENGINE, datetime.date(2026, 11, 12), found.__getitem__, directory, baselines_with(tmp_path)
    )

    assert "disable_blink_features loses: Stale" in report
    assert "gains" not in report


def test_a_seed_the_server_does_not_give_is_a_web_error(tmp_path: Path) -> None:
    directory = personas_with(tmp_path, ENGINE_RELEASE)
    found = answers({ENGINE: files(ENGINE_FEATURES, ENGINE_V8_FEATURES)}, {int(ENGINE_RELEASE): b"<html>"})

    with pytest.raises(WebError, match="variations seed"):
        write_page_features(ENGINE, datetime.date(2026, 11, 12), found.__getitem__, directory, baselines_with(tmp_path))


def test_deps_that_pin_no_v8_is_a_web_error(tmp_path: Path) -> None:
    directory = personas_with(tmp_path, ENGINE_RELEASE)
    found = answers({ENGINE: files(ENGINE_FEATURES, ENGINE_V8_FEATURES)}, {int(ENGINE_RELEASE): ENGINE_SEED})
    found[f"{CHROMIUM_URL}/+/{ENGINE}/DEPS?format=TEXT"] = gitiles("vars = {}")

    with pytest.raises(WebError, match="pins no V8"):
        write_page_features(ENGINE, datetime.date(2026, 11, 12), found.__getitem__, directory, baselines_with(tmp_path))


def test_a_seed_other_than_the_baselines_goes_to_review() -> None:
    result = persona_features(ENGINE_SOURCE, ENGINE_SOURCE, SEED, (), baseline_seed="20261106-010110.492000")

    assert result.review == (
        f"The seed is {SEED}, and Chrome {ENGINE}'s Baselines were recorded with 20261106-010110.492000: work this"
        " out on the day the Baselines are recorded",
    )


def test_baselines_that_name_no_seed_go_to_review() -> None:
    review = persona_features(ENGINE_SOURCE, ENGINE_SOURCE, SEED, (), baseline_seed=None).review

    assert review[0].endswith(
        "recorded with no seed that their chrome.txt names: work this out on the day the Baselines are recorded"
    )


def test_writing_compares_the_seed_with_the_one_in_the_baselines_chrome_txt(tmp_path: Path) -> None:
    directory = personas_with(tmp_path, ENGINE_RELEASE)
    found = answers({ENGINE: files(ENGINE_FEATURES, ENGINE_V8_FEATURES)}, {int(ENGINE_RELEASE): ENGINE_SEED})
    older = baselines_with(tmp_path, "20261106-010110.492000-production")

    report = write_page_features(ENGINE, datetime.date(2026, 11, 12), found.__getitem__, directory, older)

    assert "Baselines were recorded with 20261106-010110.492000" in report


def test_the_content_layers_table_is_compared_with_the_engine_releases() -> None:
    changed = read_release(
        OLDER, {**files(OLDER_FEATURES, OLDER_V8_FEATURES), CONTENT_RUNTIME_FEATURES: CONTENT + "//"}
    )

    review = persona_features(changed, ENGINE_SOURCE, SEED, (), baseline_seed=SEED).review

    assert f"{CONTENT_RUNTIME_FEATURES} differs from the Engine Release's" in "\n".join(review)
