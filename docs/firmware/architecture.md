# Architecture FreeRTOS native

L'application porte à l'adresse `0x100` un identifiant `ZBDP-FRT`, la version de cet en-tête, la version du format de stockage, le débit UART et la révision du build (entiers 32 bits little endian). Le build et le packaging vérifient ces valeurs ; le flasher s'en sert pour refuser une restauration incompatible. Toute évolution incompatible du stockage doit augmenter sa version et adapter le flasher.

## Arborescence

```text
firmware/freertos/
  config/FreeRTOSConfig.h      noyau : statique, préemptif, 1 kHz, stats DWT
  board/                       broches SONOFF, CCFG identique à la production
  startup/                     vecteurs, Reset_Handler, script de liaison
  runtime/                     memcpy/memset… (aucune libc, aucun malloc)
  hal/                         horloges, UART0 par interruptions, watchdog
  kernel/rtos_hooks.c          mémoire statique, débordement de pile, assert
  common/                      anneau SPSC, sérialisation bornée
  diagnostics/                 journal post-mortem, quorum watchdog
  drivers/radio/               driver RF IEEE 802.15.4 direct
  transport/znp/               MT, ZNP (interface de Z-Stack pour herdsman), diagnostic
  ieee802154/mac/              MAC : trames et coordinateur (files, indirect, association)
  zigbee/{security,nwk,aps}/   AES/CCM*/MMO, NWK, APS, coordinateur et Trust Center
  storage/                     journal nv_log (store sûr face aux coupures)
  sim/                         simulateurs hôtes : znp_sim (TCP), net_bench (débit)
  app/                         tâches
  tests/                       tests hôtes et ELF de test ARM
```

Les modules `common/`, `diagnostics/wd_quorum`, `drivers/radio/rf_logic`, `transport/`, `ieee802154/`, `zigbee/` et `firmware/src/` sont **portables** : compilés à l'identique pour le PC et pour le Cortex-M4F.

## Tâches et priorités

| Prio | Tâche | Pile (mots) | Rôle | Attente max. avant check-in |
|---|---|---|---|---|
| 5 | `radio` | 1 280 | Seul propriétaire du cœur RF et du coordinateur : MAC, NWK, sécurité, Trust Center, ZNP, Green Power, Touchlink ; événements CPE, chaînes TX | 200 ms (10 ms en mode coordinateur) |
| 3 | `host` | 512 | UART/MT, dispatch, réponses, remontée des trames radio | 200 ms (100 ms si trame en cours) |
| 1 | `superv` | 256 | Quorum watchdog, charge CPU | 250 ms |
| 0 | `IDLE` | 128 | `WFI` | — |

Toute la pile Zigbee et le traitement ZNP tournent dans `radio` ; `host` ne fait que le transport MT et le diagnostic. Il n'y a pas de tâche de timers logiciels (`configUSE_TIMERS = 0`). Le superviseur a volontairement la priorité applicative la plus basse : une tâche plus prioritaire qui monopolise le CPU l'empêche de pointer et déclenche le watchdog.

## Mémoire

