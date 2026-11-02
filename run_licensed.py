#!/usr/bin/env python3
"""Licensed entry point; the ordinary run.py intentionally remains unlocked."""

from __future__ import annotations

import sys

from ulv.licensing import LicenseSession, install_license, machine_code, show_license_error, write_machine_code_file


def main() -> int:
    if "--machine-code" in sys.argv:
        print(machine_code())
        write_machine_code_file()
        return 0
    if "--license-status" in sys.argv:
        result = LicenseSession().check()
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
    session = LicenseSession()
    result = session.check()
    if not result.valid:
        show_license_error(result.reason)
        return 3
    from ulv.server import main as server_main
    return server_main(license_session=session)


if __name__ == "__main__":
    raise SystemExit(main())
