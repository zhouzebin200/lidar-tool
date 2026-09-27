"""Build an architecture-independent OpenWrt gateway IPK."""

from __future__ import annotations

import argparse
import gzip
from pathlib import Path
import shutil
import tarfile
import tempfile


VERSION = "0.2.0-1"


def add_tree(archive: tarfile.TarFile, source: Path, target: str) -> None:
    for path in sorted(source.rglob("*")):
        if "__pycache__" in path.parts or path.suffix in (".pyc", ".pyo"):
            continue
        relative = path.relative_to(source).as_posix()
        archive_name = f"{target.rstrip('/')}/{relative}"
        info = archive.gettarinfo(str(path), arcname=archive_name)
        info.uid = info.gid = 0
        info.uname = info.gname = "root"
        info.mtime = 0
        executable = relative in ("postinst", "prerm", "etc/init.d/ulv-gateway", "usr/bin/ulv-check")
        info.mode = 0o755 if path.is_dir() or executable else 0o644
        if path.is_file():
            with path.open("rb") as stream:
                archive.addfile(info, stream)
        else:
            archive.addfile(info)


def tar_gz(build: Path, output: Path, data: bool) -> None:
    with output.open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode="w", format=tarfile.GNU_FORMAT) as archive:
                if data:
                    add_tree(archive, build / "data", ".")
                else:
                    add_tree(archive, build / "control", ".")


def ar_member(name: str, content: bytes, timestamp: int = 0) -> bytes:
    header = (
        f"{name + '/':<16}{timestamp:<12}{0:<6}{0:<6}{0o100644:<8o}{len(content):<10}`\n"
    ).encode("ascii")
    return header + content + (b"\n" if len(content) % 2 else b"")


def build(project: Path, output_directory: Path) -> Path:
    output_directory.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ulv-openwrt-") as temporary:
        root = Path(temporary)
        data = root / "data"
        control = root / "control"
        app = data / "usr" / "lib" / "ulv-gateway"
        (data / "etc" / "init.d").mkdir(parents=True)
        (data / "etc" / "config").mkdir(parents=True)
        (data / "etc" / "ulv-gateway").mkdir(parents=True)
        (data / "usr" / "bin").mkdir(parents=True)
        app.mkdir(parents=True)
        control.mkdir(parents=True)

        shutil.copy2(project / "run.py", app / "run.py")
        shutil.copytree(project / "ulv", app / "ulv", ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
        shutil.copytree(project / "web", app / "web")
        (app / "plugins").mkdir()
        shutil.copy2(project / "openwrt" / "files" / "ulv-gateway.init", data / "etc" / "init.d" / "ulv-gateway")
        shutil.copy2(project / "openwrt" / "files" / "ulv-gateway.config", data / "etc" / "config" / "ulv-gateway")
        shutil.copy2(project / "openwrt" / "files" / "ulv-check", data / "usr" / "bin" / "ulv-check")
        (data / "etc" / "init.d" / "ulv-gateway").chmod(0o755)
        (data / "usr" / "bin" / "ulv-check").chmod(0o755)

        control_text = f"""Package: ulv-gateway
Version: {VERSION}
Architecture: all
Maintainer: Universal LiDAR Viewer
Depends: python3, libpcap, linuxptp
Section: net
Priority: optional
Description: Adaptive LiDAR VLAN discovery, UDP forwarding, USB PCAP and PTP web gateway
"""
        (control / "control").write_text(control_text, encoding="utf-8", newline="\n")
        (control / "conffiles").write_text("/etc/config/ulv-gateway\n", encoding="utf-8", newline="\n")
        (control / "postinst").write_text(
            "#!/bin/sh\n[ -n \"$IPKG_INSTROOT\" ] || { /etc/init.d/ulv-gateway enable; /etc/init.d/ulv-gateway restart; }\nexit 0\n",
            encoding="utf-8", newline="\n",
        )
        (control / "prerm").write_text(
            "#!/bin/sh\n[ -n \"$IPKG_INSTROOT\" ] || /etc/init.d/ulv-gateway stop\nexit 0\n",
            encoding="utf-8", newline="\n",
        )
        (control / "postinst").chmod(0o755)
        (control / "prerm").chmod(0o755)

        control_archive = root / "control.tar.gz"
        data_archive = root / "data.tar.gz"
        tar_gz(root, control_archive, data=False)
        tar_gz(root, data_archive, data=True)
        members = [
            ("debian-binary", b"2.0\n"),
            ("control.tar.gz", control_archive.read_bytes()),
            ("data.tar.gz", data_archive.read_bytes()),
        ]
        destination = output_directory / f"ulv-gateway_{VERSION}_all.ipk"
        content = bytearray(b"!<arch>\n")
        for name, value in members:
            content.extend(ar_member(name, value))
        destination.write_bytes(content)
        return destination


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    project = Path(__file__).resolve().parents[1]
    destination = build(project, args.output or project / "dist" / "openwrt")
    print(destination)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
