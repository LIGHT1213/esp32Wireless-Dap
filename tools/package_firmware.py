#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import tempfile
import textwrap
import zipfile
from datetime import datetime, timezone
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Package ESP-IDF build outputs into a release-friendly firmware archive."
    )
    parser.add_argument("--project-dir", required=True, help="ESP-IDF project directory")
    parser.add_argument("--project-name", required=True, help="ESP-IDF app binary base name")
    parser.add_argument("--variant", required=True, help="Firmware variant name, e.g. frontend_a")
    parser.add_argument("--output-dir", required=True, help="Directory for generated archives")
    parser.add_argument("--version", required=True, help="Version label used in archive names")
    parser.add_argument("--commit", default="", help="Source commit SHA")
    return parser.parse_args()


def read_target(project_dir: Path) -> str:
    sdkconfig = project_dir / "sdkconfig"
    for line in sdkconfig.read_text(encoding="utf-8").splitlines():
        if line.startswith('CONFIG_IDF_TARGET="') and line.endswith('"'):
            return line.split('"', 2)[1]
    raise RuntimeError(f"Cannot find CONFIG_IDF_TARGET in {sdkconfig}")


def read_flash_args(build_dir: Path) -> str:
    flash_args = build_dir / "flash_args"
    content = flash_args.read_text(encoding="utf-8").strip()
    return (
        content.replace("bootloader/bootloader.bin", "bootloader.bin")
        .replace("partition_table/partition-table.bin", "partition-table.bin")
        + "\n"
    )


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sanitize_version(version: str) -> str:
    return re.sub(r"[^0-9A-Za-z._-]+", "-", version).strip("-") or "unknown"


def build_package_readme(variant: str, target: str, project_name: str) -> str:
    return textwrap.dedent(
        f"""\
        ESP32 Wireless DAP firmware package

        Variant: {variant}
        Target: {target}

        Files in this package:
        - bootloader.bin
        - partition-table.bin
        - {project_name}.bin
        - flash_args.txt
        - FLASH_COMMAND.txt
        - manifest.json

        Flash command:
        python -m esptool --chip {target} --port COMx --baud 460800 --before default_reset --after hard_reset write_flash @flash_args.txt

        Typical ports in the current hardware setup:
        - frontend_a -> COM3
        - backend_b -> COM5
        """
    )


def main() -> int:
    args = parse_args()
    project_dir = Path(args.project_dir).resolve()
    build_dir = project_dir / "build"
    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    project_name = args.project_name
    variant = args.variant
    version = sanitize_version(args.version)
    archive_stem = f"esp32-wireless-dap-{variant}-{version}"
    target = read_target(project_dir)

    app_bin = build_dir / f"{project_name}.bin"
    bootloader_bin = build_dir / "bootloader" / "bootloader.bin"
    partition_bin = build_dir / "partition_table" / "partition-table.bin"
    flash_args_text = read_flash_args(build_dir)

    required_files = [app_bin, bootloader_bin, partition_bin]
    missing_files = [str(path) for path in required_files if not path.exists()]
    if missing_files:
        raise RuntimeError(f"Missing build outputs: {', '.join(missing_files)}")

    manifest = {
        "name": archive_stem,
        "variant": variant,
        "project_name": project_name,
        "target": target,
        "version": args.version,
        "commit": args.commit,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "files": [
            "bootloader.bin",
            "partition-table.bin",
            f"{project_name}.bin",
            "flash_args.txt",
            "FLASH_COMMAND.txt",
            "SHA256SUMS.txt",
            "README.txt",
        ],
    }

    flash_command = (
        f"python -m esptool --chip {target} --port COMx --baud 460800 "
        "--before default_reset --after hard_reset write_flash @flash_args.txt\n"
    )

    archive_path = output_dir / f"{archive_stem}.zip"
    checksum_path = output_dir / f"{archive_stem}.sha256"

    with tempfile.TemporaryDirectory(prefix=f"{archive_stem}-") as temp_dir_name:
        temp_dir = Path(temp_dir_name)
        staging_dir = temp_dir / archive_stem
        staging_dir.mkdir(parents=True, exist_ok=True)

        shutil.copy2(bootloader_bin, staging_dir / "bootloader.bin")
        shutil.copy2(partition_bin, staging_dir / "partition-table.bin")
        shutil.copy2(app_bin, staging_dir / f"{project_name}.bin")
        (staging_dir / "flash_args.txt").write_text(flash_args_text, encoding="utf-8")
        (staging_dir / "FLASH_COMMAND.txt").write_text(flash_command, encoding="utf-8")
        (staging_dir / "README.txt").write_text(
            build_package_readme(
                variant=variant, target=target, project_name=project_name
            ),
            encoding="utf-8",
        )
        (staging_dir / "manifest.json").write_text(
            json.dumps(manifest, indent=2, ensure_ascii=True) + "\n", encoding="utf-8"
        )

        checksums = []
        for file_name in sorted(
            [
                "bootloader.bin",
                "partition-table.bin",
                f"{project_name}.bin",
                "flash_args.txt",
                "FLASH_COMMAND.txt",
                "README.txt",
                "manifest.json",
            ]
        ):
            checksums.append(f"{file_sha256(staging_dir / file_name)}  {file_name}")
        (staging_dir / "SHA256SUMS.txt").write_text(
            "\n".join(checksums) + "\n", encoding="utf-8"
        )

        if archive_path.exists():
            archive_path.unlink()

        with zipfile.ZipFile(archive_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for item in sorted(staging_dir.iterdir(), key=lambda path: path.name):
                archive.write(item, arcname=f"{archive_stem}/{item.name}")

    checksum_path.write_text(
        f"{file_sha256(archive_path)}  {archive_path.name}\n", encoding="utf-8"
    )

    print(f"Created {archive_path}")
    print(f"Created {checksum_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
