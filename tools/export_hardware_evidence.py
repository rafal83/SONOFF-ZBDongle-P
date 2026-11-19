"""Export an explicitly selected completed session as a secret-free report.

No serial access. Verify the private full rollback files before exporting only
the known safe measurements and booleans. Never copy host logs or backups.
"""
import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def verify_rollback(r):
    folder = Path(r["rollback_directory"]).resolve()
    if not folder.is_relative_to((ROOT / "private").resolve()): raise ValueError("Rollback files must be private")
    original = (folder / "original-a.bin").read_bytes()
    if (len(original) != 360448 or original != (folder / "original-b.bin").read_bytes()
            or original != (folder / "restore-readback.bin").read_bytes()
            or hashlib.sha256(original).hexdigest() != r["restore_readback_sha256"]
            or not r["restore_verified"] or r.get("restore_error")
            or r["production_network_loaded_into_experimental"] or r["Z2M_used"]):
        raise ValueError("Original rollback or lab isolation does not match")
    return len(original)

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--session-report", required=True, type=Path)
    ap.add_argument("--identity-report", required=True, type=Path)
    ap.add_argument("--transport-session-report", type=Path,
                    help="Independent serial/NV scope on the EXACT same HEX; never inherit its radio result")
    ap.add_argument("--output", required=True, type=Path)
    args = ap.parse_args()
    r = json.loads(args.session_report.read_text(encoding="utf-8"))
    identity = json.loads(args.identity_report.read_text(encoding="utf-8"))
    flash_size = verify_rollback(r)
    transport = json.loads(args.transport_session_report.read_text()) if args.transport_session_report else r
    verify_rollback(transport)
    if transport["experimental_hex_sha256"] != r["experimental_hex_sha256"]:
        raise ValueError("Cannot combine hardware evidence from different firmware images")
    baudrate = r.get("baudrate", 115200)
    if transport.get("baudrate", 115200) != baudrate:
        raise ValueError("Cannot combine different serial rates")
    if not (transport["serial_and_scratch_NV_tests_passed"] and r["restore_verified"] and identity["passed"]
            and identity["revision"] == 20240710 and not r.get("test_error") and not r.get("restore_error")):
        raise ValueError("Session/original boot did not pass")
    fields = ["experimental_hex_sha256", "experimental_boot", "scratch_NV_write_read",
        "scratch_NV_survived_soft_reset", "ping_300", "UART_noise_truncation_bad_FCS",
        "auto_baud_enabled", "serial_and_scratch_NV_tests_passed", "restore_verified"]
    public = {key: transport[key] for key in fields}
    public.update(date="2026-11-17", target="SONOFF ZBDongle-P CC2652P", baudrate=baudrate,
        Z2M_used=False, radio_exchange_tested=False, commissioning_tested=False,
        production_network_loaded_into_experimental=False, original_revision_after_restore=20240710,
        original_responded_after_restore=True, full_flash_restore_readback_identical=True,
        flash_backup_bytes=flash_size, backup_copies_identical_and_ROM_CRC_checked=True,
        final_qualified=False, auto_baud_deferred_by_user=True)
    if "herdsman" in r:
        h = r["herdsman"]
        if not h["passed"]: raise ValueError("Herdsman qualification failed")
        keys = ["passed", "runtime", "Z2M_used", "MQTT_used", "radio_peer_exchange_tested",
            "associations_opened", "production_NV_loaded", "adapter_start", "first_start_result",
            "permit_join_closed", "network_parameters", "local_node_descriptor", "local_active_endpoints",
            "endpoint_count", "local_simple_descriptor_1", "local_simple_descriptor_242", "network_backup",
            "soft_reset", "adapter_restart", "second_start_result", "network_key_and_parameters_persisted",
            "frame_counter_non_decreasing_after_reset", "local_AF_data_confirm_and_incoming_ZCL",
            "clear_experimental_network", "adapter_backup_restore", "backup_restore_start_result",
            "backup_restored_key_and_parameters", "frame_counter_non_decreasing_after_backup_restore"]
        public["herdsman"] = {key: h[key] for key in keys if key in h}
        public["commissioning_tested"] = h["adapter_start"]
    if "radio" in r:
        h = r["radio"]
        if not (h["passed"] and h["association_window_closed"] and h["radio_exchange_after_backup_restore"]):
            raise ValueError("Complete radio qualification did not pass")
        keys = ["passed", "Z2M_used", "MQTT_used", "production_NV_loaded", "hardware_peer_model",
            "radio_peer_exchange_tested", "associations_opened", "association_window_seconds", "harness_sha256",
            "herdsman_version", "adapter_start", "firmware_revision", "association_window_closed", "peer_joined",
            "remote_node_descriptor", "remote_manufacturer_code", "tuya_early_basic_read", "remote_active_endpoints",
            "remote_active_endpoint_list", "remote_simple_descriptors", "remote_endpoint_count", "remote_basic_attributes",
            "remote_on_off_command_and_readback", "remote_repeated_attribute_reads", "repeated_read_count",
            "backup_with_radio_peer", "configured_security_level", "adapter_resume_with_peer",
            "radio_exchange_after_coordinator_reset", "clear_experimental_network", "restore_backup_with_radio_peer",
            "radio_exchange_after_backup_restore", "frame_counter_non_decreasing_after_restore", "diagnostics",
            "baudrate", "peer_resumed_from_private_lab_NV", "application_read_latencies",
            "load_test_changed_NV_writes", "load_test_radio_reads", "radio_and_NV_load_test"]
        public["radio"] = {key: h[key] for key in keys if key in h}
        public["radio_exchange_tested"] = True
        public["commissioning_tested"] = bool(h.get("associations_opened") and h.get("peer_joined"))
    if args.transport_session_report:
        public["transport_evidence_scope"] = "Serial/scratch NV only, independent session on the same HEX"
        public["transport_session_radio_passed"] = transport.get("radio", {}).get("passed", False)
        public["evidence_sources_sha256"] = {
            "radio_session": hashlib.sha256(args.session_report.read_bytes()).hexdigest(),
            "transport_session": hashlib.sha256(args.transport_session_report.read_bytes()).hexdigest()}
    args.output.write_text(json.dumps(public, indent=2) + "\n", encoding="utf-8")
    print("Exported verified, secret-free hardware evidence")

if __name__ == "__main__": main()
