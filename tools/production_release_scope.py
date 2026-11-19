"""Attach production startup evidence only to its exact tested image/profile."""
import hashlib
import json

def attach(result, path):
    if not path.is_file(): return False
    proof = json.loads(path.read_text(encoding="utf-8"))
    if proof["experimental_hex_sha256"] != result["hex_sha256"]: return False
    if not (proof["baudrate"] == result["baudrate"] and proof["z2m_startup_tested"]
            and proof["MQTT_health_check_passed"] and proof["coordinator_check_passed"]
            and proof["network_keys_preserved"] and proof["full_readback_verified"]
            and proof["all_database_devices_in_coordinator_backup"] and not proof["final_qualified"]):
        raise ValueError("Production startup evidence has inconsistent scope")
    result.update(z2m_startup_tested=True, MQTT_health_tested=True, production_trial_started=True,
        production_network_device_count=proof["database_devices"],
        production_radio_observed_device_count=proof["fresh_radio_device_count"],
        production_all_devices_validated=proof["all_74_radio_devices_validated"],
        production_report_sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    if proof.get('hardware_tested'):
        result['hardware_tested'] = True
        result['hardware_test_scope'] = proof['hardware_test_scope']
        result['radio_peer_exchange_tested'] = proof.get('radio_peer_exchange_tested', False)
        result['radio_test_peer_count'] = proof.get('controlled_radio_peers', 1)
        result['radio_test_model'] = proof.get('controlled_radio_model')
        result['logical_backup_restore_tested'] = proof.get('logical_backup_restore_tested', False)
        result['fresh_native_backup_tested'] = proof.get('fresh_native_backup_verified', False)
        result['MQTT_archive_export_response_received'] = proof.get('MQTT_archive_export_response_received', False)
    return True
