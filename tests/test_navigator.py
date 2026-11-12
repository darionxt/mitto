"""Tests of what a Page's navigator reads of its Session's Persona, Accept-Language and Machine, in src/mitto/_navigator.py.

These need no native library: they check that the values come back from the Persona's own lines, so that a page and the
Session's requests cannot disagree. tests/test_page_navigator.py checks what a Page gives against what a Session sends.
"""

import dataclasses

import pytest

import mitto
from mitto._machine import client_hints
from mitto._native import BrandVersion, PageUserAgentData
from mitto._navigator import accept_languages, page_navigator, user_agent_data
from mitto._persona import WINDOWS_PERSONAS, Persona, RequestKind, load_persona

_MACHINE = mitto.Machine.draw(seed=1, platform_version="10.0.0")


def _brand_list(brands: tuple[BrandVersion, ...]) -> str:
    """Write brands as Chrome's SerializeBrandVersionList() writes them."""
    return ", ".join(f'"{brand.brand}";v="{brand.version}"' for brand in brands)


def _string(text: str) -> str:
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _hint_lines(data: PageUserAgentData) -> dict[str, str]:
    """Write the user agent client hints of `data` as Chrome writes them."""
    return {
        "sec-ch-ua": _brand_list(data.brands),
        "sec-ch-ua-full-version-list": _brand_list(data.full_version_list),
        "sec-ch-ua-mobile": "?1" if data.mobile else "?0",
        "sec-ch-ua-platform": _string(data.platform),
        "sec-ch-ua-platform-version": _string(data.platform_version),
        "sec-ch-ua-arch": _string(data.architecture),
        "sec-ch-ua-bitness": _string(data.bitness),
        "sec-ch-ua-model": _string(data.model),
        "sec-ch-ua-full-version": _string(data.ua_full_version),
        "sec-ch-ua-wow64": "?1" if data.wow64 else "?0",
        "sec-ch-ua-form-factors": ", ".join(_string(form_factor) for form_factor in data.form_factors),
    }


@pytest.mark.parametrize("name", WINDOWS_PERSONAS.values())
def test_the_user_agent_data_writes_back_the_personas_own_hints(name: str) -> None:
    persona = load_persona(name).with_machine(_MACHINE)
    lines = {key.lower(): value for key, value in persona.request_defaults(RequestKind.NAVIGATION)["headers"]}

    data = user_agent_data(load_persona(name), _MACHINE)

    written = _hint_lines(data)
    assert written == {key: lines[key] for key in written}
    assert written["sec-ch-ua-platform-version"] == client_hints(_MACHINE)["sec-ch-ua-platform-version"]


@pytest.mark.parametrize("name", WINDOWS_PERSONAS.values())
def test_the_navigator_has_the_personas_user_agent_and_chromes_platform_on_windows(name: str) -> None:
    persona = load_persona(name)

    navigator = page_navigator(persona, persona.accept_language, _MACHINE)

    assert (navigator.user_agent, navigator.platform, navigator.languages) == (persona.user_agent, "Win32", "en-US,en")
    assert navigator.user_agent_data == user_agent_data(persona, _MACHINE)


@pytest.mark.parametrize(
    ("header", "languages"),
    [
        pytest.param("en-US,en;q=0.9", "en-US,en", id="persona"),
        pytest.param("de-DE, de;q=0.9 ,en;q=0.8", "de-DE,de,en", id="spaces"),
        pytest.param("fr", "fr", id="one"),
        pytest.param("", "", id="none"),
        pytest.param(" , en", "en", id="empty-item"),
    ],
)
def test_the_languages_are_those_of_the_header_without_their_weights(header: str, languages: str) -> None:
    assert accept_languages(header) == languages


def _with_lines(persona: Persona, **values: str) -> Persona:
    """Return the Persona with each of `values`, by hint name with underscores, in its navigation's lines."""
    kinds = persona._kinds  # pyright: ignore[reportPrivateUsage]
    navigation = kinds[RequestKind.NAVIGATION]
    named = {name.replace("_", "-"): value for name, value in values.items()}
    headers = [
        f"{line.partition(': ')[0]}: {named[line.partition(': ')[0].lower()]}"
        if line.partition(": ")[0].lower() in named
        else line
        for line in navigation["headers"]
    ]
    return dataclasses.replace(persona, _kinds={**kinds, RequestKind.NAVIGATION: {**navigation, "headers": headers}})


def test_strings_and_lists_read_back_with_their_escapes() -> None:
    persona = _with_lines(
        load_persona(WINDOWS_PERSONAS["157"]),
        sec_ch_ua='"A \\"quoted\\", brand";v="1", "B\\\\C";v="2"',
        sec_ch_ua_model='"x,y"',
        sec_ch_ua_form_factors='"Desktop", "XR"',
    )

    data = user_agent_data(persona, _MACHINE)

    assert data.brands == (BrandVersion('A "quoted", brand', "1"), BrandVersion("B\\C", "2"))
    assert (data.model, data.form_factors) == ("x,y", ("Desktop", "XR"))


@pytest.mark.parametrize(
    "values",
    [
        pytest.param({"sec_ch_ua": '"Chromium"'}, id="brand-without-version"),
        pytest.param({"sec_ch_ua_mobile": "0"}, id="mobile-not-a-boolean"),
        pytest.param({"sec_ch_ua_arch": "x86"}, id="architecture-not-a-string"),
        pytest.param({"sec_ch_ua_model": '"a"b"'}, id="two-strings"),
    ],
)
def test_a_hint_that_is_not_what_chrome_writes_is_refused(values: dict[str, str]) -> None:
    persona = _with_lines(load_persona(WINDOWS_PERSONAS["157"]), **values)

    with pytest.raises(ValueError, match="is not"):
        user_agent_data(persona, _MACHINE)


def test_a_persona_of_another_operating_system_has_no_navigator_platform() -> None:
    persona = dataclasses.replace(load_persona(WINDOWS_PERSONAS["157"]), operating_system="Linux")

    with pytest.raises(ValueError, match=r"no navigator\.platform is known for a Persona of 'Linux'"):
        page_navigator(persona, persona.accept_language, _MACHINE)
