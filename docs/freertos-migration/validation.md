# Phase G — validation et mesures

## Niveaux de preuve

| Niveau | Signification | Résultat au 19 novembre 2026 |
|---|---|---|
| Tests unitaires hôte | C11, x86-64, `-Werror`, UBSan en mode trap | **17 programmes réussis** : `mt` 42, `radio_logic` 66, `wd_quorum` 13, `spsc_ring` 7 511, `mac_frame` 7 140, `mac_core` 603 937, `crypto` 29 684, `hal_aes` 3 186, `nwk_aps` 63 309, `nwk_security` 109 565, `nwk_join` 27 161, `join_e2e` 192, `nv_log` 315 340, `mesh` 253, `store_integrity` 62 729, `mesh_scale` 6 776, `sleepy` 2 382 vérifications |
| Instructions ARM émulées | Modules portables compilés pour Cortex-M4F, exécutés dans Unicorn 2.1.3 | **68 vérifications réussies**, dont la réception et l'envoi ZCL et `AF_DATA_REQUEST` de bout en bout, avec la profondeur de pile de chaque chemin ; voir [status.md](status.md#mesures) |
| Analyse statique des piles | `tools/stack_analysis.py` sur l'ELF, à chaque build | Marges de 528 à 1 224 o ; le build échoue sous 256 o |
| Contrôles statiques d'image | Lien, carte mémoire, symboles requis/interdits, `tools/image.py check`, CCFG identique à 2026111802 | **Réussis** aux deux débits |
| Pile portable + vrai herdsman | Simulateur TCP (`firmware/freertos/sim/znp_sim.c`) et `ZStackAdapter` herdsman 11.2.1 | **17 étapes réussies** (`tools/znp_sim_acceptance.cjs`, aussi en CI) |
| Matériel | Dongle unique de l'utilisateur, sauvegarde et restauration automatiques | Démarrage, UART, FreeRTOS, watchdog, radio, arrivée d'un appareil réel, clé de lien unique, persistance après reset ; **recette herdsman de 17 étapes réussie sur le dongle** ; **appairage et pilotage d'une ampoule via le `Controller` herdsman** ; voir [hardware-session-20261118.md](hardware-session-20261118.md) |
| Répétition de migration | Sauvegarde Z2M de production restaurée par herdsman dans le firmware simulé | **Réussie** : voir [migration.md](migration.md) |
| Réseau de production | Migré le 18 novembre au soir avec l'accord du propriétaire | Image `969fb13` en service : voir [status.md](status.md#production) |
| Recette matérielle à deux routeurs et un endormi | [hardware-campaign.md](hardware-campaign.md) | **À exécuter** : séance à autoriser |

Les tests Python du dépôt passent : 54 tests, dont l'analyseur du store FreeRTOS et la garde de l'action `restore`.

## Mesures disponibles

| Mesure | Valeur | Nature |
|---|---|---|
| Flash utilisée | 77 884 octets (image 1 Mbaud) | ELF lié |
| SRAM statique (pile MSP 2 Kio incluse) | 66 800 / 81 920 octets | ELF lié |
| MAC : *beacon request* → émission de la balise | ≈ 1 800 instructions | Émulation |
| MAC : *data poll* → émission de la trame indirecte | ≈ 860 instructions | Émulation |
| AES-128 logiciel, un bloc | ≈ 4 700 instructions | Émulation, pas des cycles |
| Ouverture NWK ENC-MIC-32, 21 octets de charge | ≈ 42 600 instructions | Émulation |
| Analyse d'une balise Zigbee | ≈ 380 instructions | Émulation |
| Dérivation d'une clé depuis un code d'installation | ≈ 14 800 instructions | Émulation |

Les comparaisons A (Z-Stack d'origine) / B (optimisé installé) / C (FreeRTOS) demandées pour le temps de démarrage, la RAM libre en fonctionnement, la latence radio, la latence ZCL, la charge CPU et la stabilité exigent un firmware C fonctionnel sur matériel ; elles ne sont pas produites. Les données B existantes restent dans `docs/performance.md`.

## Procédure de premier essai sur un dongle de laboratoire

Matériel : un second ZBDongle-P **qui n'a jamais porté le réseau de production**. Ne jamais utiliser le dongle de production.

1. Sauvegarder la Flash complète du dongle de laboratoire (`cc2538-bsl -r`), noter son SHA-256.
2. Programmer `build/freertos/diag-115200/sonoff-freertos-diag.hex` avec vérification. Le HEX ne contient rien dans `0x50000–0x55FFF`.
3. Ouvrir le port à 115200 8N1 sans contrôle de flux. Attendu : `FE 06 41 80 xx 02 FE 00 02 00 cs` (`SYS_RESET_IND`).
4. `SYS_PING`, `SYS_VERSION`, `SYS_GET_EXTADDR`, puis `3F 00` (`DIAG_INFO`) : vérifier `init_status = 0`, la version FreeRTOS 11.1.0, l'absence de crash précédent.
5. `3F 02` toutes les secondes pendant 10 minutes : `feeds` croissant, `misses` stable, aucune erreur UART.
6. Envoyer `SYS_RESET_REQ` : nouveau `SYS_RESET_IND` avec raison 1.
7. Envoi soutenu de 10 000 `SYS_PING` à 115200 puis avec l'image 1 000 000 bauds : aucune trame perdue, compteurs d'erreurs nuls.
8. Étapes radio de [radio.md](radio.md#ce-qui-reste-à-vérifier-sur-un-dongle-de-laboratoire).
9. Émission : `--beacon-request-channel N` (essai A, 3 *beacon requests* standard, balises reçues comptées) et `--coord-channel N --coord-pan P [--coord-permit S]` (essai B, coordinateur de laboratoire volatil, compteurs et arrivées relevés).
10. Restaurer la Flash sauvegardée et vérifier son SHA-256.

`tools/freertos_lab_session.py` automatise ces étapes, y compris sauvegarde et restauration, pour le cas d'un dongle unique.

## Reproduire

```powershell
python tools/fetch_sources.py FreeRTOS-Kernel
python tools/gen_freertos_vectors.py      # optionnel : régénère les vecteurs
python tools/build_freertos.py all
python -m unittest discover -s tests
```
