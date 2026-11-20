# Outils

| Usage | Commandes et fichiers |
|---|---|
| Sources verrouillées | `fetch_sources.py` |
| Support matériel TI | `download_ti_toolchain.py sdk833 --extract --device-support-only` |
| Build courant | `build_freertos.py all` |
| Tests partagés | `test_core_engines.py`, `build.py host`, `tests/herdsman_framing.mjs` |
| Tests ARM | `test_arm_freertos.py`, `stack_analysis.py` |
| Compatibilité hôte | `setup_herdsman_lab.py`, `znp_sim_acceptance.cjs`, `znp_sim_touchlink.cjs` (Touchlink de herdsman) |
| Intégrité / distribution | `image.py`, `audit_distribution.py`, `package_freertos_ci.py` |
| Web flasher | `package_web_flasher.py`, `tests/web_flasher.test.mjs` |
| Sauvegarde et restauration | `capture_dongle.py`, `freertos_lab_session.py`, `freertos_store.py`, `znp_migration_rehearsal.cjs` |
| Fuzzing | `fuzz_freertos.py`, `fuzz/Dockerfile` |
| Performances (sans matériel) | `bench_freertos.py` (CPU émulé), `bench_transaction_cpu.py` (CPU par étape d'une transaction), `bench_throughput.py` (débit soutenu et endormis, **simulation** : `sim/net_bench.c`) |

Les données du dongle et clés restent sous `private/`, hors Git et hors paquets. Les outils matériels demandent une séance, une identité explicite et l'arrêt de Zigbee2MQTT. Le [flasher](../docs/firmware/restauration.md) restaure une application compatible en conservant les données réseau actuelles. Un build n'effectue aucune action matérielle.

Voir [les licences](../docs/dependencies.md) et [les attributions](../NOTICE.md).
