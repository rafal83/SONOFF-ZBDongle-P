"""Read-only MT identity/ping checks on one explicitly identified USB dongle.

No NV command, reset command, commissioning or flash operation is provided.
Opening a USB serial driver can nevertheless affect its modem-control lines.
"""
import argparse
import json
from pathlib import Path
import time

def encode(cmd0, cmd1, payload=b""):
    if len(payload) > 250: raise ValueError("MT payload too large")
    data = bytes([len(payload), cmd0, cmd1]) + payload
    fcs = 0
    for byte in data: fcs ^= byte
    return b"\xfe" + data + bytes([fcs])

def extract(buffer):
    frames = []
    while buffer:
        if buffer[0] != 0xfe or len(buffer) >= 2 and buffer[1] > 250:
            del buffer[0]; continue
        if len(buffer) < 2 or len(buffer) < buffer[1] + 5: break
        size = buffer[1] + 5
        fcs = 0
        for byte in buffer[1:size - 1]: fcs ^= byte
        if fcs != buffer[size - 1]: del buffer[0]; continue
        frames.append((buffer[2], buffer[3], bytes(buffer[4:size - 1])))
        del buffer[:size]
    return frames

def identify_port(port, serial_number):
    if port.lower().startswith("rfc2217://"):
        # Dongle on a remote host (ser2net, RFC 2217: DTR/RTS and baud rate are
        # forwarded, so the ROM bootloader can be entered). The USB identity
        # cannot be read from here: the callers verify the IEEE address instead
        # (from the ROM before any write, by MT afterwards).
        return {"port": port, "usb_serial": serial_number, "remote": True,
                "identity": "verified by IEEE (ROM / MT), not by USB"}
    from serial.tools import list_ports
    matches = [p for p in list_ports.comports() if p.device.upper() == port.upper()]
    if len(matches) != 1: raise ValueError("Explicit serial port is not uniquely present")
    item = matches[0]
    if (item.vid, item.pid) != (0x10c4, 0xea60) or item.serial_number != serial_number:
        raise ValueError("USB identity mismatch; refusing to open the port")
    return {"port": item.device, "usb_serial": item.serial_number, "vid": item.vid, "pid": item.pid}

class MT:
    def __init__(self, port, baud):
        import serial
        extra = {} if port.lower().startswith("rfc2217://") else {"write_timeout": 1}   # unsupported by RFC 2217
        self.serial = serial.serial_for_url(port, do_not_open=True, baudrate=baud, timeout=0.05, **extra)
        self.serial.dtr = False
        self.serial.rts = False
        self.serial.open()
        self.buffer = bytearray()

    def request(self, cmd0, cmd1, payload=b"", timeout=2):
        self.serial.write(encode(cmd0, cmd1, payload))
        start = time.monotonic()
        while time.monotonic() - start < timeout:
            self.buffer.extend(self.serial.read(max(1, min(self.serial.in_waiting, 1024))))
            for response0, response1, response in extract(self.buffer):
                if (response0, response1) == ((cmd0 & 31) | 0x60, cmd1):
                    return response, (time.monotonic() - start) * 1000
                if response0 == 0x60 and len(response) >= 3 and response[1:] == bytes([cmd0, cmd1]):
                    raise RuntimeError("MT command rejected")
        raise TimeoutError(f"No MT response for {cmd0:02x}/{cmd1:02x}")

    def close(self): self.serial.close()

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", required=True)
    ap.add_argument("--usb-serial", required=True)
    ap.add_argument("--baud", type=int, choices=[115200, 460800, 1000000], default=115200)
    ap.add_argument("--expect-ieee", required=True)
    ap.add_argument("--report", type=Path, required=True)
    ap.add_argument("--pings", type=int, default=10)
    args = ap.parse_args()
    if not 1 <= args.pings <= 1000: ap.error("pings must be 1..1000")
    report = identify_port(args.port, args.usb_serial)
    mt = MT(args.port, args.baud)
    try:
        capability, _ = mt.request(0x21, 1)
        version, _ = mt.request(0x21, 2)
        ieee, _ = mt.request(0x21, 4)
        if len(capability) != 2 or len(version) not in (9, 10) or len(ieee) != 8:
            raise ValueError("Unexpected identity response layout")
        actual_ieee = "0x" + ieee[::-1].hex()
        if int(actual_ieee, 16) != int(args.expect_ieee, 16):
            raise ValueError("Coordinator IEEE mismatch; refusing further checks")
        latencies = []
        for _ in range(args.pings):
            response, ms = mt.request(0x21, 1)
            if response != capability: raise ValueError("Inconsistent SYS_PING capabilities")
            latencies.append(ms)
        report.update(passed=True, baud=args.baud, ieee=actual_ieee,
            version=dict(zip(["transportrev", "product", "major", "minor", "maintenance"], version[:5])),
            revision=int.from_bytes(version[5:9], "little"), version_extension_hex=version[9:].hex(), capabilities=int.from_bytes(capability, "little"),
            pings=len(latencies), ping_latency_ms={"min": min(latencies), "max": max(latencies), "mean": sum(latencies)/len(latencies)},
            nv_commands_sent=False, reset_commands_sent=False, commissioning_tested=False, radio_exchange_tested=False)
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(report, indent=2))
    finally: mt.close()

if __name__ == "__main__": main()
