"""One-dongle FreeRTOS diagnostic session, ALWAYS followed by restoration.

Sequence (single run, restoration in a finally block):
  1. identify the dongle (USB serial + factory IEEE read through the ROM BSL);
  2. two full Flash captures, required identical (backup kept in --session-dir);
  3. program ONLY 0x00000-0x4FFFF with the FreeRTOS diagnostic image; NV
     (0x50000-0x55FFF), spare page and CCFG are neither erased nor written;
     full read-back verification;
  4. boot and run UART/MT diagnostics (no Flash/NV command exists in the image);
     optional PASSIVE radio reception on --sniff-channel (no transmission);
  5. ROM again: capture, prove NV and CCFG are byte-identical to step 2;
  6. reprogram the original application region from the step-2 capture, full
     read-back must equal the original capture byte for byte;
  7. boot and check the Z-Stack answers SYS_PING / SYS_VERSION at 1 Mbaud
     with the expected revision and IEEE.

Zigbee2MQTT must be stopped and the dongle connected to this PC.
Nothing here commissions, joins, writes NV or transmits on the radio.

  python tools/freertos_lab_session.py --self-test     # offline checks only
  python tools/freertos_lab_session.py --port COM8 --usb-serial ... --expect-ieee 0x... \
      --session-dir private/freertos-lab-YYYYMMDD --z2m-stopped [--sniff-channel N]
  python tools/freertos_lab_session.py restore --port ... (manual recovery from a session)
"""
import argparse
import json
from pathlib import Path
import struct
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from image import read_hex, check  # noqa: E402
from capture_dongle import FLASH_SIZE  # noqa: E402

NV_START, NV_END, CCFG = 0x50000, 0x56000, 0x57FA8
FREERTOS_HEX = ROOT / "build/freertos/diag-115200/sonoff-freertos-diag.hex"


def freertos_hex(baud, hw_aes=False):
    return ROOT / f"build/freertos/diag-{baud}{'-hwaes' if hw_aes else ''}/sonoff-freertos-diag.hex"
INSTALLED_REVISION = 2026111802
INSTALLED_BAUD = 1000000


def sha(data):
    import hashlib
    return hashlib.sha256(data).hexdigest()


def compose_freertos(current, hex_memory):
    """Full image = FreeRTOS application + CURRENT NV, spare page and CCFG."""
    if len(current) != FLASH_SIZE:
        raise ValueError("Expected a complete CC2652P capture")
    outside = [a for a in hex_memory if a >= 0x34000 and not CCFG <= a < FLASH_SIZE]   # store (layout 2), NV
    if outside:
        raise ValueError(f"FreeRTOS HEX writes outside the application/CCFG: 0x{outside[0]:05x}")
    for a in range(CCFG, FLASH_SIZE):
        if a in hex_memory and hex_memory[a] != current[a]:
            raise ValueError("FreeRTOS CCFG differs from the dongle CCFG; refusing")
    image = bytearray(current)
    image[:NV_START] = b"\xff" * NV_START
    for a, b in hex_memory.items():
        if a < NV_START:
            image[a] = b
    sparse = {i: b for i, b in enumerate(image) if not NV_START <= i < NV_END}
    result = check(sparse)
    if not result["passed"]:
        raise ValueError("Recovery configuration check failed on the composed image")
    if image[NV_START:] != current[NV_START:]:
        raise AssertionError("NV/CCFG changed during composition")
    return bytes(image)


# ---------------------------------------------------------------- MT helpers
def read_frames(mt, seconds):
    from dongle_probe import extract
    frames = []
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        mt.buffer.extend(mt.serial.read(max(1, min(mt.serial.in_waiting, 1024))))
        frames.extend(extract(mt.buffer))
    return frames


def decode_info(p):
    if len(p) < 74:
        raise ValueError(f"DIAG_INFO too short ({len(p)})")
    f = struct.unpack_from("<BBBBIIBBIIII16sIHHBIIIBBBI", p)
    return {"format": f[0], "version": f"{f[1]}.{f[2]}.{f[3]}", "build": f[4], "uptime_ms": f[5],
            "reset_source": f[6], "previous_crash_kind": f[7], "crash_pc": hex(f[8]), "crash_lr": hex(f[9]),
            "crash_detail": f[10], "crash_cfsr": hex(f[11]), "crash_where": f[12].rstrip(b"\0").decode(errors="replace"),
            "boots_with_valid_record": f[13], "cpu_load_permille": f[14], "cpu_peak_permille": f[15],
            "tasks": f[16], "init_status": f[17], "sram_static": f[18], "flash_used": f[19],
            "freertos": f"{f[20]}.{f[21]}.{f[22]}", "uart_baud": f[23],
            **({"source_commit": f"{struct.unpack_from('<I', p, 74)[0]:08x}", "source_modified": bool(p[78])}
               if len(p) >= 79 else {})}


COUNTER_NAMES = ["uart_rx_bytes", "uart_tx_bytes", "uart_rx_overflow", "uart_hw_overrun", "uart_framing",
                 "uart_parity", "uart_break", "uart_isr_count", "uart_isr_max_cycles", "uart_rx_high_water",
                 "uart_tx_high_water", "mt_accepted", "mt_bad_fcs", "mt_oversized", "mt_timeouts",
                 "mt_invalid_type", "mt_resync_discarded", "mt_rx_errors", "tx_dropped", "tx_dropped_normal",
                 "tx_dropped_control", "tx_high_water", "wd_feeds", "wd_misses", "wd_last_missing",
                 "wd_worst_consecutive", "radio_frames_dropped"]
RADIO_NAMES = ["powerups", "setup_failures", "last_error_step", "rx_frames", "rx_crc_fail", "rx_ignored",
               "rx_buf_full", "rx_overflow_restarts", "rx_dropped_parse", "tx_attempts", "tx_ok", "tx_no_ack",
               "tx_busy", "tx_error", "tx_ack_pending", "internal_errors", "synth_no_lock", "src_match_overflow",
               "isr_count", "isr_max_cycles", "last_cmd_status", "last_rssi", "max_rssi", "channel", "power_dbm",
               "high_pa", "powered", "setup_fail_step", "setup_fail_status", "setup_fail_fs_status"]


