"""Opens a Session on a storage directory that another process wrote, runs a script in a Page at a URL, and prints what
the script gave and the Machine that the Session claims.

A process of its own, so that only the directory can bring the storage back: nothing of the process that wrote it is
here. tests/test_page_storage.py runs this, with the server in its own process.
"""

import argparse
import dataclasses
import json

import mitto

# Longer than any local page takes, even on a busy machine.
_TIMEOUT_SECONDS = 60.0


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("storage", help="the storage directory")
    parser.add_argument("url", help="the page to open")
    parser.add_argument("script", help="what to run in the page")
    arguments = parser.parse_args()
    with (
        mitto.Session(_storage=arguments.storage) as session,
        session._open_page(arguments.url, timeout=_TIMEOUT_SECONDS) as page,  # pyright: ignore[reportPrivateUsage]
    ):
        result = page.evaluate(arguments.script)
        machine = session.machine
    print(json.dumps({"result": result, "machine": dataclasses.asdict(machine)}))


if __name__ == "__main__":
    main()
