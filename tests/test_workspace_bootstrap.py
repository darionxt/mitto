"""Tests of the script that sets up the workspace, none of which need Chromium or depot_tools."""

import os
import shutil
import subprocess
from pathlib import Path

import pytest

from tools.workspace import bootstrap

GIT = shutil.which("git")
needs_git = pytest.mark.skipif(GIT is None, reason="needs git")


def git(repository: Path, *args: str) -> str:
    assert GIT is not None
    return subprocess.run([GIT, "-C", str(repository), *args], check=True, capture_output=True, text=True).stdout


def test_git_settings_come_after_those_the_environment_has() -> None:
    env = bootstrap.with_git_config(
        {"GIT_CONFIG_COUNT": "1", "GIT_CONFIG_KEY_0": "user.name", "GIT_CONFIG_VALUE_0": "A"},
        {"core.autocrlf": "false", "core.eol": "lf"},
    )

    assert env == {
        "GIT_CONFIG_COUNT": "3",
        "GIT_CONFIG_KEY_0": "user.name",
        "GIT_CONFIG_VALUE_0": "A",
        "GIT_CONFIG_KEY_1": "core.autocrlf",
        "GIT_CONFIG_VALUE_1": "false",
        "GIT_CONFIG_KEY_2": "core.eol",
        "GIT_CONFIG_VALUE_2": "lf",
    }


@needs_git
def test_git_reads_the_settings_above_the_users_own(tmp_path: Path) -> None:
    global_config = tmp_path / "gitconfig"
    global_config.write_text("[core]\n\tautocrlf = true\n\teol = crlf\n")
    env = bootstrap.with_git_config({**os.environ, "GIT_CONFIG_GLOBAL": str(global_config)}, bootstrap.GIT_CONFIG)

    def setting(name: str) -> str:
        assert GIT is not None
        return subprocess.run(
            [GIT, "config", "--get", name], env=env, check=True, capture_output=True, text=True
        ).stdout

    assert setting("core.autocrlf") == "false\n"
    assert setting("core.eol") == "lf\n"


@pytest.mark.parametrize("host", ["mac", "linux", "win"])
def test_tools_run_with_depot_tools_first_and_without_the_callers_python(host: bootstrap.Host, tmp_path: Path) -> None:
    workspace = bootstrap.Workspace(tmp_path, "157.0.8103.39")
    base = {"PATH": "/usr/bin", "VIRTUAL_ENV": "/repo/.venv", "PYTHONPATH": "/elsewhere", "HOME": "/home/a"}

    env = bootstrap.tool_environment(workspace, host, base)

    assert env["PATH"].split(os.pathsep) == [str(tmp_path / "depot_tools"), "/usr/bin"]
    assert "VIRTUAL_ENV" not in env
    assert "PYTHONPATH" not in env
    assert env["HOME"] == "/home/a"
    assert env["DEPOT_TOOLS_METRICS"] == "0"
    assert env["VPYTHON_VIRTUALENV_ROOT"] == str(tmp_path / ".cache/vpython")
    assert env["BOTO_PATH"] == str(tmp_path / ".cache/boto.cfg")
    settings = {env[f"GIT_CONFIG_KEY_{i}"]: env[f"GIT_CONFIG_VALUE_{i}"] for i in range(int(env["GIT_CONFIG_COUNT"]))}
    if host == "win":
        assert env["DEPOT_TOOLS_WIN_TOOLCHAIN"] == "0"
        assert settings == {"core.autocrlf": "false", "core.eol": "lf", "core.longpaths": "true"}
    else:
        assert "DEPOT_TOOLS_WIN_TOOLCHAIN" not in env
        assert settings == {"core.autocrlf": "false", "core.eol": "lf"}


def test_depot_tools_commands_are_their_batch_files_on_windows(tmp_path: Path) -> None:
    workspace = bootstrap.Workspace(tmp_path, "157.0.8103.39")

    assert bootstrap.depot_tool(workspace, "win", "gsutil.py") == tmp_path / "depot_tools/gsutil.py.bat"
    assert bootstrap.depot_tool(workspace, "linux", "gclient") == tmp_path / "depot_tools/gclient"


def test_gclient_file_pins_the_revision() -> None:
    text = bootstrap.gclient_file("refs/tags/157.0.8103.39")

    namespace: dict[str, object] = {}
    exec(text, namespace)  # gclient reads the file as Python too.
    assert namespace["solutions"] == [
        {
            "name": "src",
            "url": "https://chromium.googlesource.com/chromium/src.git@refs/tags/157.0.8103.39",
            "managed": True,
            "custom_deps": {},
            "custom_vars": {
                "checkout_configuration": "small",
                "checkout_js_coverage_modules": False,
                "generate_location_tags": False,
            },
        }
    ]


@pytest.mark.parametrize(
    ("value", "message"),
    [
        (None, "set MITTO_WORKSPACE"),
        ("", "set MITTO_WORKSPACE"),
        ("relative/workspace", "must be an absolute path"),
    ],
)
def test_a_workspace_must_be_named_by_an_absolute_path(value: str | None, message: str) -> None:
    with pytest.raises(bootstrap.SetupError, match=message):
        bootstrap.workspace_root(value)


def test_a_workspace_path_has_no_spaces(tmp_path: Path) -> None:
    with pytest.raises(bootstrap.SetupError, match="must not contain spaces"):
        bootstrap.workspace_root(str(tmp_path / "my workspace"))
    assert not (tmp_path / "my workspace").exists()