- `configSUPPORT_DYNAMIC_ALLOCATION = 0` : aucun tas FreeRTOS n'est lié ; toutes les tâches, files et mutex sont statiques. Le build refuse `malloc`, `free`, `pvPortMalloc`.
- Mesures du build (`manifest.json` de l'image) : **106 840 octets de flash** et **75 272 octets de SRAM statique** sur 81 920 pour l'image `--hw-aes` à 1 Mbaud du 22 novembre 2026. Ces chiffres comprennent la pile MSP de 2 Kio et le coordinateur complet : MAC, 200 appareils, sécurité NWK pour 200 pairs, tables de routage.
- L'analyse statique des piles (`tools/stack_analysis.py`) est faite à chaque build ; le build échoue si une marge passe sous 256 o. Pile radio : 4 656 o au pire cas sur 5 120.

## Interruptions

| IRQ | Priorité | API FreeRTOS | Travail en ISR |
|---|---|---|---|
| UART0 | 0x40 | `vTaskNotifyGiveFromISR` | Vidage FIFO RX vers anneau, remplissage FIFO TX, comptage d'erreurs |
| RF CPE0/CPE1 | 0x20 | `xTaskNotifyIndexedFromISR` | Lecture/acquittement des drapeaux, aucune copie de trame |
| WDT | 0x20 | aucune | Enregistre les tâches manquantes puis reset |
| SysTick/PendSV | 0xE0 | noyau | — |

Chaque ISR mesure sa durée maximale en cycles DWT (exposée par les commandes de diagnostic).

## Sûreté de fonctionnement

- **Watchdog à quorum** : alimenté uniquement si `host`, `radio` et `superv` ont pointé depuis l'évaluation précédente ; délai de 2 s. Première échéance : interruption qui consigne le masque des tâches absentes, puis reset ; si l'ISR elle-même est bloquée, la seconde échéance réinitialise le MCU en matériel.
- **Journal post-mortem** en SRAM non initialisée (`.noinit`), validé par somme FNV-1a : assert, débordement de pile (`configCHECK_FOR_STACK_OVERFLOW=2`), HardFault (PC, LR, xPSR, CFSR, HFSR, MMFAR, BFAR), IRQ inattendue, watchdog, reset demandé. Sur cette carte, la SRAM ne survit pas à un reset à chaud (constaté sur matériel) : l'enregistrement est donc aussi émis sur l'UART avant le reset.
- Toutes les attentes matérielles (horloge HF, domaines d'alimentation, doorbell RF) sont bornées et renvoient un code d'étape.
- **Portes d'horloge par mode CPU** : chaque périphérique est activé en *run*, *sleep* et *deep sleep*. Sans les portes *sleep*, le `WFI` de la tâche idle coupait l'horloge de l'UART (constaté sur matériel).
- **Jalons de démarrage** (AREQ `5F 81`) et **enregistrement de plantage** (AREQ `5F 82`) émis en UART scrutée, indépendamment de FreeRTOS.
- **Accélérateur AES (`--hw-aes`)** : activé seulement après l'autotest de démarrage. Le matériel ne décide jamais seul : le premier bloc de chaque opération est recalculé en logiciel, une opération sur 32 à 95 (tirage pseudo-aléatoire) est entièrement recalculée, et toute trame rejetée par l'accélérateur est revérifiée en logiciel. Chaque attente du moteur et du DMA est bornée à 1 ms, suivie d'une remise à zéro et d'un rechargement de la clé. Le chiffrement en place part d'une copie de l'entrée, si bien qu'une reprise logicielle ne lit jamais une sortie modifiée. Un écart silencieux abandonne l'accélérateur, tout comme 8 fautes consécutives ; le logiciel seul sert alors jusqu'au redémarrage. Compteurs dans `DIAG 0x1B`.
- **Compteurs de trames sortants** : l'enregistrement de compteurs du store est un plancher chargé au montage, quelle que soit la clé. Une re-formation avec la même clé ne repart donc jamais de zéro. Le formatage de récupération (`DIAG 0x1A WIPE`) relit d'abord en lecture seule les versions valides de cet enregistrement dans les deux bancs (`nv_log_salvage`), puis réécrit le plancher. Sans plancher au montage, les compteurs écrits par l'hôte reçoivent 2^22 de plus. Une coupure pendant un changement de clé laisse des compteurs liés à la nouvelle clé : la restauration est alors refusée (`COUNTERS_OTHER_KEY`) plutôt que de réutiliser un compteur, et `restoreBackup` permet la récupération. Store illisible au démarrage : rien n'est effacé, et aucune trame sécurisée n'est émise. Seule la réécriture d'une ancienne Flash complète échappe à ces gardes : voir [restauration.md](restauration.md).

## Diagnostic sur l'UART

L'image parle ZNP à l'hôte (zigbee-herdsman, adaptateur `zstack`) : voir [znp-compat.md](znp-compat.md). Le transport MT est celui de Z-Stack (`FE LEN CMD0 CMD1 DATA FCS`). La tâche `host` sert elle-même le diagnostic, sans passer par la tâche radio.

| Accès | Contenu |
|---|---|
| Sous-système privé `0x1F` | `INFO` (version, révision de build, commit source et indicateur de modification, *uptime*, cause du dernier reset, dernier plantage, flash et SRAM), `TASK n`, `COUNTERS` (UART, MT, watchdog), `COORD_STATS`, `MESH_STATS`, `STORE_STATUS`, `PERF`, `EVENTS` (8 derniers événements réseau), `RELIABILITY`, `LINKS` (destinations qui perdent des réponses, premiers relais de leurs routes), `HOPS` (tentatives MAC et ACK manquants par voisin) ; commandes de laboratoire `RADIO_*`, `COORD_*`, `APS_SEND`, `STORE_FORMAT` |
| Éléments NV virtuels `0x0FA0` à `0x0FAA`, en lecture seule (`SYS osalNvRead`, `osalNvReadExt`, `osalNvLength`) | Les blocs `INFO`, `COUNTERS`, `COORD_STATS`, `MESH_STATS`, `STORE_STATUS`, `PERF`, `EVENTS`, `RELIABILITY`, `LINKS`, `HOPS` et `RADIO_STATS` (puissance d'émission, amplificateur 20 dBm, RSSI, erreurs du pilote radio), par une commande ZNP standard que herdsman sait envoyer : lisibles pendant que Zigbee2MQTT tourne, par sa propre file de requêtes. Toute écriture sur ces identifiants est refusée. |

Les outils : `tools/freertos_perf.py` (port série libre), `tools/z2m/diag_check.cjs` (même lecture que l'extension, port série libre) et l'extension Zigbee2MQTT [`tools/z2m/coordinator-diag.js`](../../tools/z2m/coordinator-diag.js). Le format exact des blocs est défini dans `app/host_task.c`.

Les compteurs vivent en RAM : un redémarrage du dongle les remet à zéro, y compris une coupure de l'alimentation USB quand on le débranche.

## Construction

```powershell
python tools/fetch_sources.py FreeRTOS-Kernel
python tools/build_freertos.py all
```

Sorties : `build/freertos/diag-115200/` et `build/freertos/diag-1000000/` (`sonoff-freertos-diag.elf`, `.hex`, `manifest.json`), `build/freertos/host/host-tests.json`, `build/freertos/arm-test/arm-tests.json`. Le compilateur est Zig 0.13 (Clang/LLD), déjà utilisé par le diagnostic historique ; TI Arm Clang reste une alternative non configurée pour cette cible. Les images Z-Stack de `build/` ne sont jamais réécrites.
