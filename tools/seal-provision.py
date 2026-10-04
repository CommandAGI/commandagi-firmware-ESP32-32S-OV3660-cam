#!/usr/bin/env python3
# Certify a sealing camera at the factory bench (README § Sealed stream). The steps around CommandAGI's
# factory tools (scripts/integrity/factory-unit.mjs runs them in order, with the lock of a production unit):
#
#   python3 tools/seal-provision.py status /dev/ttyACM0 [--wait 180]   # the `seal-status` line, once the unit answers
#   python3 tools/seal-provision.py pin    /dev/ttyACM0 482913         # a production unit's PIN, stored once
#   python3 tools/seal-provision.py spki   /dev/ttyACM0 > unit.spki.pem
#   node scripts/integrity/device-ca.mjs issue --env dev --spki unit.spki.pem --product CommandAGI-Cam-002 \
#        --serial <serial> --key software --envelope none --lock none > unit.chain.pem
#   python3 tools/seal-provision.py chain  /dev/ttyACM0 unit.chain.pem
#
# `spki` prints the unit's public key (PEM). `chain` stores the certificate chain (PEM, leaf first) on the
# unit; the unit refuses a chain whose leaf does not hold its key. The first seal after the next boot
# announces the chain instead of the bare key. `status` waits for the unit (its first boot under the lock
# encrypts the flash and restarts) and prints what it says of its key and its chip. No command reads the
# private key: the unit has none that does. Needs pyserial (PlatformIO ships it).
import base64, json, re, sys, time

try:
    import serial
except Exception:
    sys.exit("pyserial is missing (pip install pyserial)")


def open_port(port):
    # Open without pulsing EN or GPIO0: the unit keeps running, and its key is already made.
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.5
    s.dtr = False
    s.rts = False
    s.open()
    time.sleep(0.3)
    s.reset_input_buffer()
    return s


def until(s, pattern, seconds=8.0):
    deadline, seen = time.time() + seconds, ""
    while time.time() < deadline:
        seen += s.read(4096).decode("utf-8", "replace")
        m = re.search(pattern, seen, re.S)
        if m:
            return m
    sys.exit(f"no answer from the unit (looked for {pattern!r}); is it a CAGI_DEVICE_SEALS build?")


def status(port, seconds):
    # The native USB port goes away while the unit restarts: open it again until the unit answers.
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            s = open_port(port)
            seen, ask = "", 0.0
            while time.time() < deadline:
                if time.time() >= ask:
                    s.write(b"seal-status\n")
                    ask = time.time() + 2.0
                seen += s.read(4096).decode("utf-8", "replace")
                m = re.search(r"seal-status [^\n]*\n", seen)
                if m:
                    return m.group(0).strip()
        except (serial.SerialException, OSError):
            time.sleep(1.0)
    sys.exit(f"the unit did not answer seal-status within {seconds:.0f} s")


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if len(sys.argv) < 3 or cmd not in ("spki", "chain", "status", "pin") or (cmd in ("chain", "pin") and len(sys.argv) < 4):
        sys.exit("usage: seal-provision.py spki PORT | chain PORT CHAIN.pem | status PORT [--wait SECONDS] | pin PORT PIN")
    port = sys.argv[2]
    if cmd == "status":
        wait = float(sys.argv[sys.argv.index("--wait") + 1]) if "--wait" in sys.argv else 8.0
        print(status(port, wait))
        return
    s = open_port(port)
    if cmd == "spki":
        s.write(b"seal-spki\n")
        print(until(s, r"-----BEGIN PUBLIC KEY-----.*?-----END PUBLIC KEY-----").group(0))
        return
    if cmd == "pin":
        if not re.fullmatch(r"\d{6}", sys.argv[3]):
            sys.exit("the PIN is six digits")
        s.write(("prov-pin " + sys.argv[3] + "\n").encode())
        m = until(s, r"prov-pin (ok|refused)[^\n]*")
        print(m.group(0).strip())
        sys.exit(0 if m.group(1) == "ok" else 1)
    pem = open(sys.argv[3]).read()
    certs = [re.sub(r"\s+", "", b) for b in re.findall(r"-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----", pem, re.S)]
    if not certs:
        sys.exit(f"{sys.argv[3]} holds no certificate")
    for c in certs:
        base64.b64decode(c, validate=True)
    # In small pieces: the unit reads its serial port once per loop (every 200 ms before it has creds),
    # and its receive buffer is 256 bytes.
    line = ("seal-chain " + json.dumps(certs, separators=(",", ":")) + "\n").encode()
    for i in range(0, len(line), 32):
        s.write(line[i:i + 32])
        time.sleep(0.05)
    m = until(s, r"seal-chain (ok|refused)[^\n]*")
    print(m.group(0).strip())
    sys.exit(0 if m.group(1) == "ok" else 1)


main()