def decode_counters(p, names):
    n = p[0]
    values = struct.unpack_from(f"<{n}I", p, 1)
    out = dict(zip(names, values))
    for k in ("last_rssi", "max_rssi", "power_dbm"):
        if k in out:
            out[k] = struct.unpack("<i", struct.pack("<I", out[k]))[0]
    return out


def capture_boot(serial_port, report, seconds=6.0):
    """Read the UART right after the ROM reset, on the SAME open handle, so
    the polled boot milestones (0x5F 0x81) and crash records (0x5F 0x82)
    emitted within milliseconds of reset are not lost."""
    from dongle_probe import extract
    serial_port.baudrate = 115200
    raw = bytearray()
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        raw.extend(serial_port.read(max(1, min(serial_port.in_waiting, 1024))))
    frames = extract(bytearray(raw))
    boot = {"raw_bytes": len(raw), "raw_head_hex": bytes(raw[:512]).hex(), "frames": []}
    for cmd0, cmd1, p in frames[:64]:
        item = {"cmd": f"{cmd0:02x}{cmd1:02x}", "payload": p.hex()}
        if (cmd0, cmd1) == (0x5F, 0x81) and len(p) >= 7:
            item["milestone"] = {"stage": p[0], "init_status": int.from_bytes(p[1:5], "little"),
                                 "reset_source": p[5], "previous_crash_kind": p[6]}
        if (cmd0, cmd1) == (0x5F, 0x82) and len(p) >= 45:
            f = struct.unpack_from("<B7I16s", p)
            item["crash"] = {"kind": f[0], "pc": hex(f[1]), "lr": hex(f[2]), "psr": hex(f[3]),
                             "cfsr": hex(f[4]), "hfsr": hex(f[5]), "bfar": hex(f[6]), "detail": f[7],
                             "where": f[8].rstrip(b"\0").decode(errors="replace")}
        if (cmd0, cmd1) == (0x41, 0x80):
            item["reset_ind"] = True
        boot["frames"].append(item)
    boot["frame_count"] = len(frames)
    report.setdefault("freertos", {})["boot_capture"] = boot


COORD_NAMES = ["tx_frames", "tx_retries", "tx_no_ack", "tx_busy", "tx_error", "rx_frames", "rx_filtered",
               "rx_duplicates", "rx_malformed", "beacons", "assoc_requests", "data_requests", "polls_served",
               "polls_empty", "indirect_expired", "queue_full", "pool_full", "associations_accepted",
               "associations_denied", "transport_keys_sent", "transport_key_failures", "announces", "joined",
               "nwk_rx", "nwk_malformed", "nwk_unsecured_dropped", "nwk_unknown_source", "nwk_auth_failures",
               "aps_malformed", "aps_data_up", "aps_auth_failures", "aps_replays", "key_requests",
               "link_keys_sent", "keys_verified", "key_verify_failures", "aps_acks_sent", "aps_acks_rx",
               "zdo_requests", "zdo_responses_sent", "aps_tx", "aps_tx_failures", "aps_events_dropped",
               "devices", "permit", "pan_id", "channel", "store_writes", "store_failures",
               "restored_devices", "restored"]
MESH_NAMES = ["nwk_commands", "link_status_rx", "link_status_tx", "route_records", "rreq_rx", "rrep_rx", "rreq_tx", "mtorr_tx", "network_status_rx", "relayed", "relay_dropped", "not_for_us", "broadcast_duplicates", "no_route", "update_device", "tunnels_sent", "rejoins", "tc_rejoins", "leaves", "timeout_requests", "neighbors", "znp_out_dropped", "znp_in_refused", "route_failures", "neighbors_dropped", "routes_expired", "early_mtorr", "early_mtorr_deferred", "neighbor_restarts", "tx_not_link_failures", "bcast_queue_full", "link_status_failures", "aps_duplicates", "mac_queue_full", "mac_pool_full", "mac_tx_busy", "mac_kicks_deferred", "mac_tx_rounds", "disc_buffered", "disc_sent", "disc_expired", "rreq_coalesced", "aps_counter_reuse", "rreq_suppressed", "bcast_dup_unopened", "net_status_retries"]


def beacon_request_test(mt, channel, report):
    """Essai A: standard MAC beacon requests (no device state changes);
    only the number of beacons and of distinct PAN IDs is kept."""
    t = report["freertos"]
    r, _ = mt.request(0x3F, 0x10, bytes([channel, 0, 1]), timeout=3)
    result = {"channel": channel, "radio_start_status": r[0], "requests": []}
    if r[0] == 0:
        for i in range(3):
            mpdu = bytes([0x03, 0x08, 0xA0 + i, 0xFF, 0xFF, 0xFF, 0xFF, 0x07])
            reply, _ = mt.request(0x3F, 0x13, bytes([0]) + mpdu, timeout=3)
            frames = read_frames(mt, 2.0)
            beacons = [p for c0, c1, p in frames if (c0, c1) == (0x5F, 0x80) and len(p) > 11 and p[11] & 7 == 0]
            pans = {p[14] | (p[15] << 8) for p in beacons if len(p) > 15}
            result["requests"].append({"tx_status": reply[0], "tx_result": reply[1] if len(reply) > 1 else None,
                                       "beacons": len(beacons), "distinct_pan_ids": len(pans)})
        s, _ = mt.request(0x3F, 0x12)
        result["radio_stats"] = decode_counters(s, RADIO_NAMES)
    mt.request(0x3F, 0x11, timeout=3)
    t["beacon_request_test"] = result


