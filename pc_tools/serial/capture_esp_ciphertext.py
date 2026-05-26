#!/usr/bin/env python3
import argparse
import os
import select
import subprocess
import sys
import termios
import time
from pathlib import Path


BAUD_MAP = {
    9600: termios.B9600,
    19200: termios.B19200,
    38400: termios.B38400,
    57600: termios.B57600,
    115200: termios.B115200,
    230400: termios.B230400,
}


def configure_serial(fd: int, baud: int) -> None:
    if baud not in BAUD_MAP:
        raise ValueError(f"unsupported baud rate: {baud}")

    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[4] = BAUD_MAP[baud]
    attrs[5] = BAUD_MAP[baud]
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)


def read_lines_until_cipher(fd: int, timeout_s: float, show_hex: bool) -> tuple[bytes, list[str]]:
    deadline = time.time() + timeout_s
    buffer = b""
    in_hex = False
    in_encrypt_report = False
    ciphertext: bytes | None = None
    hex_parts: list[str] = []
    log_lines: list[str] = []

    while time.time() < deadline:
        remaining = max(0.0, deadline - time.time())
        ready, _, _ = select.select([fd], [], [], min(0.2, remaining))
        if not ready:
            continue

        chunk = os.read(fd, 4096)
        if not chunk:
            continue
        buffer += chunk

        while b"\n" in buffer:
            raw_line, buffer = buffer.split(b"\n", 1)
            line = raw_line.decode("utf-8", errors="replace").strip()
            if not line:
                continue
            log_lines.append(line)

            if line == "BEGIN_ENCRYPT_REPORT":
                in_encrypt_report = True
                print(line)
                continue
            if line == "END_ENCRYPT_REPORT" and in_encrypt_report:
                print(line)
                if ciphertext is not None:
                    return ciphertext, log_lines
                continue
            if line == "BEGIN_CIPHERTEXT_HEX":
                in_hex = True
                hex_parts.clear()
                print(line)
                continue
            if line == "END_CIPHERTEXT_HEX" and in_hex:
                print(line)
                hex_text = "".join(hex_parts)
                ciphertext = bytes.fromhex(hex_text)
                in_hex = False
                if not in_encrypt_report:
                    return ciphertext, log_lines
                continue
            if in_hex:
                if show_hex:
                    print(line)
                hex_parts.append(line)
            else:
                print(line)

    raise TimeoutError("timed out waiting for END_CIPHERTEXT_HEX")


def read_lines_until_marker(fd: int, timeout_s: float, end_marker: str) -> list[str]:
    deadline = time.time() + timeout_s
    buffer = b""
    log_lines: list[str] = []

    while time.time() < deadline:
        remaining = max(0.0, deadline - time.time())
        ready, _, _ = select.select([fd], [], [], min(0.2, remaining))
        if not ready:
            continue

        chunk = os.read(fd, 4096)
        if not chunk:
            continue
        buffer += chunk

        while b"\n" in buffer:
            raw_line, buffer = buffer.split(b"\n", 1)
            line = raw_line.decode("utf-8", errors="replace").strip()
            if not line:
                continue
            log_lines.append(line)
            print(line)
            if line == end_marker:
                return log_lines

    raise TimeoutError(f"timed out waiting for {end_marker}")


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(line_buffering=True)

    parser = argparse.ArgumentParser(description="Capture ESP32 mini CKKS ciphertext over serial")
    parser.add_argument("--port", default="/dev/cu.usbserial-10")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--value", type=float, default=1.25)
    parser.add_argument("--out", type=Path, default=Path("pc_tools/test_vectors/cipher_from_esp.bin"))
    parser.add_argument("--timeout", type=float, default=90.0)
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--show-hex", action="store_true")
    parser.add_argument("--bench-runs", type=int, default=0, help="send BENCH instead of ENCRYPT when > 0")
    parser.add_argument("--report", type=Path, default=None, help="optional file to save serial report lines")
    parser.add_argument("--bundle", type=Path, default=Path("pc_tools/test_vectors/bundle.bin"))
    parser.add_argument("--secret", type=Path, default=Path("pc_tools/test_vectors/secret.bin"))
    parser.add_argument("--he-tool", type=Path, default=Path("pc_tools/build/he_tool"))
    args = parser.parse_args()

    fd = os.open(args.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        configure_serial(fd, args.baud)
        time.sleep(1.2)

        if args.bench_runs > 0:
            command = f"BENCH {args.value:.12g} {args.bench_runs}\n".encode("ascii")
            os.write(fd, command)
            lines = read_lines_until_marker(fd, args.timeout, "END_BENCH_REPORT")
            if args.report:
                args.report.parent.mkdir(parents=True, exist_ok=True)
                args.report.write_text("\n".join(lines) + "\n", encoding="utf-8")
                print(f"saved_report={args.report}")
            return 0

        command = f"ENCRYPT {args.value:.12g}\n".encode("ascii")
        os.write(fd, command)

        ciphertext, lines = read_lines_until_cipher(fd, args.timeout, args.show_hex)
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_bytes(ciphertext)
        print(f"saved_ciphertext={args.out} bytes={len(ciphertext)}")
        if args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text("\n".join(lines) + "\n", encoding="utf-8")
            print(f"saved_report={args.report}")

        if args.verify:
            cmd = [
                str(args.he_tool),
                "decrypt-check",
                "--bundle",
                str(args.bundle),
                "--secret",
                str(args.secret),
                "--cipher",
                str(args.out),
                "--expected",
                str(args.value),
                "--max-abs-err",
                "0.2",
                "--compute-a",
                "1",
                "--compute-b",
                "2",
                "--compute-c",
                "3",
            ]
            print("running_verify=" + " ".join(cmd))
            subprocess.run(cmd, check=True)

        return 0
    finally:
        os.close(fd)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2)
