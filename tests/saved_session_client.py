"""Opens a Session on a file of cookies that another process saved, gets each URL in turn, and prints the Machine it claims.

A process of its own, so that only the file can bring the Session back: nothing of the process that saved it is here.
tests/test_client_hints.py runs this, with the server in its own process.
"""

import argparse
import asyncio
import dataclasses
import json

import mitto
from mitto._native import PrivateSettings

# Longer than any local request takes, even on a busy machine.
_TIMEOUT_SECONDS = 30.0


def _settings(spki_hash: str) -> PrivateSettings:
    """Return what reaches the test's server: every name on this machine, and its certificate taken as it is."""
    return PrivateSettings(ignore_certificate_errors_spki_list=[spki_hash], host_resolver_rules="MAP * 127.0.0.1")


def _get_each(cookie_file: str, spki_hash: str, urls: list[str]) -> mitto.Machine:
    with mitto.Session(cookie_file=cookie_file, timeout=_TIMEOUT_SECONDS, _settings=_settings(spki_hash)) as session:
        for url in urls:
            session.get(url)
        return session.machine


async def _get_each_on_a_loop(cookie_file: str, spki_hash: str, urls: list[str]) -> mitto.Machine:
    async with mitto.AsyncSession(
        cookie_file=cookie_file, timeout=_TIMEOUT_SECONDS, _settings=_settings(spki_hash)
    ) as session:
        for url in urls:
            await session.get(url)
        return session.machine


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cookie_file")
    parser.add_argument("spki_hash", help="the hash of the server certificate's key, which the Session takes as it is")
    parser.add_argument("urls", nargs="+")
    parser.add_argument("--asyncio", action="store_true", help="go through an AsyncSession")
    arguments = parser.parse_args()
    if arguments.asyncio:
        machine = asyncio.run(_get_each_on_a_loop(arguments.cookie_file, arguments.spki_hash, arguments.urls))
    else:
        machine = _get_each(arguments.cookie_file, arguments.spki_hash, arguments.urls)
    print(json.dumps(dataclasses.asdict(machine)))


if __name__ == "__main__":
    main()