def request_keep(mt, cmd0, cmd1, payload, sink, timeout=3.0):
    """Send an SREQ and wait for its SRSP WITHOUT discarding the AREQ events
    received meanwhile (they are appended to `sink`)."""
    from dongle_probe import encode, extract
    mt.serial.write(encode(cmd0, cmd1, payload))
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        mt.buffer.extend(mt.serial.read(max(1, min(mt.serial.in_waiting, 1024))))
        for f in extract(mt.buffer):
            if (f[0], f[1]) == ((cmd0 & 31) | 0x60, cmd1) or (f[0] == 0x60 and f[2][1:] == bytes([cmd0, cmd1])):
                return f[2]
            sink.append(f)
    raise TimeoutError(f"No SRSP for {cmd0:02x}/{cmd1:02x}")


def coordinator_test(mt, channel, pan_id, permit, seconds, report, toggle=False, reboot=False):
    """Essai B: volatile laboratory coordinator (nothing written to Flash).
    Optional: ZCL On/Off Toggle to endpoint 1 of the first joined device."""
    t = report["freertos"]
    frames = []
    # Flag 1: always start a NEW laboratory network (store formatted).
    r = request_keep(mt, 0x3F, 0x14, bytes([channel, 0]) + pan_id.to_bytes(2, "little") + bytes([permit, 1]), frames)
    result = {"channel": channel, "pan_id": hex(pan_id), "permit_seconds": permit, "start_status": r[0],
              "join_events": [], "key_events": [], "aps_events": [], "toggles": [], "reboot": None}
    if r[0] == 0:
        print(f"COORD_WINDOW_OPEN channel={channel} pan={pan_id:#06x} permit={permit}s seconds={seconds}", flush=True)
        end = time.monotonic() + seconds
        target, joined_at, last_toggle, rebooted = None, None, 0.0, False
        while time.monotonic() < end:
            frames.extend(read_frames(mt, 0.5))
            while frames:
                c0, c1, p = frames.pop(0)
                if (c0, c1) == (0x5F, 0x83) and len(p) >= 12:
                    short = p[0] | p[1] << 8
                    item = {"short": hex(short), "capability": hex(p[10]), "at_s": round(seconds - (end - time.monotonic()), 1)}
                    if p[11] == 1:
                        result["key_events"].append(item)
                        print(f"KEY_ESTABLISHED {item}", flush=True)
                    else:
                        result["join_events"].append(item)
                        print(f"JOINED {item}", flush=True)
                        if target is None and p[10] & 0x08:
                            target, joined_at = short, time.monotonic()
                elif (c0, c1) == (0x5F, 0x84) and len(p) >= 11:
                    cluster, profile = p[4] | p[5] << 8, p[6] | p[7] << 8
                    data = p[11:11 + p[10]]
                    zcl_cmd = data[2] if profile == 0x0104 and len(data) >= 3 else None
                    result["aps_events"].append({"src": hex(p[0] | p[1] << 8), "src_ep": p[2], "cluster": hex(cluster),
                                                 "profile": hex(profile), "len": p[10], "aps_secured": bool(p[9]),
                                                 "zcl_command": zcl_cmd, "after_reboot": rebooted})
            now = time.monotonic()
            if reboot and not rebooted and len(result["toggles"]) == 2 and now - last_toggle > 4:
                # Real MCU reset, then restart the coordinator from its store
                # (no new-network flag, joining closed).
                from dongle_probe import encode
                mt.serial.write(encode(0x41, 0x00, b"\x01"))
                boot = read_frames(mt, 3.0)
                ind = [f for f in boot if (f[0], f[1]) == (0x41, 0x80)]
                rr = request_keep(mt, 0x3F, 0x14, bytes([channel, 0]) + pan_id.to_bytes(2, "little") + bytes([0, 0]),
                                  frames)
                st = request_keep(mt, 0x3F, 0x15, b"", frames)
                stats = decode_counters(st, COORD_NAMES)
                result["reboot"] = {"reset_ind": ind[0][2].hex() if ind else None, "start_status": rr[0],
                                    "restored": stats.get("restored"), "restored_devices": stats.get("restored_devices"),
                                    "devices": stats.get("devices"), "permit": stats.get("permit")}
                print(f"REBOOTED {result['reboot']}", flush=True)
                rebooted, last_toggle = True, time.monotonic()
                continue
            if (toggle and target is not None and now - joined_at > 8 and now - last_toggle > 6
                    and len(result["toggles"]) < 4):
                seq = 0x40 + len(result["toggles"])
                payload = target.to_bytes(2, "little") + bytes([1, 1]) + (6).to_bytes(2, "little") + \
                    (0x0104).to_bytes(2, "little") + bytes([1]) + bytes([0x01, seq, 0x02])
                rr = request_keep(mt, 0x3F, 0x17, payload, frames)
                result["toggles"].append({"status": rr[0], "at_s": round(seconds - (end - now), 1),
                                          "after_reboot": rebooted})
                print(f"TOGGLE sent status={rr[0]}", flush=True)
                last_toggle = now
        print("COORD_WINDOW_CLOSED", flush=True)
        st = request_keep(mt, 0x3F, 0x15, b"", frames)
        result["stats"] = decode_counters(st, COORD_NAMES)
        rs = request_keep(mt, 0x3F, 0x12, b"", frames)
        result["radio_stats"] = decode_counters(rs, RADIO_NAMES)
    request_keep(mt, 0x3F, 0x11, b"", frames)
    t["coordinator_test"] = result


