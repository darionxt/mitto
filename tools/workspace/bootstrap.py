#!/usr/bin/env python3
"""Sets up the workspace that the native core builds in, on every Host.

The workspace holds depot_tools, a shallow Chromium checkout pinned to the tag
in tools/workspace/chromium_tag with the patches in tools/workspace/patches
applied, and the out/mitto build directory for the native core:

    MITTO_WORKSPACE=/absolute/path uv run tools/workspace/bootstrap.py [gclient args]

Extra arguments, such as --verbose, are passed to gclient sync. Safe to rerun.
Each release in chromium_tag gets its own checkout directory. See
docs/building.md.

This file needs only the standard library, as export.py does.
"""

import argparse
import ctypes
import dataclasses
import filecmp
import os
import platform
import re
import shutil
import stat
import subprocess
import sys
from collections.abc import Callable, Mapping, Sequence
from pathlib import Path
from typing import Literal, NoReturn

REPO = Path(__file__).resolve().parents[2]
TAG_FILE = REPO / "tools/workspace/chromium_tag"
ARGS_GN = REPO / "tools/workspace/args.gn"
PATCHES = REPO / "tools/workspace/patches"
NATIVE = REPO / "native"

DEPOT_TOOLS_URL = "https://chromium.googlesource.com/chromium/tools/depot_tools.git"
SRC_URL = "https://chromium.googlesource.com/chromium/src.git"

# Nothing in the Chromium tree refers to //mitto, so make it the root of the
# build graph. That also leaves out the browser and everything else that the
# native core does not depend on.
GN_ROOT = ("--root-target=//mitto", "--root-pattern=//mitto:*")

# How git writes the files of a checkout, set for each git command that the
# set-up runs rather than in the user's own configuration, so that every file
# has the same bytes on every Host. Without core.eol, Git for Windows writes
# CRLF into every file that a .gitattributes marks as text, such as all of
# expat's. Chromium's git server has no Git LFS, so a file that a dependency
# keeps in LFS stays the pointer that git holds, as depot_tools asks with
# GIT_LFS_SKIP_SMUDGE. That variable needs git-lfs itself, which MinGit lacks
# while its own configuration still requires the filter, so the filter does
# nothing at all.
GIT_CONFIG = {
    "core.autocrlf": "false",
    "core.eol": "lf",
    "filter.lfs.clean": "",
    "filter.lfs.smudge": "",
    "filter.lfs.process": "",
    "filter.lfs.required": "false",
}
# Lets Git for Windows write paths longer than Windows' 260 characters without
# the system-wide setting that would allow them for every program.
WINDOWS_GIT_CONFIG = {"core.longpaths": "true"}

# ES_CONTINUOUS | ES_SYSTEM_REQUIRED: the machine stays awake until this
# process exits or says otherwise.
WINDOWS_STAY_AWAKE = 0x80000001

type Host = Literal["mac", "linux", "win"]


class SetupError(Exception):
    """Stops the set-up with a message for the person running it."""


def die(message: str) -> NoReturn:
    raise SetupError(message)


def note(message: str) -> None:
    print(f"bootstrap: {message}", file=sys.stderr, flush=True)


@dataclasses.dataclass(frozen=True)
class Workspace:
    """The directories of a workspace, for one Chromium release."""

    root: Path
    tag: str

    @property
    def depot_tools(self) -> Path:
        return self.root / "depot_tools"

    @property
    def cache(self) -> Path:
        return self.root / ".cache"

    @property
    def chromium(self) -> Path:
        return self.root / f"chromium-{self.tag}"

    @property
    def src(self) -> Path:
        return self.chromium / "src"

    @property
    def applied_patches(self) -> Path:
        return self.chromium / "applied-patches"


def read_tag() -> str:
    tag = TAG_FILE.read_text().strip()
    if not re.fullmatch(r"\d+\.0\.\d+\.\d+", tag):
        die(f"tools/workspace/chromium_tag holds '{tag}', not a Chromium version")
    return tag


def current_host() -> Host:
    match sys.platform, platform.machine():
        case "darwin", "arm64":
            return "mac"
        case "linux", "x86_64":
            return "linux"
        case "win32", "AMD64":
            return "win"
        case system, machine:
            die(f"unsupported host {system}/{machine}; use macOS arm64, Linux x86_64 or Windows x64")


