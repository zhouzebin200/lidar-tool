#!/usr/bin/env python3
"""Licensed entry point; the ordinary run.py intentionally remains unlocked."""

from __future__ import annotations

import sys

from ulv.licensing import find_license, install_license, machine_code, show_license_error


def main() -> int:
    if "--machine-code" in sys.argv:
        print(machine_code())
        return 0
    if "--license-status" in sys.argv:
        result = find_license()
        print(f"{result.reason}: {result.path or '-'}")
        return 0 if result.valid else 3
    if "--install-license" in sys.argv:
        index = sys.argv.index("--install-license")
        if index + 1 >= len(sys.argv):
            print("--install-license requires a license code or file path", file=sys.stderr)
            return 2
        result = install_license(sys.argv[index + 1])
        print(f"{result.reason}: {result.path or '-'}")
        return 0 if result.valid else 3
    result = find_license()
    if not result.valid:
        show_license_error(result.reason)
        return 3
    from ulv.server import main as server_main
    return server_main()


if __name__ == "__main__":
    raise SystemExit(main())