def test_a_workspace_inside_the_repository_is_refused_before_it_is_made(tmp_path: Path) -> None:
    # As the repository's own path is, once links are resolved.
    tmp_path = tmp_path.resolve()
    repo = tmp_path / "repo"
    repo.mkdir()
    outside = tmp_path / "outside"
    outside.mkdir()
    (outside / "into-repo").symlink_to(repo, target_is_directory=True)

    for value in (repo / "workspace", repo / "native/../workspace", outside / "into-repo/workspace"):
        with pytest.raises(bootstrap.SetupError, match="outside the repository"):
            bootstrap.workspace_root(str(value), repo)
    assert not (repo / "workspace").exists()


def test_a_workspace_is_made_and_resolved(tmp_path: Path) -> None:
    (tmp_path / "real").mkdir()
    (tmp_path / "link").symlink_to(tmp_path / "real", target_is_directory=True)

    root = bootstrap.workspace_root(str(tmp_path / "link/workspace"), tmp_path / "repo")

    assert root == (tmp_path / "real/workspace").resolve()
    assert root.is_dir()


def _checkout_with_a_patch(tmp_path: Path) -> tuple[Path, Path]:
    src = tmp_path / "src"
    src.mkdir()
    git(src, "init", "--quiet")
    (src / "a.cc").write_text("int a = 1;\n")
    git(src, "add", "a.cc")
    git(src, "-c", "user.name=A", "-c", "user.email=a@example.com", "commit", "--quiet", "-m", "a")
    (src / "a.cc").write_text("int a = 2;\n")
    patches = tmp_path / "patches"
    patches.mkdir()
    (patches / "a.patch").write_text(git(src, "diff"))
    git(src, "checkout", "--", "a.cc")
    return src, patches


@needs_git
def test_a_rerun_takes_the_patches_out_and_applies_them_again(tmp_path: Path) -> None:
    assert GIT is not None
    src, patches = _checkout_with_a_patch(tmp_path)
    applied = tmp_path / "applied-patches"
    env = dict(os.environ)

    bootstrap.apply_patches(src, applied, patches, GIT, env, "1.0.0.0")
    assert (src / "a.cc").read_text() == "int a = 2;\n"
    assert (applied / "a.patch").read_bytes() == (patches / "a.patch").read_bytes()

    bootstrap.take_out_patches(src, applied, GIT, env)
    assert (src / "a.cc").read_text() == "int a = 1;\n"
    assert not list(applied.iterdir())
    assert git(src, "status", "--porcelain") == ""

    bootstrap.apply_patches(src, applied, patches, GIT, env, "1.0.0.0")
    assert (src / "a.cc").read_text() == "int a = 2;\n"


@needs_git
def test_a_patch_that_cannot_be_taken_out_stops_the_run(tmp_path: Path) -> None:
    assert GIT is not None
    src, patches = _checkout_with_a_patch(tmp_path)
    applied = tmp_path / "applied-patches"
    bootstrap.apply_patches(src, applied, patches, GIT, dict(os.environ), "1.0.0.0")
    (src / "a.cc").write_text("int a = 3;\n")

    with pytest.raises(bootstrap.SetupError, match=r"cannot take a\.patch out"):
        bootstrap.take_out_patches(src, applied, GIT, dict(os.environ))
    assert (applied / "a.patch").is_file()


@needs_git
def test_a_patch_that_does_not_apply_stops_the_run(tmp_path: Path) -> None:
    assert GIT is not None
    src, patches = _checkout_with_a_patch(tmp_path)
    (src / "a.cc").write_text("int b = 1;\n")

    with pytest.raises(bootstrap.SetupError, match=r"a\.patch does not apply to Chromium 1\.0\.0\.0"):
        bootstrap.apply_patches(src, tmp_path / "applied-patches", patches, GIT, dict(os.environ), "1.0.0.0")


@needs_git
def test_a_clone_without_a_commit_is_removed_and_a_finished_one_kept(tmp_path: Path) -> None:
    assert GIT is not None
    unfinished = tmp_path / "unfinished"
    unfinished.mkdir()
    git(unfinished, "init", "--quiet")
    finished, _ = _checkout_with_a_patch(tmp_path)

    bootstrap.drop_unfinished_clone(unfinished, GIT, dict(os.environ))
    bootstrap.drop_unfinished_clone(finished, GIT, dict(os.environ))

    assert not unfinished.exists()
    assert (finished / "a.cc").is_file()


def test_native_is_linked_once_and_a_stale_link_is_replaced(tmp_path: Path) -> None:
    tmp_path = tmp_path.resolve()
    src = tmp_path / "src"
    src.mkdir()
    native = tmp_path / "native"
    native.mkdir()
    (native / "BUILD.gn").write_text("")

    (src / "mitto").symlink_to(tmp_path / "elsewhere", target_is_directory=True)
    bootstrap.link_native(src, native)
    assert (src / "mitto").resolve() == native
    first = (src / "mitto").lstat()

    bootstrap.link_native(src, native)
    assert (src / "mitto").lstat().st_ino == first.st_ino
    assert (src / "mitto/BUILD.gn").is_file()


def test_a_directory_in_place_of_the_link_stops_the_run(tmp_path: Path) -> None:
    src = tmp_path / "src"
    (src / "mitto").mkdir(parents=True)

    with pytest.raises(bootstrap.SetupError, match="is not a link"):
        bootstrap.link_native(src, tmp_path / "native")


def test_the_link_is_excluded_from_git_once(tmp_path: Path) -> None:
    exclude = tmp_path / ".git/info/exclude"
    exclude.parent.mkdir(parents=True)
    exclude.write_text("# git ls-files --others --exclude-from=.git/info/exclude\n*.swp")

    bootstrap.exclude_link(tmp_path)
    bootstrap.exclude_link(tmp_path)

    assert exclude.read_text() == "# git ls-files --others --exclude-from=.git/info/exclude\n*.swp\n/mitto\n"
