"""The Runtime's bindings generator: reads Blink's Web IDL database and writes V8 bindings.

docs/runtime.md says how it works. It runs on the build's own Python, which is 3.11 in the Engine Release's tree, so
it uses nothing newer.
"""