def workspace_root(value: str | None, repo: Path = REPO) -> Path:
    """Returns the directory that MITTO_WORKSPACE names, created if need be."""
    if not value:
        die("set MITTO_WORKSPACE to an absolute path outside the repository")
    if not Path(value).is_absolute():
        die("MITTO_WORKSPACE must be an absolute path")
    if any(character.isspace() for character in value):
        die("MITTO_WORKSPACE must not contain spaces; Chromium does not support them")
    # Resolved before it exists, so that nothing is made inside the repository.
    root = Path(value).resolve()
    if root.is_relative_to(repo):
        die("MITTO_WORKSPACE must be outside the repository")
    root.mkdir(parents=True, exist_ok=True)
    return root


def with_git_config(env: Mapping[str, str], config: Mapping[str, str]) -> dict[str, str]:
    """Adds settings for every git command to an environment, after those it has.

    git reads them in each command, above the user's own configuration and the
    repository's, so they also reach every git that depot_tools and gclient run.
    """
    result = dict(env)
    count = int(result.get("GIT_CONFIG_COUNT", "0"))
    for key, value in config.items():
        result[f"GIT_CONFIG_KEY_{count}"] = key
        result[f"GIT_CONFIG_VALUE_{count}"] = value
        count += 1
    result["GIT_CONFIG_COUNT"] = str(count)
    return result


def tool_environment(workspace: Workspace, host: Host, base: Mapping[str, str]) -> dict[str, str]:
    """Returns the environment that depot_tools, gclient, git and gn run in."""
    # A virtual environment that started this script would otherwise be where
    # the tools of depot_tools install their packages.
    env = {name: value for name, value in base.items() if name not in {"PYTHONHOME", "PYTHONPATH", "VIRTUAL_ENV"}}
    # First on PATH: the depot_tools wrappers find vpython3 through PATH, so
    # another installation earlier on it would run in place of this one.
    env["PATH"] = os.pathsep.join(entry for entry in (str(workspace.depot_tools), base.get("PATH", "")) if entry)
    env["DEPOT_TOOLS_METRICS"] = "0"
    # Keep the Python environments that depot_tools creates, and gsutil's
    # state, inside the workspace instead of the user's home directory. gsutil
    # reads its state_dir from a boto file; BOTO_PATH names one without making
    # gsutil.py wrap every call in luci-auth, as BOTO_CONFIG would.
    env["VPYTHON_VIRTUALENV_ROOT"] = str(workspace.cache / "vpython")
    env["UV_CACHE_DIR"] = str(workspace.cache / "uv")
    env["BOTO_PATH"] = str(workspace.cache / "boto.cfg")
    config = dict(GIT_CONFIG)
    if host == "win":
        # Use the Visual Studio installed on the machine, rather than the
        # packaged copy that only Google's own machines can download.
        env["DEPOT_TOOLS_WIN_TOOLCHAIN"] = "0"
        config |= WINDOWS_GIT_CONFIG
    return with_git_config(env, config)


def depot_tool(workspace: Workspace, host: Host, name: str) -> Path:
    """Returns a depot_tools command. On Windows its wrapper runs in cmd.exe, as Chromium asks, whatever the shell."""
    return workspace.depot_tools / (f"{name}.bat" if host == "win" else name)


def find_git(env: Mapping[str, str]) -> str:
    git = shutil.which("git", path=env["PATH"])
    if git is None:
        die("git is not on PATH; see docs/building.md")
    return git


def run(command: Sequence[str | Path], env: Mapping[str, str], cwd: Path | None = None, quiet: bool = False) -> None:
    subprocess.run(
        [str(part) for part in command], cwd=cwd, env=env, check=True, stdout=subprocess.DEVNULL if quiet else None
    )


def run_awake(command: Sequence[str | Path], host: Host, env: Mapping[str, str], cwd: Path) -> None:
    """Runs a long download, which a machine that went to sleep would break."""
    if host == "mac":
        command = ["caffeinate", "-i", *command]
    if sys.platform == "win32":
        ctypes.windll.kernel32.SetThreadExecutionState(WINDOWS_STAY_AWAKE)
    run(command, env, cwd)


def write_if_changed(path: Path, text: str) -> None:
    if not path.is_file() or path.read_text() != text:
        path.write_text(text, newline="\n")


def remove_tree(path: Path) -> None:
    """Removes a directory, including the read-only files that git leaves on Windows."""

    def make_writable_and_retry(function: Callable[[str], object], name: str, error: BaseException) -> None:
        # Windows refuses to delete a read-only file, and git makes its objects
        # read-only. Elsewhere that flag is no reason for a failure.
        if sys.platform != "win32":
            raise error
        Path(name).chmod(stat.S_IWRITE)
        function(name)

    shutil.rmtree(path, onexc=make_writable_and_retry)


