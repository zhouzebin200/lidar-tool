#!/bin/sh
if [ -n "${ZIG_EXECUTABLE:-}" ] && [ -x "${ZIG_EXECUTABLE}" ]; then
    ZIG="${ZIG_EXECUTABLE}"
elif command -v zig >/dev/null 2>&1; then
    ZIG="$(command -v zig)"
else
    ZIG="$(python3 -c 'import pathlib, ziglang; print(pathlib.Path(ziglang.__file__).parent / "zig")' 2>/dev/null || true)"
fi
exec "${ZIG:-zig}" cc -target x86_64-windows-gnu -mcpu=x86_64 "$@"
