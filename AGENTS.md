# mitto

A Python library whose HTTP traffic matches real Chrome, built on Chromium's own network stack. Read `CONTEXT.md` for the vocabulary, `docs/adr/` for decisions and `docs/roadmap.md` for the plan.

## Project rules

- C++ follows the Chromium C++ style guide and `styleguide/c++/c++-features.md` (ADR-0002). Generic C++ guidelines that conflict with them do not apply.
- Python targets 3.12+, uses ruff for linting and formatting, and type-checks strictly.
- A change that callers will notice says so in `CHANGELOG.md`, under the version that ships it (`docs/releasing.md`, "What changes for callers").

## Agent skills

### Issue tracker

Issues live in GitHub Issues for darionxt/mitto, managed via the `gh` CLI. See `docs/agents/issue-tracker.md`.

### Triage labels

Default five-role vocabulary (`needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, `wontfix`). See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: one `CONTEXT.md` + `docs/adr/` at the repo root. See `docs/agents/domain.md`.
