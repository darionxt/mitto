"""Tests of the Page program, mitto_page, which runs the script on its command line on V8 and prints what it returns.

CI's builds leave the Runtime out until the source export carries it, so these run only where MITTO_PAGE_PROGRAM names
a program built with tools/workspace/runtime_args.gn. What a page sees is compared with Chrome elsewhere: these check
what holds of any Chrome, such as compilers that give a page WebAssembly, and ICU's whole data behind Intl.
"""

import os
import re
import subprocess
from pathlib import Path

import pytest

_PROGRAM_ENV_VAR = "MITTO_PAGE_PROGRAM"
_TIMEOUT_SECONDS = 60
_SCRIPT_THREW = 1
_WRONG_ARGUMENTS = 2


@pytest.fixture
def program() -> Path:
    """Return the Page program, or skip if the environment names none."""
    path = os.environ.get(_PROGRAM_ENV_VAR)
    if not path:
        pytest.skip(f"no Page program; set {_PROGRAM_ENV_VAR} to run this test")
    return Path(path)


def _run(program: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [program, *arguments], capture_output=True, encoding="utf-8", timeout=_TIMEOUT_SECONDS, check=False
    )


def _evaluate(program: Path, script: str) -> str:
    result = _run(program, script)
    assert (result.returncode, result.stderr) == (0, "")
    return result.stdout.removesuffix("\n")


def test_it_prints_the_version_of_v8(program: Path) -> None:
    result = _run(program, "--version")

    assert result.returncode == 0
    assert re.fullmatch(r"\d+\.\d+\.\d+\.\d+\n", result.stdout)


@pytest.mark.parametrize(
    ("script", "printed"),
    [
        pytest.param("1 + 1", "2", id="number"),
        pytest.param("[1, 'two', null]", "1,two,", id="array"),
        pytest.param("'b\\u00fccher \\u{1F600}'", "bücher \U0001f600", id="utf-8"),
        pytest.param("let a = 6; a * 7", "42", id="last-statement"),
        pytest.param("undefined", "undefined", id="undefined"),
    ],
)
def test_it_prints_what_the_script_returns_as_string_writes_it(program: Path, script: str, printed: str) -> None:
    assert _evaluate(program, script) == printed


def test_its_compilers_give_a_page_webassembly(program: Path) -> None:
    # V8 leaves WebAssembly out of the global object without its compilers.
    assert _evaluate(program, "typeof WebAssembly") == "object"


def test_intl_has_icus_whole_data(program: Path) -> None:
    # With only the few items that the library holds, Intl would know the root locale alone.
    script = """
        [
          Intl.DateTimeFormat.supportedLocalesOf(["ar", "de", "hi", "ja", "zh-Hant"]).length,
          Intl.supportedValuesOf("timeZone").length > 400,
          new Intl.Collator("de").resolvedOptions().locale,
        ].join(" ")
    """

    assert _evaluate(program, script) == "5 true de"


def test_a_script_that_throws_prints_what_it_threw(program: Path) -> None:
    result = _run(program, "throw new TypeError('no ' + 'good')")

    assert (result.returncode, result.stdout, result.stderr) == (_SCRIPT_THREW, "", "Uncaught TypeError: no good\n")


def test_a_script_that_does_not_compile_prints_why(program: Path) -> None:
    result = _run(program, "let = ;")

    assert result.returncode == _SCRIPT_THREW
    assert result.stderr.startswith("Uncaught SyntaxError: ")


def test_a_result_that_string_cannot_write_counts_as_thrown(program: Path) -> None:
    result = _run(program, "Symbol('x')")

    assert result.returncode == _SCRIPT_THREW
    assert result.stderr.startswith("Uncaught TypeError: ")


@pytest.mark.parametrize("arguments", [(), ("1", "2")], ids=["none", "two"])
def test_it_takes_exactly_one_argument(program: Path, arguments: tuple[str, ...]) -> None:
    result = _run(program, *arguments)

    assert result.returncode == _WRONG_ARGUMENTS
    assert result.stderr.startswith("usage: ")
