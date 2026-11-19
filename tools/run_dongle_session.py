"""Authorized one-dongle test session, always followed by original restoration.

Requires two verified original snapshots and an audited experimental HEX.
Optional herdsman/radio modes commission a private lab network. Radio mode
opens a bounded pairing window for an explicitly reserved lamp. No Z2M or
production NV is used. Keep the dongle connected throughout.
"""
import argparse
import binascii
import hashlib
import json
from pathlib import Path
import struct
import time
import subprocess
import shutil
from capture_dongle import FLASH_SIZE, load_bsl, read_words, capture as capture_image
from dongle_probe import MT, identify_port
from image import read_hex, check

def sha(data): return hashlib.sha256(data).hexdigest()

def load_radio_nv(previous):
    """Return only NV from our verified experimental snapshot, never original NV."""
    previous = Path(previous).resolve()
    if not previous.is_relative_to((Path(__file__).resolve().parents[1] / "private").resolve()):
        raise ValueError("Experimental radio state must remain private")
    prior = json.loads((previous / "hardware-session.json").read_text())
    saved = (previous / "experimental-final.bin").read_bytes()
    reference = bytearray([255]) * FLASH_SIZE
    reference_hex = previous / "experimental.hex"
    if sha(reference_hex.read_bytes()) != prior["experimental_hex_sha256"]:
        raise ValueError("Snapshot image provenance mismatch")
    for address, byte in read_hex(reference_hex).items(): reference[address] = byte
    if (len(saved) != FLASH_SIZE or sha(saved) != prior["experimental_snapshot_sha256"]
            or prior["production_network_loaded_into_experimental"] or not prior.get("radio", {}).get("adapter_start")
            or saved[:0x50000] != reference[:0x50000] or saved[0x56000:] != reference[0x56000:]
            or saved[0x50000:0x56000] == (previous / "original-a.bin").read_bytes()[0x50000:0x56000]):
        raise ValueError("Refusing unverified or production radio NV")
    return saved[0x50000:0x56000]

def open_rom(port, usb_serial, ieee):
    import serial
    identify_port(port, usb_serial)
    bsl = load_bsl()
    command = bsl.CommandInterface()
    command.sp = serial.serial_for_url(port, do_not_open=True, baudrate=500000, timeout=2, write_timeout=2)
    command.sp.dtr = False; command.sp.rts = False; command.sp.open()
    try:
        command.invoke_bootloader(sonoff_usb=True)
        if not command.sendSynch(): raise IOError("ROM synchronization failed")
        device = bsl.CC26xx(command)
        if device.size != FLASH_SIZE or device.page_size != 8192: raise ValueError("Unexpected target geometry")
        raw = bytes(command.cmdMemReadCC26xx(0x500012f0)) + bytes(command.cmdMemReadCC26xx(0x500012f4))
        if int.from_bytes(raw, "little") != int(ieee, 16): raise ValueError("Target IEEE mismatch")
        return command, device
    except BaseException:
        command.close(); raise

def program_main(command, device, data):
    if len(data) != FLASH_SIZE: raise ValueError("Refusing a partial main-Flash image")
    bl = int.from_bytes(data[0x57fd8:0x57fdc], "little")
    if bl >> 24 != 0xc5 or bl & 255 != 0xc5: raise ValueError("Image would disable ROM recovery")
    if not device.erase(): raise IOError("Main-bank erase failed")
    if not command.writeMemory(0, bytearray(data)): raise IOError("Main-Flash programming failed")
    crc = binascii.crc32(data) & 0xffffffff
    if command.cmdCRC32CC26xx(0, FLASH_SIZE) != crc: raise IOError("Programmed main Flash does not match CRC32")
    return f"{crc:08x}"

def boot(command):
    # Changing SONOFF modem-control levels can itself reset the chip. Issue
    # the ROM reset BEFORE changing them, otherwise its ACK is sought from
    # an application which has already booted and cannot speak the ROM protocol.
    if not command.cmdReset(): raise IOError("ROM reset failed")
    command.sp.dtr = False; command.sp.rts = False

def wait_mt(port, timeout=30, baud=115200):
    mt = MT(port, baud)
    deadline = time.monotonic() + timeout
    try:
        while time.monotonic() < deadline:
            try:
                capability, _ = mt.request(0x21, 1, timeout=1)
                if len(capability) == 2: return mt, capability
            except TimeoutError: pass
        raise TimeoutError("Firmware did not answer SYS_PING within the boot deadline")
    except BaseException:
        mt.close(); raise

