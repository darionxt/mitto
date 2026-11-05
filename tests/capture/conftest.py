"""Hypothesis settings for the capture kit's tests."""

from hypothesis import settings

# A pull request must not fail on an input no one touched, so the default run looks at the same inputs every time.
settings.register_profile("capture", derandomize=True, deadline=None)
# For looking for new failures: uv run pytest tests/capture/test_fuzz.py --hypothesis-profile=fuzz
settings.register_profile("fuzz", max_examples=100_000, deadline=None)
settings.load_profile("capture")
