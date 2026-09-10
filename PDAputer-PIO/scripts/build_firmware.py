# Generic PlatformIO ESP32 firmware merger.
#
# Add this file to a PlatformIO project and register it in platformio.ini:
#
#   extra_scripts =
#       post:scripts/build_firmware.py
#
# It creates a single flashable .bin in the firmware/ directory by merging the
# bootloader, partition table, app, and other binary segments from PlatformIO's
# real upload command.
#
# Optional:
#   - Define FIRMWARE_VERSION in a source/header file to include it in filenames.
#   - Set custom_firmware_version in platformio.ini to override version detection.
#   - Set custom_firmware_output_dir in platformio.ini to change output directory.
#   - Set custom_firmware_manifest = no to disable firmware.json generation.

import json
import os
import re
from pathlib import Path

Import("env")


PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
DEFAULT_OUTPUT_DIR = "firmware"
DEFAULT_MANIFEST_NAME = "firmware.json"


def project_option(name, default=None):
    try:
        return env.GetProjectOption(name, default)
    except Exception:
        return default


def truthy(value):
    return str(value).strip().lower() not in ("0", "false", "no", "off")


def firmware_output_dir():
    configured = project_option("custom_firmware_output_dir", DEFAULT_OUTPUT_DIR)
    output = Path(str(configured))
    if not output.is_absolute():
        output = PROJECT_DIR / output
    return output


def manifest_enabled():
    return truthy(project_option("custom_firmware_manifest", "yes"))


def detect_version():
    configured = project_option("custom_firmware_version")
    if configured:
        return str(configured).strip()

    candidates = [
        PROJECT_DIR / "include" / "main.h",
        PROJECT_DIR / "include" / "version.h",
        PROJECT_DIR / "src" / "main.cpp",
    ]

    pattern = re.compile(r'#define\s+FIRMWARE_VERSION\s+"([^"]+)"')
    for path in candidates:
        if not path.is_file():
            continue
        try:
            match = pattern.search(path.read_text(encoding="utf-8"))
        except UnicodeDecodeError:
            continue
        if match:
            return match.group(1)

    return None


def parse_upload_command():
    app_bin = Path(env.subst("$BUILD_DIR")) / f"{env.subst('$PROGNAME')}.bin"
    upload_cmd = f"{env.subst('$UPLOADCMD')} {app_bin}"
    chip_match = re.search(r"--chip\s+(\S+)", upload_cmd)
    chip = chip_match.group(1) if chip_match else "esp32"

    pairs = [
        (int(offset, 0), path)
        for offset, path in re.findall(r"(0x[0-9a-fA-F]+)\s+(\S+\.bin)", upload_cmd)
    ]

    if app_bin.is_file():
        app_offset = detect_app_offset()
        pairs = [
            (offset, path)
            for offset, path in pairs
            if Path(path).resolve() != app_bin.resolve()
        ]
        pairs.append((app_offset, str(app_bin)))

    return chip, pairs, upload_cmd


def detect_app_offset():
    value = env.subst("$ESP32_APP_OFFSET")
    if value and value.startswith("0x"):
        return int(value, 0)

    partition_csv = env.subst("$PARTITIONS_TABLE_CSV")
    if partition_csv and Path(partition_csv).is_file():
        for line in Path(partition_csv).read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            fields = [field.strip() for field in line.split(",")]
            if len(fields) >= 4 and fields[0] == "app0" and fields[3].startswith("0x"):
                return int(fields[3], 0)

    return 0x10000


def merge_bins(pairs, out_path):
    if not pairs:
        raise ValueError("No .bin segments were found in PlatformIO upload command")

    print(f"Merging {len(pairs)} firmware segments")

    segments = []
    for offset, path in pairs:
        bin_path = Path(path)
        if not bin_path.is_file():
            raise ValueError(f"Binary segment not found: {bin_path}")
        segments.append((offset, bin_path.read_bytes(), bin_path))

    start_offset = min(offset for offset, _, _ in segments)
    end_offset = max(offset + len(data) for offset, data, _ in segments)
    image = bytearray([0xFF]) * (end_offset - start_offset)

    for offset, data, _ in segments:
        relative = offset - start_offset
        image[relative:relative + len(data)] = data

    out_path = out_path.with_name(f"{out_path.stem}_0x{start_offset:X}{out_path.suffix}")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(image)

    return out_path, start_offset, len(image)


def update_manifest(manifest_path, env_name, firmware_path, address, size, chip, version):
    manifest = {}
    if manifest_path.is_file():
        try:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            print(f"Warning: Replacing invalid manifest JSON: {manifest_path}")

    entry = {
        "id": env_name,
        "file": firmware_path.name,
        "address": f"0x{address:X}",
        "size": size,
        "chip": chip,
    }
    if version:
        entry["version"] = version

    manifest[env_name] = entry
    manifest_path.write_text(json.dumps(manifest, indent=4) + "\n", encoding="utf-8")


def after_build(source, target, env):
    if env.get("__BUILD_FIRMWARE_MERGED"):
        return
    env["__BUILD_FIRMWARE_MERGED"] = True

    env_name = env.subst("$PIOENV")
    version = detect_version()
    output_dir = firmware_output_dir()

    filename_parts = [env_name]
    if version:
        filename_parts.append(version)
    merged_base = output_dir / ("_".join(filename_parts) + ".bin")

    try:
        chip, pairs, upload_cmd = parse_upload_command()
        firmware_path, address, size = merge_bins(pairs, merged_base)
    except ValueError as error:
        print(f"Firmware merge skipped: {error}")
        try:
            print(upload_cmd)
        except UnboundLocalError:
            pass
        return

    if manifest_enabled():
        update_manifest(
            output_dir / DEFAULT_MANIFEST_NAME,
            env_name,
            firmware_path,
            address,
            size,
            chip,
            version,
        )

    version_label = f" {version}" if version else ""
    print(
        f"Wrote merged firmware for {env_name}{version_label}: "
        f"{firmware_path} ({size} bytes at 0x{address:X})"
    )


env.AddPostAction("upload", after_build)
env.AddPostAction("buildprog", after_build)
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", after_build)