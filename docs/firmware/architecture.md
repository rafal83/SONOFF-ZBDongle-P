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
  transport/znp/               dispatcher MT
  ieee802154/mac/              codec de trames MAC
  zigbee/{security,nwk,aps}/   AES/CCM*/MMO, codecs NWK et APS
  app/                         tâches
  tests/                       tests hôtes et ELF de test ARM
```

Les modules `common/`, `diagnostics/wd_quorum`, `drivers/radio/rf_logic`, `transport/`, `ieee802154/`, `zigbee/` et `firmware/src/` sont **portables** : compilés à l'identique pour le PC et pour le Cortex-M4F.

## Tâches et priorités

| Prio | Tâche | Pile (mots) | Rôle | Attente max. avant check-in |
|---|---|---|---|---|
| 5 | `radio` | 768 | Seul propriétaire du cœur RF et du coordinateur (MAC, arrivée, sécurité NWK), événements CPE, chaînes TX | 200 ms (10 ms en mode coordinateur) |
| 4 | `Tmr Svc` | 256 | Démon des timers logiciels FreeRTOS | — |
| 3 | `host` | 512 | UART/MT, dispatch, réponses, remontée des trames radio | 200 ms (100 ms si trame en cours) |
| 1 | `superv` | 256 | Quorum watchdog, charge CPU | 250 ms |
| 0 | `IDLE` | 128 | `WFI` | — |

La pile Zigbee s'insérera entre `radio` et `host`. Le superviseur a volontairement la priorité applicative la plus basse : une tâche plus prioritaire qui monopolise le CPU l'empêche de pointer et déclenche le watchdog.

## Mémoire

- `configSUPPORT_DYNAMIC_ALLOCATION = 0` : aucun tas FreeRTOS n'est lié ; toutes les tâches, files et mutex sont statiques. Le build refuse `malloc`, `free`, `pvPortMalloc`.
- Mesures du build (`build/freertos/diag-*/manifest.json`) : **41 284 octets de Flash**, **41 392 octets de SRAM statique** sur 81 920 (pile MSP de 2 Kio et coordinateur complet compris : MAC, table de 200 équipements, sécurité NWK pour 200 pairs).
- Les plus gros objets : pool TX MT (2 096), file des trames radio (1 088), anneaux UART (2 × 1 024), piles des tâches.

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
- **Journal post-mortem** en SRAM non initialisée (`.noinit`), validé par somme FNV-1a : assert, débordement de pile (`configCHECK_FOR_STACK_OVERFLOW=2`), HardFault (PC, LR, xPSR, CFSR, HFSR, MMFAR, BFAR), IRQ inattendue, watchdog, reset demandé. La conservation de la SRAM au travers d'un reset logiciel est attendue sur CC26x2 mais **non vérifiée sur cette carte**.
- Toutes les attentes matérielles (horloge HF, domaines d'alimentation, doorbell RF) sont bornées et renvoient un code d'étape.
- **Portes d'horloge par mode CPU** : chaque périphérique est activé en *run*, *sleep* et *deep sleep*. Sans les portes *sleep*, le `WFI` de la tâche idle coupait l'horloge de l'UART (constaté sur matériel, séance r2).
- **Jalons de démarrage** (AREQ `5F 81`) et **enregistrement de plantage** (AREQ `5F 82`) émis en UART scrutée, indépendamment de FreeRTOS. La SRAM ne conserve pas le journal à travers un reset à chaud (constaté sur matériel).

## Diagnostic sur l'UART

Le transport MT reste celui de Z-Stack (`FE LEN CMD0 CMD1 DATA FCS`) avec le parseur et le pool existants. Commandes réellement implémentées :

| Commande | Réponse |
|---|---|
| `SYS_PING` (21 01) | capacités = **SYS uniquement** (0x0001) |
| `SYS_VERSION` (21 02) | transport 2, **produit 0xFE (expérimental)**, version 0.2.0, révision = date de build |
| `SYS_GET_EXTADDR` (21 04) | IEEE d'usine (FCFG1) |
| `SYS_RESET_REQ` (41 00) | reset réel après vidage de l'UART ; `SYS_RESET_IND` émis à chaque démarrage |
| Sous-système privé 0x1F | `INFO`, `TASK n`, `COUNTERS`, `RADIO_START/STOP/STATS/TX`, `COORD_START` (canal, puissance, PAN, secondes de *permit join*), `COORD_PERMIT`, `COORD_STATS`, trames reçues en AREQ 0x5F 0x80, arrivées en AREQ 0x5F 0x83 |
| Toute autre SREQ | erreur MT RPC standard (sous-système ou commande inconnue) |

Le produit 0xFE et la capacité SYS seule empêchent zigbee-herdsman de croire à un Z-Stack : il échouera au démarrage, comme attendu. Le format exact des réponses du sous-système 0x1F est défini dans `app/host_task.c`.

La radio reste **hors tension** au démarrage ; elle n'est mise en route que par `DIAG_RADIO_START` (réception brute) ou `DIAG_COORD_START` (**coordinateur de laboratoire volatil** : clé réseau tirée du TRNG et conditionnée par MMO, rien n'est écrit en Flash, tout est oublié au reset ou à l'arrêt). Les associations sont refusées tant que l'hôte n'ouvre pas le *permit join* (`DIAG_COORD_PERMIT`). Ainsi le démarrage, l'UART et le diagnostic ne dépendent pas du driver RF encore non validé.

## Construction

```powershell
python tools/fetch_sources.py FreeRTOS-Kernel
python tools/build_freertos.py all
```

Sorties : `build/freertos/diag-115200/` et `build/freertos/diag-1000000/` (`sonoff-freertos-diag.elf`, `.hex`, `manifest.json`), `build/freertos/host/host-tests.json`, `build/freertos/arm-test/arm-tests.json`. Le compilateur est Zig 0.13 (Clang/LLD), déjà utilisé par le diagnostic historique ; TI Arm Clang reste une alternative non configurée pour cette cible. Les images Z-Stack de `build/` ne sont jamais réécrites.
