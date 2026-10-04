#!/usr/bin/env python3
# Certify a sealing camera at the factory bench (README § Sealed stream). Two steps around CommandAGI's
# factory tool (scripts/integrity/device-ca.mjs in the CommandAGI repository):
#
#   python3 tools/seal-provision.py spki  /dev/ttyACM0 > unit.spki.pem
#   node scripts/integrity/device-ca.mjs issue --env dev --spki unit.spki.pem --product CommandAGI-Cam-002 \
#        --serial <serial> --key software --envelope none > unit.chain.pem
#   python3 tools/seal-provision.py chain /dev/ttyACM0 unit.chain.pem
#
# `spki` prints the unit's public key (PEM). `chain` stores the certificate chain (PEM, leaf first) on the
# unit; the unit refuses a chain whose leaf does not hold its key. The first seal after the next boot
# announces the chain instead of the bare key. Needs pyserial (PlatformIO ships it).
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


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("spki", "chain") or (sys.argv[1] == "chain" and len(sys.argv) < 4):
        sys.exit("usage: seal-provision.py spki PORT | chain PORT CHAIN.pem")
    s = open_port(sys.argv[2])
    if sys.argv[1] == "spki":
        s.write(b"seal-spki\n")
        print(until(s, r"-----BEGIN PUBLIC KEY-----.*?-----END PUBLIC KEY-----").group(0))
        return
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
