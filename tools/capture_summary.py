"""Read only aggregate reports from the serial port; discard identifier lines."""

import argparse
import re
import time

import serial


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--target-seconds", type=int, default=300)
    parser.add_argument("--timeout-seconds", type=int, default=390)
    args = parser.parse_args()

    deadline = time.monotonic() + args.timeout_seconds
    in_summary = False
    report_seconds = 0
    with serial.Serial(args.port, args.baud, timeout=1) as device:
        while time.monotonic() < deadline:
            line = device.readline().decode("utf-8", errors="replace").strip()
            match = re.search(r"=== SUMMARY elapsed_s=(\d+)", line)
            if match:
                in_summary = True
                report_seconds = int(match.group(1))
            if in_summary:
                print(line, flush=True)
            if "=== END SUMMARY ===" in line:
                in_summary = False
                if report_seconds >= args.target_seconds:
                    return
    raise TimeoutError("No complete summary reached the requested elapsed time")


if __name__ == "__main__":
    main()