def gclient_file(revision: str) -> str:
    return f"""\
# Written by mitto's tools/workspace/bootstrap.py; local edits are overwritten.
solutions = [
  {{
    "name": "src",
    "url": "{SRC_URL}@{revision}",
    "managed": True,
    "custom_deps": {{}},
    "custom_vars": {{
      # Skip dependencies that only Chromium's own bots and tests need.
      "checkout_configuration": "small",
      "checkout_js_coverage_modules": False,
      "generate_location_tags": False,
    }},
  }},
]
"""


def drop_unfinished_clone(src: Path, git: str, env: Mapping[str, str]) -> None:
    """Removes a first clone of src that was interrupted.

    It leaves a repository without a commit, which gclient would then fetch
    with full history, so that clone starts over.
    """
    if not (src / ".git").is_dir():
        return
    head = subprocess.run(
        [git, "-C", str(src), "rev-parse", "--verify", "--quiet", "HEAD"],
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )
    # git says 1 for a HEAD without a commit, and more when it cannot read the
    # repository at all, which is no reason to delete it.
    if head.returncode == 1:
        note("removing the unfinished first clone of src")
        remove_tree(src)
    elif head.returncode != 0:
        die(f"git cannot read {src}: {head.stderr.strip()}")


def pinned_revision(workspace: Workspace, git: str, env: Mapping[str, str]) -> str:
    """Returns what .gclient names for src.

    gclient fetches every branch without a depth limit whenever the revision
    it is given is not a commit the checkout already has. So each release gets
    its own checkout, and only its first sync, which is shallow, names the tag;
    later syncs name the commit it checked out.
    """
    if not (workspace.src / ".git").is_dir():
        return f"refs/tags/{workspace.tag}"
    head = subprocess.run(
        [git, "-C", str(workspace.src), "rev-parse", "HEAD"], env=env, check=True, capture_output=True, text=True
    )
    return head.stdout.strip()


def take_out_patches(src: Path, applied: Path, git: str, env: Mapping[str, str]) -> None:
    """Takes out what the last run applied, which it left copies of.

    gclient refuses to sync a checkout with local changes, and the patches in
    the repository may have changed since.
    """
    if not (src / ".git").is_dir():
        return
    for patch in sorted(applied.glob("*.patch"), reverse=True):
        reverse = subprocess.run([git, "-C", str(src), "apply", "--reverse", str(patch)], env=env)
        if reverse.returncode != 0:
            die(f"cannot take {patch.name} out of {src}; undo other changes to the files it touches")
        patch.unlink()


def apply_patches(src: Path, applied: Path, patches: Path, git: str, env: Mapping[str, str], tag: str) -> None:
    """Applies the only changes to Chromium's source. Each file starts with its reason."""
    applied.mkdir(parents=True, exist_ok=True)
    for patch in sorted(patches.glob("*.patch")):
        note(f"applying {patch.name}")
        if subprocess.run([git, "-C", str(src), "apply", str(patch)], env=env).returncode != 0:
            die(f"{patch.name} does not apply to Chromium {tag}; see docs/building.md")
        shutil.copyfile(patch, applied / patch.name)


def fetch_builtins_profiles(workspace: Workspace, env: Mapping[str, str]) -> None:
    """Fetches the profiles by which V8 lays out its builtins in Chrome's releases.

    The Runtime's build reads them, as tools/workspace/runtime_args.gn asks.
    Chromium's DEPS fetches them only with checkout_pgo_profiles, which also
    fetches Chrome's own profiles for its compiler, of no use here. So this runs
    V8's own script, as that hook of DEPS does. It downloads nothing when the
    profiles for this V8 are there already.
    """
    script = workspace.src / "v8/tools/builtins-pgo/download_profiles.py"
    run(
        [sys.executable, script, "download", "--depot-tools", workspace.depot_tools, "--check-v8-revision", "--quiet"],
        env,
        workspace.src,
    )


def link_native(src: Path, native: Path = NATIVE) -> None:
    """Exposes the repository's native sources to GN as //mitto.

    Windows makes a symbolic link only with a privilege that most users lack,
    so the link is a directory junction there.
    """
    link = src / "mitto"
    is_link = link.is_symlink() or link.is_junction()
    if is_link and link.resolve() == native:
        return
    if is_link:
        # rmdir removes a junction, or a link to a directory, on Windows.
        if sys.platform == "win32":
            link.rmdir()
        else:
            link.unlink()
    elif link.exists():
        die(f"{link} is not a link; move it away so that it can link to {native}")
    if sys.platform == "win32":
        run(["cmd", "/c", "mklink", "/J", link, native], os.environ, quiet=True)
    else:
        link.symlink_to(native, target_is_directory=True)


