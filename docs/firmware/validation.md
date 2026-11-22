# Validation et mesures

## Niveaux de preuve

| Niveau | Signification | Résultat au 22 novembre 2026 |
|---|---|---|
| Tests unitaires hôte | C11, x86-64, `-Werror`, UBSan en mode trap | **21 programmes réussis** : `mt` 53, `uart_noise` 7, `store_migration` 2,78 M, `radio_logic` 66, `wd_quorum` 13, `spsc_ring` 7 511, `mac_frame` 7 140, `mac_core` 634 369, `crypto` 29 712, `hal_aes` 11 737, `nwk_aps` 63 309, `nwk_security` 109 565, `nwk_join` 10,5 M, `join_e2e` 195, `nv_log` 4,8 M, `mesh` 491, `store_integrity` 62 733, `mesh_scale` 5 605, `sleepy` 2 382, `gp` 55, `reliability` 2 670 contrôles |
| Instructions ARM émulées | Modules portables compilés pour Cortex-M4F, exécutés dans Unicorn 2.1.3 | **68 vérifications réussies**, dont la réception et l'envoi ZCL et `AF_DATA_REQUEST` de bout en bout, avec la profondeur de pile de chaque chemin ; banc CPU (`tools/bench_freertos.py`) |
| Analyse statique des piles | `tools/stack_analysis.py` sur l'ELF, à chaque build | Marges de 312 à 640 o (radio : 464 o sur 5 120) ; le build échoue sous 256 o |
| Contrôles statiques d'image | Lien, carte mémoire, symboles requis/interdits, `tools/image.py check`, CCFG identique à 2026111802 | **Réussis** aux deux débits |
| Pile portable + vrai herdsman | Simulateur TCP (`firmware/freertos/sim/znp_sim.c`) et `ZStackAdapter` herdsman 11.2.1 | **19 étapes réussies** (`tools/znp_sim_acceptance.cjs`, aussi en CI) ; séance Touchlink complète (`tools/znp_sim_touchlink.cjs`) |
| Matériel | Dongle unique, sauvegarde et restauration automatiques | Démarrage, UART, FreeRTOS, watchdog, radio, arrivée d'un appareil réel, clé de lien unique, persistance après reset ; recette herdsman réussie sur le dongle ; lecture du diagnostic par les éléments NV virtuels avec la couche ZNP de herdsman ; voir [hardware-session-20261118.md](hardware-session-20261118.md) |
| Restauration d'une sauvegarde réseau | Sauvegarde Z2M de production restaurée par herdsman dans le firmware simulé | **Réussie** : voir [restauration.md](restauration.md) |
| Réseau de production | Migré le 18 novembre au soir | En service depuis ; image courante et mesures dans [status.md](status.md#production) et [benchmarks.md](benchmarks.md) |
| Recette matérielle à deux routeurs et un endormi | [hardware-campaign.md](hardware-campaign.md) | **À exécuter** sur un réseau d'essai isolé |

Les tests Python du dépôt passent : 54 tests, dont l'analyseur du store FreeRTOS et la garde de l'action `restore`.

## Flasher et restauration depuis une sauvegarde

Les tests JavaScript couvrent 28 scénarios : protocole ROM fragmenté, modèle incompatible, sauvegarde complète vérifiée, import JSON et BIN, refus d'une sauvegarde corrompue ou d'un autre dongle, format de stockage incompatible, interruption USB et erreur de relecture. Une restauration doit réécrire l'application sauvegardée tout en gardant les données réseau actuelles, même si celles de la sauvegarde sont différentes.

Le parcours dans Chrome a aussi été exercé avec un dongle ROM simulé, aux formats ordinateur et mobile : installation, sélection d'un fichier de sauvegarde, confirmation distincte, refus de mauvais SHA-256 et d'IEEE différent, restauration et conservation du stockage. Aucun port USB physique n'a été ouvert. Les rapports générés à chaque build font foi pour l'image publiée ; ces essais ne remplacent pas une validation matérielle.

## Mesures disponibles

| Mesure | Valeur | Nature |
|---|---|---|
| Flash utilisée | 103 896 octets (image `--hw-aes` 1 Mbaud, 22 novembre) | ELF lié |
| SRAM statique (pile MSP 2 Kio incluse) | 74 752 / 81 920 octets | ELF lié |
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
