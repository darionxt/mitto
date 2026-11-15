"""A handler of `wpt serve`: keeps what a test page posts, in the directory that MITTO_WPT_RESULTS names.

wptserve runs this file by itself, so it imports nothing of mitto's. The runner finds a test's file by result_key().
"""

import hashlib
import json
import os
from pathlib import Path
from typing import Any
from urllib.parse import unquote, urlsplit

RESULTS_ENV_VAR = "MITTO_WPT_RESULTS"


def result_key(address: str) -> str:
    """The name of the file that keeps the results of a test at an address, such as "/dom/nodes/x.html?a" or a whole
    URL. Only the path and the query count, as the page's URL writes them or percent-encoded."""
    parts = urlsplit(address)
    path = parts.path + (f"?{parts.query}" if parts.query else "")
    return hashlib.sha1(unquote(path).encode()).hexdigest() + ".json"


def main(request: Any, response: Any) -> tuple[int, list[tuple[str, str]], bytes]:
    """Write the posted record under the test's key."""
    body: bytes = request.body
    record = json.loads(body)
    (Path(os.environ[RESULTS_ENV_VAR]) / result_key(record["url"])).write_bytes(body)
    return 200, [("Content-Type", "text/plain")], b"kept"