def exclude_link(src: Path) -> None:
    """Keeps //mitto out of the checkout's git status."""
    exclude = src / ".git/info/exclude"
    text = exclude.read_text() if exclude.is_file() else ""
    if "/mitto" in text.splitlines():
        return
    exclude.parent.mkdir(parents=True, exist_ok=True)
    if text and not text.endswith("\n"):
        text += "\n"
    exclude.write_text(text + "/mitto\n", newline="\n")


def copy_args(src: Path) -> None:
    """Puts args.gn into out/mitto, whose copy is overwritten on every run."""
    build_dir = src / "out/mitto"
    build_dir.mkdir(parents=True, exist_ok=True)
    target = build_dir / "args.gn"
    if not target.is_file() or not filecmp.cmp(ARGS_GN, target, shallow=False):
        shutil.copyfile(ARGS_GN, target)


def check_xcode() -> None:
    """Stops unless xcode-select points into a full Xcode, which gn gen needs on macOS."""
    selected = subprocess.run(["xcode-select", "-p"], capture_output=True, text=True)
    developer_dir = selected.stdout.strip() if selected.returncode == 0 else ""
    if not (Path(developer_dir) / "Platforms/MacOSX.platform/Developer/SDKs").is_dir():
        die(
            "the checkout is ready, but gn gen needs a full Xcode installation"
            f" and xcode-select points to '{developer_dir}'; see docs/building.md"
        )


def set_up(workspace: Workspace, host: Host, gclient_args: Sequence[str]) -> None:
    env = tool_environment(workspace, host, os.environ)
    git = find_git(env)

    # depot_tools updates itself whenever gclient runs, so a clone is enough.
    if not (workspace.depot_tools / ".git").is_dir():
        note("installing depot_tools")
        run([git, "clone", DEPOT_TOOLS_URL, workspace.depot_tools], env)

    workspace.cache.mkdir(exist_ok=True)
    write_if_changed(Path(env["BOTO_PATH"]), f"[GSUtil]\nstate_dir = {workspace.cache / 'gsutil'}\n")

    workspace.chromium.mkdir(exist_ok=True)
    drop_unfinished_clone(workspace.src, git, env)
    write_if_changed(workspace.chromium / ".gclient", gclient_file(pinned_revision(workspace, git, env)))

    # gclient downloads Google Storage objects in parallel, and each download
    # waits only 30 seconds for another one to finish installing gsutil.
    # Install it first so that a slow connection does not fail the sync.
    run([depot_tool(workspace, host, "gsutil.py"), "version"], env, quiet=True)

    take_out_patches(workspace.src, workspace.applied_patches, git, env)
    note(f"syncing Chromium {workspace.tag}; the first run downloads about 10 GB")
    note("on that run, the server prepares src for several minutes before any data arrives")
    gclient = depot_tool(workspace, host, "gclient")
    run_awake(
        [gclient, "sync", "--no-history", "--delete_unversioned_trees", *gclient_args], host, env, workspace.chromium
    )
    apply_patches(workspace.src, workspace.applied_patches, PATCHES, git, env, workspace.tag)
    fetch_builtins_profiles(workspace, env)

    link_native(workspace.src)
    exclude_link(workspace.src)
    copy_args(workspace.src)
    if host == "mac":
        check_xcode()
    run([depot_tool(workspace, host, "gn"), "gen", "out/mitto", *GN_ROOT], env, workspace.src)
    note(f"done: {workspace.src / 'out/mitto'} is ready")


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Sets up the workspace in MITTO_WORKSPACE that the native core builds in.",
        epilog="Other arguments, such as --verbose, are passed to gclient sync. See docs/building.md.",
    )
    _, gclient_args = parser.parse_known_args(argv)
    try:
        tag = read_tag()
        host = current_host()
        set_up(Workspace(workspace_root(os.environ.get("MITTO_WORKSPACE")), tag), host, gclient_args)
    except SetupError as error:
        print(f"bootstrap: error: {error}", file=sys.stderr)
        return 1
    except subprocess.CalledProcessError as error:
        print(f"bootstrap: error: {Path(error.cmd[0]).name} exited with {error.returncode}", file=sys.stderr)
        return error.returncode if error.returncode > 0 else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