def hardware_tests(port, expected_ieee, report, auto_baud=False, baud=115200, expected_revision=20261114):
    mt, capability = wait_mt(port, baud=baud)
    try:
        version, _ = mt.request(0x21, 2)
        ieee, _ = mt.request(0x21, 4)
        if len(version) < 9 or int.from_bytes(version[5:9], "little") != expected_revision:
            raise ValueError("The experimental image is not the expected revision")
        if int.from_bytes(ieee, "little") != int(expected_ieee, 16): raise ValueError("Experimental IEEE mismatch")
        report["experimental_boot"] = True
        # Private scratch system, on the freshly erased experimental NV only.
        key = struct.pack("<BHH", 13, 42, 1)
        value = bytes((i * 7 + 3) & 255 for i in range(64))
        created, _ = mt.request(0x21, 0x30, key + struct.pack("<I", len(value)))
        if created != b"\x00": raise RuntimeError(f"Scratch NV create failed with status {created.hex()}")
        for _ in range(20):
            written, _ = mt.request(0x21, 0x34, key + struct.pack("<HB", 0, len(value)) + value)
            if written != b"\x00": raise RuntimeError("Scratch NV write failed")
            read, _ = mt.request(0x21, 0x33, key + struct.pack("<HB", 0, len(value)))
            if read != bytes([0, len(value)]) + value: raise RuntimeError("Scratch NV read mismatch")
        report["scratch_NV_write_read"] = 20
        # A soft reset checks persisted data and a second real firmware boot.
        from dongle_probe import encode
        mt.serial.write(encode(0x41, 0, b"\x01"))
        mt.close(); time.sleep(1)
        mt, _ = wait_mt(port, baud=baud)
        read, _ = mt.request(0x21, 0x33, key + struct.pack("<HB", 0, len(value)))
        if read != bytes([0, len(value)]) + value: raise RuntimeError("Scratch NV did not survive reset")
        report["scratch_NV_survived_soft_reset"] = True
        latency = []
        for _ in range(300):
            response, ms = mt.request(0x21, 1)
            if response != capability: raise RuntimeError("SYS_PING mismatch")
            latency.append(ms)
        report["ping_300"] = {"min_ms": min(latency), "max_ms": max(latency), "mean_ms": sum(latency)/len(latency)}
        for noise in [b"\xfe" * 300, bytes.fromhex("fe fa 21"), bytes.fromhex("fe 00 21 01 ff")]:
            mt.serial.write(noise)
            response, _ = mt.request(0x21, 1, timeout=1)
            if response != capability: raise RuntimeError("UART framing recovery failed")
        report["UART_noise_truncation_bad_FCS"] = True
        report["auto_baud"] = []
        report["auto_baud_enabled"] = auto_baud
        for baud in ((460800, 1000000, 115200, 1000000, 460800, 115200) if auto_baud else ()):
            mt.serial.baudrate = baud; mt.buffer.clear(); mt.serial.reset_input_buffer()
            started = time.monotonic(); retried = False
            try: response, _ = mt.request(0x21, 1, timeout=0.25)
            except TimeoutError:
                retried = True
                mt.serial.write(b"\xef"); time.sleep(1)
                response, _ = mt.request(0x21, 1, timeout=0.25)
            if response != capability: raise RuntimeError("Autobaud handshake mismatch")
            for _ in range(30):
                response, _ = mt.request(0x21, 1)
                if response != capability: raise RuntimeError("Autobaud ping mismatch")
            report["auto_baud"].append({"baud": baud, "retried": retried, "passed": True, "duration_ms": (time.monotonic()-started)*1000})
        report["serial_and_scratch_NV_tests_passed"] = True
    finally: mt.close()

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", required=True)
    ap.add_argument("--usb-serial", required=True)
    ap.add_argument("--backup-dir", type=Path, required=True)
    ap.add_argument("--experimental", type=Path, required=True)
    ap.add_argument("--test-and-restore", action="store_true", required=True)
    ap.add_argument("--herdsman-lab", action="store_true",
                    help="Form a fresh private lab network with the real ZStackAdapter; never open associations")
    ap.add_argument("--radio-peer", action="store_true",
                    help="Qualify radio with an explicitly reserved HG06106C; open pairing for at most 180s")
    ap.add_argument("--radio-resume-session", type=Path,
                    help="Reuse ONLY a verified private experimental radio snapshot, never a production backup")
    ap.add_argument("--transport-benchmark", action="store_true", help="Benchmark standard SYS/NV commands on fresh lab NV")
    ap.add_argument("--radio-load-test", action="store_true", help="Exercise concurrent radio reads and changed lab NV writes")
    ap.add_argument("--benchmark-samples", type=int, default=40)
    ap.add_argument("--benchmark-rounds", type=int, default=3)
    args = ap.parse_args()
    if args.radio_resume_session and not args.radio_peer: raise ValueError("Radio resume requires radio mode")
    if args.radio_load_test and not args.radio_peer: raise ValueError("Radio load test requires radio mode")
    if args.transport_benchmark and (args.radio_peer or args.herdsman_lab):
        raise ValueError("Benchmark requires a fresh isolated NV without commissioning")
    if not 10 <= args.benchmark_samples <= 200 or not 2 <= args.benchmark_rounds <= 10:
        raise ValueError("Benchmark samples must be 10..200 and rounds 2..10")
    harness = Path(__file__).with_name("herdsman_lab.cjs")
    node = shutil.which("node")
    if args.herdsman_lab or args.radio_peer:
        if not node: raise RuntimeError("Node.js is required for the real herdsman test")
        # This dependency/native-driver check opens no serial port. Fail BEFORE
        # capturing, erasing or touching the authorized hardware.
        subprocess.run([node, str(harness), "--check-runtime"], check=True, capture_output=True, timeout=20)
    radio_harness = Path(__file__).with_name("herdsman_radio.cjs")
    if args.radio_peer:
        if args.herdsman_lab: raise ValueError("Use local herdsman or radio qualification, not both in one fresh NV")
        subprocess.run([node, str(radio_harness), "--check-runtime"], check=True, capture_output=True, timeout=20)
    folder = args.backup_dir.resolve()
    original = (folder / "original-a.bin").read_bytes()
    capture = json.loads((folder / "capture.json").read_text())
    capture.pop("left_in_ROM", None)
    if not capture["passed"] or sha(original) != capture["flash_sha256"] or original != (folder / "original-b.bin").read_bytes():
        raise ValueError("Original snapshots/manifest are inconsistent")
    if args.usb_serial != capture["usb_serial"]: raise ValueError("Backup belongs to a different USB dongle")
    memory = read_hex(args.experimental)
    if not check(memory)["passed"]: raise ValueError("Experimental recovery audit failed")
    manifest = json.loads(args.experimental.with_name("manifest.json").read_text())
    expected_revision = manifest["znp_version"]["revision"]
    baud = manifest.get("baudrate", 115200)
    if baud not in (115200, 460800, 1000000): raise ValueError("Unsupported fixed benchmark rate")
    if manifest["hex_sha256"] != sha(args.experimental.read_bytes()): raise ValueError("Experimental HEX SHA mismatch")
    data = bytearray([255]) * FLASH_SIZE
    for address, byte in memory.items(): data[address] = byte
    if args.radio_resume_session:
        previous = args.radio_resume_session.resolve()
        data[0x50000:0x56000] = load_radio_nv(previous)
    report = {"hardware_tested": True, "Z2M_used": False, "radio_exchange_tested": False,
              "production_network_loaded_into_experimental": False, "original_sha256": sha(original),
              "experimental_hex_sha256": manifest["hex_sha256"], "restore_verified": False}
    report["baudrate"] = baud
    command = None
    touched = False
    test_error = None
    try:
        command, device = open_rom(args.port, args.usb_serial, capture["ieee"])
        # Closing/opening this USB bridge can reboot the application and
        # advance its durable counters. Freeze a fresh baseline in the SAME
        # ROM connection as the erase, retaining the initial snapshots too.
        for number in range(1, 1000):
            live_folder = folder / f"session-{number:03d}"
            if not live_folder.exists(): live_folder.mkdir(); break
        else: raise ValueError("No unused session backup directory")
        print("Capturing the immediate rollback baseline twice, without reconnecting", flush=True)
        first = live_folder / "original-a.bin"; second = live_folder / "original-b.bin"
        crc_a = capture_image(command, first); crc_b = capture_image(command, second)
        fresh = first.read_bytes()
        if fresh != second.read_bytes() or crc_a != crc_b: raise ValueError("Immediate snapshots differ")
        if fresh[:0x50000] != original[:0x50000] or fresh[0x56000:] != original[0x56000:]:
            raise ValueError("Original firmware/CCFG changed, not just NV; refusing erase")
        report["initial_backup_sha256"] = sha(original)
        report["initial_to_immediate_changed_bytes"] = sum(a != b for a,b in zip(fresh, original))
        report["immediate_original_sha256"] = sha(fresh)
        report["immediate_captures_identical_and_ROM_CRC_verified"] = True
        report["rollback_directory"] = str(live_folder)
        original = fresh
        shutil.copyfile(args.experimental, live_folder / "experimental.hex")
        (live_folder / "capture.json").write_text(json.dumps(dict(capture, flash_sha256=sha(fresh),
            nv_sha256=sha(fresh[0x50000:0x56000]), crc32=f"{crc_a:08x}",
            ROM_at_last_command=True, serial_close_may_reboot_application=True), indent=2) + "\n")
        touched = True
        print("Programming experimental image with production NV excluded", flush=True)
        report["experimental_program_crc32"] = program_main(command, device, data)
        boot(command); command.close(); command = None
        if args.radio_resume_session or args.transport_benchmark:
            mt, _ = wait_mt(args.port, baud=baud)
            try:
                version, _ = mt.request(0x21, 2)
                if int.from_bytes(version[5:9], "little") != expected_revision: raise ValueError("Resumed image revision mismatch")
            finally: mt.close()
            report["experimental_boot"] = True
            if args.radio_resume_session: report["experimental_radio_NV_resumed"] = True
        else:
            hardware_tests(args.port, capture["ieee"], report, auto_baud=manifest.get("auto_baud", False),
                           baud=baud, expected_revision=expected_revision)
        if args.transport_benchmark:
            from transport_benchmark import measure
            report["performance"] = measure(args.port, baud, expected_revision, capture["ieee"],
                live_folder / "performance-raw.csv", samples=args.benchmark_samples, rounds=args.benchmark_rounds)
            if manifest.get("optimized"):
                hardware_tests(args.port, capture["ieee"], report, auto_baud=False,
                               baud=baud, expected_revision=expected_revision)
        if args.herdsman_lab:
            state_dir = live_folder / "herdsman"
            state_dir.mkdir()
            print("Testing real ZStackAdapter on a private fresh network (associations closed)", flush=True)
            try:
                with (state_dir / "process-private.log").open("wb") as output:
                    child = subprocess.run([node, str(harness), "--port", args.port,
                        "--expect-ieee", capture["ieee"], "--state-dir", str(state_dir)],
                        stdout=output, stderr=subprocess.STDOUT, timeout=200)
            finally:
                result_file = state_dir / "result.json"
                if result_file.exists(): report["herdsman"] = json.loads(result_file.read_text())
            if child.returncode or not report.get("herdsman", {}).get("passed"):
                raise RuntimeError("Real herdsman qualification failed; private diagnostics retained")
        if args.radio_peer:
            state_dir = live_folder / "radio"
            state_dir.mkdir()
            print("Starting private radio qualification; pairing readiness is recorded in radio/result.json", flush=True)
            try:
                with (state_dir / "process-private.log").open("wb") as output:
                    child_args = [node, str(radio_harness), "--port", args.port,
                        "--expect-ieee", capture["ieee"], "--state-dir", str(state_dir),
                        "--expected-revision", str(expected_revision), "--baudrate", str(baud)]
                    if args.radio_resume_session: child_args += ["--resume-dir", str(previous / "radio")]
                    if args.radio_load_test: child_args += ["--stress-writes", "80"]
                    child = subprocess.run(child_args,
                        stdout=output, stderr=subprocess.STDOUT, timeout=600)
            finally:
                result_file = state_dir / "result.json"
                if result_file.exists(): report["radio"] = json.loads(result_file.read_text())
            if child.returncode or not report.get("radio", {}).get("passed"):
                raise RuntimeError("Radio qualification failed; private diagnostics retained")
            report["radio_exchange_tested"] = True
    except Exception as exc:
        test_error = str(exc); report["test_error"] = test_error
        print("Test session result:", test_error, flush=True)
    finally:
        if command: command.close(); command = None
        try:
            if touched:
                print("Restoring original full Flash and production NV", flush=True)
                command, device = open_rom(args.port, args.usb_serial, capture["ieee"])
                try:
                    if args.radio_peer:
                        try:
                            capture_image(command, live_folder / "experimental-final.bin")
                            report["experimental_snapshot_sha256"] = sha((live_folder / "experimental-final.bin").read_bytes())
                        except Exception as exc:
                            report["experimental_snapshot_error"] = str(exc)
                            # Snapshot failure must never prevent rollback.
                    report["restore_crc32"] = program_main(command, device, original)
                    readback = bytearray()
                    for address in range(0, FLASH_SIZE, 252):
                        readback.extend(read_words(command, address, min(63, (FLASH_SIZE-address)//4)))
                    if readback != original: raise IOError("Original restoration readback differs")
                    (live_folder / "restore-readback.bin").write_bytes(readback)
                    report["restore_verified"] = True
                    report["restore_readback_sha256"] = sha(readback)
                    boot(command)
                finally: command.close()
        except Exception as exc:
            report["restore_error"] = str(exc)
            raise
        finally:
            encoded = json.dumps(report, indent=2) + "\n"
            (folder / "hardware-session.json").write_text(encoded, encoding="utf-8")
            if "live_folder" in locals(): (live_folder / "hardware-session.json").write_text(encoded, encoding="utf-8")
    print(json.dumps(report, indent=2), flush=True)
    if test_error: raise SystemExit(1)

if __name__ == "__main__": main()
