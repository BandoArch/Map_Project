"""Install a checksummed OSM file over USB, retaining /map.osm.bak on the SD."""

import argparse
from pathlib import Path
import time
import xml.etree.ElementTree as ET
import zlib

import serial


def wait_line(port, prefix, timeout=30):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        line = port.readline().decode("utf-8", errors="replace").strip()
        if not line:
            continue
        if "ERROR" in line:
            raise RuntimeError(line)
        if line.startswith(prefix):
            return line
        print(line, flush=True)
    raise TimeoutError(f"No {prefix!r} response from board")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("file", type=Path)
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--satellite", action="store_true", help="Upload a 512x512 RGB565 satellite image")
    args = parser.parse_args()
    data = args.file.read_bytes()
    if args.satellite:
        if len(data) != 8 + 512 * 512 * 2 or data[:8] != b"T565\x00\x02\x00\x02":
            raise ValueError("Expected a 512x512 T565 satellite image")
    else:
        root = ET.fromstring(data)
        if root.tag != "osm" or not root.findall("node") or not root.findall("way"):
            raise ValueError("Expected an OSM XML map containing nodes and ways")
    checksum = zlib.crc32(data)
    with serial.Serial(args.port, 115200, timeout=0.5, write_timeout=10) as port:
        port.reset_input_buffer()
        command = "SATPUT" if args.satellite else "OSMPUT"
        port.write(f"\n{command} {len(data)} {checksum:08x}\n".encode())
        wait_line(port, "TILE READY")
        result = None
        for sequence, offset in enumerate(range(0, len(data), 128)):
            chunk = data[offset:offset + 128]
            expected = f"TILEACK {sequence} {offset + len(chunk)}"
            final = offset + len(chunk) == len(data)
            acknowledged = False
            for attempt in range(4):
                port.write(f"TILECHUNK {sequence} {len(chunk)} {zlib.crc32(chunk):08x}\n".encode())
                port.flush()
                time.sleep(0.002)
                port.write(chunk)
                port.flush()
                deadline = time.monotonic() + (60 if final else 3)
                while time.monotonic() < deadline:
                    line = port.readline().decode("utf-8", errors="replace").strip()
                    if "ERROR" in line:
                        raise RuntimeError(line)
                    if line == expected:
                        acknowledged = True
                        break
                    if final and line.startswith("TILE OK"):
                        result = line
                        acknowledged = True
                        break
                if acknowledged:
                    break
                if final:
                    raise TimeoutError("No final acknowledgement")
                print(f"Retrying frame {sequence}", flush=True)
            if not acknowledged:
                raise TimeoutError(f"No acknowledgement for frame {sequence}")
            if sequence % 512 == 0:
                print(f"Uploaded {offset + len(chunk)}/{len(data)} bytes", flush=True)
        if result is None:
            result = wait_line(port, "TILE OK", timeout=60)
        if result != f"TILE OK 15 {checksum:08x}":
            raise RuntimeError("Final checksum mismatch")
        print(f"SD readback checksum verified: {checksum:08x}", flush=True)
        wait_line(port, "MAP DISPLAYED /satellite.t565" if args.satellite else "MAP DISPLAYED /map.osm", timeout=60)
        print("Map rendered on the board", flush=True)


if __name__ == "__main__":
    main()
