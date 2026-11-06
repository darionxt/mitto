"""The capture kit: a local server that records what a client sends, raw and in the normal form Baselines use."""

from tools.capture.cases import (
    CASE_HOST,
    CASES,
    COOKIE_SET_PATH,
    CROSS_SITE_HOST,
    MAIN_CASES,
    RUNS,
    SAME_SITE_HOST,
    Case,
    Run,
)
from tools.capture.normal_form import RecordingError, dumps_normal_form, normal_form
from tools.capture.page import KIND_PATHS, RequestKind
from tools.capture.recording import (
    HTTP1,
    HTTP2,
    HTTP3,
    PLAIN_HTTP,
    PROXIED_HTTP1,
    PROXIED_HTTP2,
    PROXY,
    Recording,
    raw_form,
)
from tools.capture.server import DEFAULT_HOST, CaptureServer

__all__ = [
    "CASES",
    "CASE_HOST",
    "COOKIE_SET_PATH",
    "CROSS_SITE_HOST",
    "DEFAULT_HOST",
    "HTTP1",
    "HTTP2",
    "HTTP3",
    "KIND_PATHS",
    "MAIN_CASES",
    "PLAIN_HTTP",
    "PROXIED_HTTP1",
    "PROXIED_HTTP2",
    "PROXY",
    "RUNS",
    "SAME_SITE_HOST",
    "CaptureServer",
    "Case",
    "Recording",
    "RecordingError",
    "RequestKind",
    "Run",
    "dumps_normal_form",
    "normal_form",
    "raw_form",
]