def freertos_tests(port, expected_ieee, report, sniff_channel, extra=None):
    from dongle_probe import MT, encode
    from run_dongle_session import wait_mt
    t = report.setdefault("freertos", {})
    mt, capability = wait_mt(port, timeout=15, baud=115200)
    try:
        t["boot_answered_ping"] = True
        t["ping_capability"] = int.from_bytes(capability, "little")
        version, _ = mt.request(0x21, 2)
        t["version"] = {"transport": version[0], "product": version[1],
                        "release": f"{version[2]}.{version[3]}.{version[4]}",
                        "build": int.from_bytes(version[5:9], "little")}
        ieee, _ = mt.request(0x21, 4)
        t["ieee_matches"] = int.from_bytes(ieee, "little") == int(expected_ieee, 16)
        info, _ = mt.request(0x3F, 0x00)
        t["info_first_boot"] = decode_info(info)
        tasks = []
        for i in range(t["info_first_boot"]["tasks"]):
            p, _ = mt.request(0x3F, 0x01, bytes([i]))
            name = p[2:10].rstrip(b"\0").decode(errors="replace")
            prio, state, hwm, runtime = struct.unpack_from("<BBHI", p, 10)
            tasks.append({"name": name, "priority": prio, "state": state, "stack_free_words": hwm})
        t["tasks"] = tasks
        # Unknown Z-Stack commands must be refused with an MT RPC error.
        mt.serial.write(encode(0x21, 0x13, struct.pack("<H", 0x21)))   # SYS_OSAL_NV_LENGTH
        refused = [f for f in read_frames(mt, 0.5) if f[0] == 0x60]
        t["zstack_nv_command_refused"] = bool(refused) and refused[0][2][0] == 2
        latencies = []
        for _ in range(1000):
            r, ms = mt.request(0x21, 1)
            if r != capability:
                raise RuntimeError("SYS_PING mismatch")
            latencies.append(ms)
        latencies.sort()
        t["ping_1000_ms"] = {"min": latencies[0], "median": latencies[500], "p99": latencies[990], "max": latencies[-1]}
        for noise in [b"\xfe" * 300, bytes.fromhex("fe fa 21"), bytes.fromhex("fe 00 21 01 ff")]:
            mt.serial.write(noise)
            r, _ = mt.request(0x21, 1, timeout=1)
            if r != capability:
                raise RuntimeError("UART framing recovery failed")
        t["uart_noise_recovery"] = True
        c1, _ = mt.request(0x3F, 0x02)
        time.sleep(10)
        c2, _ = mt.request(0x3F, 0x02)
        a, b = decode_counters(c1, COUNTER_NAMES), decode_counters(c2, COUNTER_NAMES)
        t["counters_after_10s"] = b
        t["watchdog_feeds_in_10s"] = b["wd_feeds"] - a["wd_feeds"]
        t["watchdog_misses_in_10s"] = b["wd_misses"] - a["wd_misses"]
        info, _ = mt.request(0x3F, 0x00)
        t["info_after_load"] = decode_info(info)
        if sniff_channel:
            r, _ = mt.request(0x3F, 0x10, bytes([sniff_channel, 0, 1]), timeout=3)
            t["radio_start_status"] = r[0]
            frames = read_frames(mt, 20) if r[0] == 0 else []
            rx = [f for f in frames if f[0] == 0x5F and f[1] == 0x80]
            kinds = {}
            for _, _, p in rx:
                if len(p) > 11:
                    kinds[p[11] & 7] = kinds.get(p[11] & 7, 0) + 1
            rssi = sorted(struct.unpack("b", bytes([p[2]]))[0] for _, _, p in rx) if rx else []
            # Only counts and statistics are kept: no frame content is stored.
            t["passive_rx"] = {"channel": sniff_channel, "seconds": 20, "frames": len(rx),
                               "frame_types": {str(k): v for k, v in sorted(kinds.items())},
                               "crc_ok": sum(1 for _, _, p in rx if p[4] & 1),
                               "rssi_median": rssi[len(rssi) // 2] if rssi else None}
            s, _ = mt.request(0x3F, 0x12)
            t["radio_stats"] = decode_counters(s, RADIO_NAMES)
            r, _ = mt.request(0x3F, 0x11, timeout=3)
            t["radio_stop_status"] = r[0]
        if extra and extra.beacon_request_channel:
            beacon_request_test(mt, extra.beacon_request_channel, report)
        if extra and extra.coord_channel:
            coordinator_test(mt, extra.coord_channel, extra.coord_pan, extra.coord_permit,
                             extra.coord_seconds, report, extra.coord_toggle, extra.coord_reboot_test)
        # Requested reset: the image must come back and report it.
        mt.serial.write(encode(0x41, 0x00, b"\x01"))
        frames = read_frames(mt, 3)
        ind = [f for f in frames if f[0] == 0x41 and f[1] == 0x80]
        t["reset_ind_after_reset_req"] = ind[0][2].hex() if ind else None
        info, _ = mt.request(0x3F, 0x00, timeout=3)
        t["info_after_reset"] = decode_info(info)
        t["passed"] = bool(t["ieee_matches"] and t["version"]["product"] == 0xFE and t["ping_capability"] == 1
                           and t["zstack_nv_command_refused"] and t["watchdog_misses_in_10s"] == 0
                           and t["watchdog_feeds_in_10s"] >= 30 and ind
                           and t["info_first_boot"]["init_status"] == 0)
        # Informative only: SRAM retention across a warm reset is not
        # guaranteed on CC26x2 (not observed in session r3).
        t["crash_record_survived_warm_reset"] = t["info_after_reset"]["previous_crash_kind"] == 6
        t["crash_frame_emitted_before_reset"] = any(f[0] == 0x5F and f[1] == 0x82 for f in frames)
    finally:
        mt.close()


def verify_zstack(port, expected_ieee, report):
    from run_dongle_session import wait_mt
    mt, capability = wait_mt(port, timeout=30, baud=INSTALLED_BAUD)
    try:
        version, _ = mt.request(0x21, 2)
        ieee, _ = mt.request(0x21, 4)
        revision = int.from_bytes(version[5:9], "little")
        report["restored_zstack"] = {"ping_capability": hex(int.from_bytes(capability, "little")),
                                     "revision": revision, "product": version[1],
                                     "ieee_matches": int.from_bytes(ieee, "little") == int(expected_ieee, 16)}
        if revision != INSTALLED_REVISION or not report["restored_zstack"]["ieee_matches"]:
            raise ValueError("Restored firmware is not the expected Z-Stack image")
    finally:
        mt.close()


# ----------------------------------------------------------------- session
def restore(args, folder, report, original, guard=False):
    from migrate_production import capture_twice, write_verified
    from run_dongle_session import open_rom, boot
    command, device = open_rom(args.port, args.usb_serial, args.expect_ieee)
    try:
        after = capture_twice(command, folder, "after-test")
        if guard:
            rollback_guard(after, args, report)
        report["nv_identical_after_test"] = after[NV_START:NV_END] == original[NV_START:NV_END]
        report["ccfg_and_spare_identical_after_test"] = after[NV_END:] == original[NV_END:]
        if not (report["nv_identical_after_test"] and report["ccfg_and_spare_identical_after_test"]):
            raise RuntimeError("NV/CCFG differ from the backup: automatic restore stopped, see manual recovery")
        report["restored"] = write_verified(command, device, original, after, folder, "restored")
        report["restored_equals_backup"] = (folder / "restored-readback.bin").read_bytes() == original
        boot(command)
    finally:
        command.close()
    verify_zstack(args.port, args.expect_ieee, report)


def session(args):
    from migrate_production import capture_twice, write_verified
    from run_dongle_session import open_rom, boot
    folder = args.session_dir.resolve()
    if not folder.is_relative_to((ROOT / "private").resolve()):
        raise ValueError("The session directory must be under private/ (not versioned)")
    folder.mkdir(parents=True, exist_ok=False)
    hex_memory = read_hex(FREERTOS_HEX)
    report = {"image": str(FREERTOS_HEX.relative_to(ROOT)), "image_hex_sha256": sha(FREERTOS_HEX.read_bytes()),
              "z2m_stopped_confirmed_by_user": args.z2m_stopped,
              "radio_transmission": bool(args.beacon_request_channel or args.coord_channel),
              "nv_written": False, "freertos_passed": False, "restore_passed": False}
    original = None
    programmed = False
    command = None
    try:
        command, device = open_rom(args.port, args.usb_serial, args.expect_ieee)
        original = capture_twice(command, folder, "original")
        report["backup_sha256"] = sha(original)
        report["backup_files"] = ["original-a.bin", "original-b.bin"]
        target = compose_freertos(original, hex_memory)
        (folder / "freertos-target.bin").write_bytes(target)
        programmed = True
        report["freertos_programmed"] = write_verified(command, device, target, original, folder, "freertos")
        boot(command)
        try:
            capture_boot(command.sp, report)
        except Exception as exc:
            report.setdefault("freertos", {})["boot_capture_error"] = f"{type(exc).__name__}: {exc}"
        command.close()
        command = None
        try:
            freertos_tests(args.port, args.expect_ieee, report, args.sniff_channel, args)
        except Exception as exc:
            report.setdefault("freertos", {})["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        if command:
            try:
                boot(command)
            except Exception:
                pass
            command.close()
        if programmed and original is not None:
            try:
                restore(args, folder, report, original)
                report["restore_passed"] = bool(report.get("restored_equals_backup") and
                                                report.get("restored_zstack", {}).get("ieee_matches"))
                report["freertos_passed"] = bool(report.get("freertos", {}).get("passed"))
            except Exception as exc:
                report["restore_error"] = f"{type(exc).__name__}: {exc}"
                report["manual_recovery"] = (
                    "Keep the dongle on this PC. Run: python tools/freertos_lab_session.py restore "
                    f"--port {args.port} --usb-serial {args.usb_serial} --expect-ieee {args.expect_ieee} "
                    f"--session-dir {folder}")
        (folder / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(report, indent=2))


STORE_START = 0x34000   # storage layout 2 (layout 1: 0x48000, inside it)
STORAGE_LAYOUT = 2


def read_range(command, start, end):
    """Read [start, end) twice through the ROM; both reads must be identical."""
    from capture_dongle import read_words
    out = []
    for _ in range(2):
        data = bytearray()
        for address in range(start, end, 252):
            data += read_words(command, address, min(63, (end - address) // 4))
        out.append(bytes(data))
    if out[0] != out[1]:
        raise ValueError(f"Two reads of {start:#x}-{end:#x} differ")
    return out[0]


def read_nv_ccfg(command):
    return read_range(command, NV_START, FLASH_SIZE)


def check_backup(path, nv_ccfg):
    """The production backup must exist, be a double-verified full image and
    hold the SAME NV/CCFG as the dongle now (so it restores exactly)."""
    a = path.read_bytes()
    b = path.with_name(path.name.replace("-a.bin", "-b.bin")).read_bytes()
    if a != b or len(a) != FLASH_SIZE:
        raise ValueError("Production backup missing or not double-verified")
    if a[NV_START:] != nv_ccfg:
        raise ValueError("Dongle NV/CCFG differ from the backup: refusing (take a fresh backup first)")
    return a


def fast_flash(args):
    """Development loop: program the FreeRTOS image and LEAVE it running.
    NV and CCFG are read, preserved and covered by the full-Flash CRC check."""
    from migrate_production import program_application
    from run_dongle_session import open_rom, boot
    image = freertos_hex(args.baud, getattr(args, "hw_aes", False))
    hex_memory = read_hex(image)
    report = {"action": "flash", "image": str(image.relative_to(ROOT)), "uart_baud": args.baud,
              "image_hex_sha256": sha(image.read_bytes()), "production_restored": False}
    command, device = open_rom(args.port, args.usb_serial, args.expect_ieee)
    try:
        if args.take_backup:
            # Full double capture in the SAME ROM session, right before
            # programming: Z-Stack cannot boot (and touch its NV) in between.
            from migrate_production import capture_twice
            folder = args.take_backup.resolve()
            if not folder.is_relative_to((ROOT / "private").resolve()):
                raise ValueError("The backup directory must be under private/")
            folder.mkdir(parents=True, exist_ok=False)
            capture_twice(command, folder, "original")
            args.production_backup = folder / "original-a.bin"
        nv_ccfg = read_nv_ccfg(command)
        backup = check_backup(args.production_backup.resolve(), nv_ccfg)
        # Never back to an older storage layout over a migrated store: the
        # old firmware would find no store (layout 1 headers erased) or, worse,
        # format one inside the new bank. Identity of the image: 0x10C.
        import freertos_store
        image_layout = int.from_bytes(bytes(hex_memory.get(0x10C + i, 0xFF) for i in range(4)), "little")
        store_layout = freertos_store.layout(backup)
        report["storage_layout"] = {"image": image_layout, "store": store_layout}
        if store_layout and image_layout < store_layout and not args.wipe_store:
            raise ValueError(f"Image storage layout {image_layout} older than the store's ({store_layout}): refused")
        report["backup"] = str(args.production_backup)
        report["backup_sha256"] = sha(backup)
        current = bytearray(b"\xff" * FLASH_SIZE)
        current[NV_START:] = nv_ccfg
        target = bytearray(compose_freertos(bytes(current), hex_memory))
        if not args.wipe_store:
            # Keep the FreeRTOS nv_log store: the lab network and its paired
            # devices survive the firmware update.
            target[STORE_START:NV_START] = read_range(command, STORE_START, NV_START)
        report["store_preserved"] = not args.wipe_store
        target = bytes(target)
        report["crc32_full_flash"] = program_application(command, device, target, bytes(current))
        boot(command)
    finally:
        command.close()
    from run_dongle_session import wait_mt
    mt, capability = wait_mt(args.port, timeout=15, baud=args.baud)
    try:
        version, _ = mt.request(0x21, 2)
        report["running"] = {"product": version[1], "build": int.from_bytes(version[5:9], "little"),
                             "capability": int.from_bytes(capability, "little")}
    finally:
        mt.close()
    print(json.dumps(report, indent=2))
    return report


def migrate(args):
    """PRODUCTION migration to the FreeRTOS coordinator. Never automatic:
    refused without --confirm-production-migration. Steps:
      1. inspect the STOPPED Z2M data (private copy): unified backup present,
         same coordinator IEEE as the dongle, zstack adapter, its baud rate;
      2. rehearsal: the real herdsman restores THAT backup into the simulated
         firmware and every carried device, key and counter must match;
      3. double full capture of the dongle in the same ROM session (rollback
         image, Z-Stack NV included), FreeRTOS image at the Z2M baud rate,
         empty FreeRTOS store (herdsman will restore the backup into it);
      4. the firmware must answer SYS_VERSION.
    Then start Z2M unchanged: herdsman detects the unconfigured adapter and
    restores the backup ("restored"). Rollback: action `restore` with the
    capture of step 3."""
    import subprocess
    sys.path.insert(0, str(ROOT / "tools"))
    from prepare_production_migration import inspect_data
    data = args.z2m_data.resolve()
    if not data.is_relative_to((ROOT / "private").resolve()):
        raise ValueError("--z2m-data must be a private copy of the stopped Z2M data directory")
    info = inspect_data(data)
    # herdsman restores the NWK frame counter as backup + 2500: the backup
    # must be the one Z2M wrote when it was cleanly stopped just before.
    age = time.time() - (data / "coordinator_backup.json").stat().st_mtime
    if age > 2 * 3600:
        raise ValueError("coordinator_backup.json is older than 2 h: restart then cleanly stop Z2M on the "
                         "Z-Stack firmware first, and copy its data again")
    if info["ieee"].removeprefix("0x").lower() != args.expect_ieee.removeprefix("0x").lower():
        raise ValueError("Z2M coordinator IEEE differs from the dongle")
    if info["configured_baudrate"] not in (115200, 1000000):
        raise ValueError("Unsupported Z2M baud rate")
    args.baud = info["configured_baudrate"]
    rehearsal = subprocess.run(["node", str(ROOT / "tools/znp_migration_rehearsal.cjs"), "--backup",
                                str(data / "coordinator_backup.json")], cwd=ROOT, capture_output=True, text=True,
                               timeout=900)
    try:
        result = json.loads(rehearsal.stdout[rehearsal.stdout.index("{"):])
    except ValueError:
        raise ValueError("Rehearsal produced no report")
    if not result.get("passed"):
        raise ValueError("Rehearsal failed: " + str(result.get("error")))
    args.wipe_store = True
    report = fast_flash(args)
    if not report.get("running") or report["running"]["product"] != 1:
        raise ValueError("The FreeRTOS firmware did not answer as expected; roll back with the `restore` action")
    print(json.dumps({"migration": "firmware installed", "next": "start Zigbee2MQTT unchanged (expect 'restored')",
                      "rehearsal_devices_identical": result.get("devices_identical"),
                      "rollback_capture": str(args.take_backup)}, indent=2))


def run_tests_only(args):
    """Run the diagnostic and optional radio/coordinator tests on the image
    already running on the dongle (no flashing, no restore)."""
    folder = args.session_dir.resolve()
    folder.mkdir(parents=True, exist_ok=False)
    report = {"action": "test", "freertos": {}}
    try:
        freertos_tests(args.port, args.expect_ieee, report, args.sniff_channel, args)
    except Exception as exc:
        report["freertos"]["error"] = f"{type(exc).__name__}: {exc}"
    report["freertos_passed"] = bool(report["freertos"].get("passed"))
    (folder / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: v for k, v in report.items() if k != "freertos"}, indent=2))


def fresh_backup(args):
    """Double full capture of the dongle as it is now (Z-Stack production
    image expected), to be used as --production-backup for later restores."""
    from migrate_production import capture_twice
    from run_dongle_session import open_rom, boot
    folder = args.session_dir.resolve()
    if not folder.is_relative_to((ROOT / "private").resolve()):
        raise ValueError("The backup directory must be under private/")
    folder.mkdir(parents=True, exist_ok=False)
    command, device = open_rom(args.port, args.usb_serial, args.expect_ieee)
    try:
        image = capture_twice(command, folder, "original")
        boot(command)
    finally:
        command.close()
    print(json.dumps({"action": "backup", "files": [str(folder / "original-a.bin"), str(folder / "original-b.bin")],
                      "sha256": sha(image)}, indent=2))


def rollback_guard(after, args, report):
    """A byte-for-byte restore puts back the Z-Stack NV of the capture, with
    the NWK frame counter of THAT moment. If the network then ran on the
    FreeRTOS firmware (its store holds a network), the coordinator would go
    back to counters already used: devices drop its frames, and CCM* nonces
    repeat under the same key. Use the `rollback` action instead."""
    import freertos_store
    recs = freertos_store.records(after)
    report["freertos_store_network"] = freertos_store.has_network(recs)
    report["freertos_store_counters"] = freertos_store.counters(recs)
    if report["freertos_store_network"] and not getattr(args, "accept_counter_regression", False):
        raise RuntimeError("The FreeRTOS store holds a network: restoring the old Z-Stack NV would bring the "
                           "NWK frame counter back below values already used. Use the `rollback` action "
                           "(fresh backup + Z-Stack state cleared + restoreBackup), or pass "
                           "--accept-counter-regression for a laboratory network only")


def clear_zstack_state(port, report):
    """On the restored Z-Stack: STARTUP_OPTION = clear config + state, drop the
    'configured' marker, reset (standard ZNP commands, as herdsman uses).
    herdsman then finds an unconfigured adapter and restores the backup."""
    from run_dongle_session import wait_mt
    from dongle_probe import encode
    mt, _ = wait_mt(port, timeout=30, baud=INSTALLED_BAUD)
    try:
        status, _ = mt.request(0x21, 0x09, struct.pack("<HBB", 0x0003, 0, 1) + b"\x03")   # SYS_OSAL_NV_WRITE
        report["startup_option_written"] = status[:1] == b"\x00"
        deleted, _ = mt.request(0x21, 0x12, struct.pack("<HH", 0x0060, 1))                # SYS_OSAL_NV_DELETE
        report["has_configured_deleted_status"] = deleted[0]
        mt.serial.write(encode(0x41, 0x00, b"\x01"))                                      # SYS_RESET_REQ
    finally:
        mt.close()
    time.sleep(3)
    mt, _ = wait_mt(port, timeout=30, baud=INSTALLED_BAUD)
    try:
        read, _ = mt.request(0x21, 0x08, struct.pack("<HB", 0x0060, 0))                   # SYS_OSAL_NV_READ
        report["has_configured_after_reset"] = read.hex()
        report["zstack_unconfigured"] = read[0] != 0 or (len(read) > 2 and read[2] != 0x55)
    finally:
        mt.close()
    if not (report["startup_option_written"] and report["zstack_unconfigured"]):
        raise RuntimeError("Z-Stack state not cleared: do NOT start Zigbee2MQTT, see docs/firmware/restauration.md")


def rollback(args):
    """FreeRTOS -> Z-Stack production rollback WITHOUT counter regression.
    Needs: the pre-migration capture (--session-dir), a FRESH unified backup
    read from the FreeRTOS dongle with Z2M stopped (tools/production_nv_snapshot.cjs,
    --fresh-backup) and --confirm-production-rollback. Steps: check that the
    backup NWK counter + 2500 (the herdsman restore increment) exceeds what the
    FreeRTOS store has reserved; rewrite the Z-Stack APPLICATION only (its NV,
    untouched by FreeRTOS, and the CCFG stay); have Z-Stack clear its network
    state; then Z2M is started with the fresh backup and restores it."""
    from migrate_production import capture_twice, write_verified
    from run_dongle_session import open_rom, boot
    import freertos_store
    folder = args.session_dir.resolve()
    a, b = (folder / "original-a.bin").read_bytes(), (folder / "original-b.bin").read_bytes()
    if a != b or len(a) != FLASH_SIZE:
        raise ValueError("Capture files missing or inconsistent")
    fresh_path = args.fresh_backup.resolve()
    if not fresh_path.is_relative_to((ROOT / "private").resolve()):
        raise ValueError("--fresh-backup must be under private/")
    if time.time() - fresh_path.stat().st_mtime > 2 * 3600:
        raise ValueError("--fresh-backup is older than 2 h: stop Z2M and read it again")
    fresh = json.loads(fresh_path.read_text(encoding="utf-8"))
    if "0x" + fresh["coordinator_ieee"].lower() != args.expect_ieee.lower():
        raise ValueError("The fresh backup is not this coordinator's")
    fc = int(fresh["network_key"]["frame_counter"])
    sub = folder / f"rollback-{int(time.time())}"
    sub.mkdir()
    report = {"action": "rollback", "fresh_backup": str(fresh_path), "fresh_frame_counter": fc}
    command, device = open_rom(args.port, args.usb_serial, args.expect_ieee)
    try:
        after = capture_twice(command, sub, "before-rollback")
        recs = freertos_store.records(after)
        reserved = freertos_store.counters(recs)
        report["freertos_reserved_counters"] = reserved
        if reserved is None:
            raise RuntimeError("No FreeRTOS counters record: nothing to protect, use `restore`")
        if fc + 2500 <= reserved[0]:
            raise RuntimeError(f"Backup counter {fc} + 2500 does not exceed the reserved {reserved[0]}: "
                               "the backup is older than the FreeRTOS traffic, read it again just before")
        if after[NV_START:] != a[NV_START:]:
            raise RuntimeError("Z-Stack NV/CCFG differ from the capture: automatic rollback stopped")
        report["restored"] = write_verified(command, device, a, after, sub, "restored")
        boot(command)
    finally:
        command.close()
    verify_zstack(args.port, args.expect_ieee, report)
    clear_zstack_state(args.port, report)
    (sub / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({**report, "next": "copy --fresh-backup over the coordinator_backup.json of Z2M, then start "
                                        "Z2M unchanged: expect strategy restoreBackup / restored"}, indent=2))


def manual_restore(args):
    folder = args.session_dir.resolve()
    a, b = (folder / "original-a.bin").read_bytes(), (folder / "original-b.bin").read_bytes()
    if a != b or len(a) != FLASH_SIZE:
        raise ValueError("Backup files missing or inconsistent")
    report = {"manual_restore": True}
    sub = folder / f"manual-restore-{int(time.time())}"
    sub.mkdir()
    restore(args, sub, report, a, guard=True)
    (sub / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


def self_test():
    """Offline: compose against the last verified full read-back of the dongle."""
    capture = ROOT / "private/production-sdk833-r2-20261118/install/target-readback.bin"
    current = capture.read_bytes()
    target = compose_freertos(current, read_hex(FREERTOS_HEX))
    assert target[NV_START:] == current[NV_START:]
    assert target[:NV_START] != current[:NV_START]
    tampered = dict(read_hex(FREERTOS_HEX))
    tampered[NV_START + 4] = 0
    try:
        compose_freertos(current, tampered)
        raise AssertionError("an image touching NV must be refused")
    except ValueError:
        pass
    tampered = dict(read_hex(FREERTOS_HEX))
    tampered[0x57FD8] ^= 0xFF
    try:
        compose_freertos(current, tampered)
        raise AssertionError("an image changing CCFG must be refused")
    except ValueError:
        pass
    print(json.dumps({"self_test": "passed", "freertos_bytes": sum(1 for i in range(NV_START) if target[i] != 0xFF),
                      "nv_and_ccfg_preserved": True, "reference_capture": str(capture.relative_to(ROOT))}, indent=2))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("action", nargs="?", choices=["session", "restore", "flash", "test", "backup", "migrate", "rollback"],
                    default="session")
    ap.add_argument("--baud", type=int, choices=[115200, 1000000], default=115200,
                    help="flash: UART rate of the FreeRTOS image (Z2M serial.baudrate)")
    ap.add_argument("--z2m-data", type=Path, help="migrate: private copy of the STOPPED Z2M data directory")
    ap.add_argument("--confirm-production-migration", action="store_true",
                    help="migrate: explicit consent of the network owner (never implied)")
    ap.add_argument("--take-backup", type=Path, help="flash: double full capture into this private dir first")
    ap.add_argument("--wipe-store", action="store_true", help="flash: erase the FreeRTOS nv_log store too")
    ap.add_argument("--hw-aes", action="store_true", help="flash: the --hw-aes variant (AES accelerator)")
    ap.add_argument("--production-backup", type=Path,
                    default=ROOT / "private/freertos-lab-20261118-r7/original-a.bin")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--fresh-backup", type=Path,
                    help="rollback: unified backup read from the FreeRTOS dongle (Z2M stopped) just before")
    ap.add_argument("--confirm-production-rollback", action="store_true",
                    help="rollback: explicit consent of the network owner (never implied)")
    ap.add_argument("--accept-counter-regression", action="store_true",
                    help="restore: allow putting back an old Z-Stack NV over a network run on FreeRTOS "
                         "(laboratory networks only)")
    ap.add_argument("--port")
    ap.add_argument("--usb-serial")
    ap.add_argument("--expect-ieee")
    ap.add_argument("--session-dir", type=Path)
    ap.add_argument("--z2m-stopped", action="store_true")
    ap.add_argument("--sniff-channel", type=int, choices=range(11, 27), metavar="11..26")
    ap.add_argument("--beacon-request-channel", type=int, choices=range(11, 27), metavar="11..26",
                    help="Essai A: TRANSMITS 3 standard beacon requests on this channel")
    ap.add_argument("--coord-channel", type=int, choices=range(11, 27), metavar="11..26",
                    help="Essai B: TRANSMITS as a volatile laboratory coordinator on this channel")
    ap.add_argument("--coord-pan", type=lambda v: int(v, 0), default=0x6A5B)
    ap.add_argument("--coord-permit", type=int, default=0, choices=range(0, 256), metavar="0..255")
    ap.add_argument("--coord-seconds", type=float, default=30.0)
    ap.add_argument("--coord-reboot-test", action="store_true",
                    help="After 2 toggles: real reset, restore the network from the store, toggle again")
    ap.add_argument("--coord-toggle", action="store_true",
                    help="Send 4 ZCL On/Off Toggle commands to endpoint 1 of the first RX-on device that joins")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if not (args.port and args.usb_serial and args.expect_ieee and (args.session_dir or args.action in ("flash", "migrate"))):
        ap.error("--port, --usb-serial, --expect-ieee and --session-dir are required")
    if args.action == "restore":
        return manual_restore(args)
    if args.action == "rollback":
        if not (args.z2m_stopped and args.confirm_production_rollback and args.fresh_backup):
            ap.error("rollback needs --z2m-stopped, --fresh-backup and --confirm-production-rollback")
        return rollback(args)
    if args.action == "flash":
        if not args.z2m_stopped:
            ap.error("Stop Zigbee2MQTT first and pass --z2m-stopped")
        return fast_flash(args)
    if args.action == "migrate":
        if not (args.z2m_stopped and args.confirm_production_migration and args.z2m_data and args.take_backup):
            ap.error("migrate needs --z2m-stopped, --z2m-data, --take-backup and --confirm-production-migration")
        return migrate(args)
    if args.action == "test":
        return run_tests_only(args)
    if args.action == "backup":
        return fresh_backup(args)
    if not args.z2m_stopped:
        ap.error("Stop Zigbee2MQTT first and pass --z2m-stopped")
    session(args)


if __name__ == "__main__":
    main()
