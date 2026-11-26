"""Merge PlatformIO ESP32-S3 build outputs into a single 0x0 factory.bin for Web Flasher."""
import argparse
import json
import os
import shutil
import sys


def merge_images(parts, out_path):
    if not parts:
        raise ValueError("No input partitions provided")
    parts = sorted(parts, key=lambda item: item[0])
    total_end = 0
    for offset, path in parts:
        total_end = max(total_end, offset + os.path.getsize(path))

    buf = bytearray(b"\xff" * total_end)
    for offset, path in parts:
        with open(path, "rb") as f:
            data = f.read()
        buf[offset:offset + len(data)] = data
        print(f"  0x{offset:06x}  {len(data):8d} bytes  {os.path.basename(path)}")

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(buf)
    print(f"Wrote {out_path} ({len(buf)} bytes)")


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    default_build = os.path.join(root, ".pio", "build", "esp32s3")
    default_out = os.path.join(default_build, "factory.bin")

    ap = argparse.ArgumentParser(description="Merge ESP32-S3 bootloader, partitions, and firmware into factory.bin")
    ap.add_argument("--build-dir", default=default_build, help="PlatformIO build directory")
    ap.add_argument("--out", default=default_out, help="Output factory.bin path")
    ap.add_argument("--copy-to-flasher", action="store_true",
                    help="Also copy to flasher/public/firmwares/universal-airprint.bin")
    args = ap.parse_args()

    offsets = {
        "bootloader": 0x000000,
        "partition-table": 0x008000,
        "otadata": 0x00f000,
        "app": 0x020000,
    }
    flasher_args = os.path.join(args.build_dir, "flasher_args.json")
    if os.path.isfile(flasher_args):
        with open(flasher_args, "r", encoding="utf-8") as f:
            meta = json.load(f)
        for key in list(offsets):
            off = meta.get(key, {}).get("offset")
            if isinstance(off, str):
                offsets[key] = int(off, 16)

    candidates = [
        (offsets["bootloader"], os.path.join(args.build_dir, "bootloader.bin"), True),
        (offsets["partition-table"], os.path.join(args.build_dir, "partitions.bin"), True),
        (offsets["otadata"], os.path.join(args.build_dir, "ota_data_initial.bin"), False),
        (offsets["app"], os.path.join(args.build_dir, "firmware.bin"), True),
    ]
    parts = []
    for offset, path, required in candidates:
        if os.path.isfile(path):
            parts.append((offset, path))
        elif required:
            print(f"Missing required build artifact: {path}\nRun 'pio run -e esp32s3' first.", file=sys.stderr)
            return 1

    merge_images(parts, args.out)
    if args.copy_to_flasher:
        dst = os.path.join(root, "flasher", "public", "firmwares", "universal-airprint.bin")
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(args.out, dst)
        print(f"Copied to {dst}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
