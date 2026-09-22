#!/usr/bin/env python3
# Read the BLE advertised-name suffix ("CommandAGI Cam XXXX") off a freshly-flashed ESP32-CAM over
# serial, and print just the 4-hex suffix to stdout (nothing if not seen). Used by batch-flash.mjs to
# label each unit so the printed label matches what the app shows during BLE pairing.
#
# Why a separate pyserial reader (not `pio device monitor`): miniterm needs an interactive TTY and
# crashes (Console()) when run from a script / background, so it never reads a byte. pyserial works
# headless. We also pulse EN (via RTS, DTR high = GPIO0 high) on open to reboot into the app, so the
# once-per-boot advertising line is reliably reprinted within our read window.
#
# Usage: python3 read-suffix.py /dev/ttyUSB0 [timeout_seconds]
import sys, time, re

try:
    import serial  # pyserial — ships as a PlatformIO dependency
except Exception:
    sys.exit(2)  # no pyserial → caller records PIN only

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"
timeout_s = float(sys.argv[2]) if len(sys.argv) > 2 else 12.0
pat = re.compile(r"CommandAGI Cam ([0-9A-Fa-f]{4})")

try:
    s = serial.Serial(port, 115200, timeout=0.5)
except Exception as e:
    print(f"open failed: {e}", file=sys.stderr)
    sys.exit(3)

# ESP32 auto-reset pulse: DTR=False keeps GPIO0 high (boot the app, not the ROM loader); toggle RTS
# (EN) low→high to reset. Harmless if the board lacks the auto-reset caps — we just read what's there.
try:
    s.setDTR(False)
    s.setRTS(True)
    time.sleep(0.1)
    s.setRTS(False)
except Exception:
    pass  # some adapters don't expose the control lines

deadline = time.time() + timeout_s
found = None
while time.time() < deadline:
    try:
        line = s.readline().decode("utf-8", "replace").strip()
    except Exception:
        break
    if not line:
        continue
    print(line, file=sys.stderr)  # surface the boot log for live debugging, off stdout
    m = pat.search(line)
    if m:
        found = m.group(1).upper()
        break
s.close()

if found:
    print(found)  # ONLY the suffix on stdout
    sys.exit(0)
sys.exit(1)
