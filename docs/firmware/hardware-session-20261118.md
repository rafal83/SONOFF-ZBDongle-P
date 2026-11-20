# Essais matériels du 18 novembre 2026

Dongle : le seul ZBDongle-P disponible, coordinateur de production (IEEE `0x00124b00256c52ef`, COM8, série USB `C8C5…6278`). Zigbee2MQTT arrêté par l'utilisateur pendant les trois séances. Outil : `tools/freertos_lab_session.py`.

Chaque séance : deux captures complètes identiques de la Flash, programmation de la seule zone `0x00000–0x4FFFF`, essais, nouvelle capture prouvant que NV et CCFG n'ont pas changé, réécriture de l'application d'origine, relecture complète égale à la sauvegarde, puis vérification que Z-Stack 2026111802 répond à 1 Mbaud avec la bonne IEEE. **Les trois restaurations ont réussi.** Aucune émission radio, aucune écriture NV. Rapports et sauvegardes : `private/freertos-lab-20261118*/` (hors Git).

## Séance 1 — aucune réponse

Image programmée et relue correctement, aucune réponse MT en 15 s. Pas d'instrumentation : cause inconnue.

## Séance 2 — démarrage instrumenté

Jalons émis en UART scrutée : 1 (horloges, `init_status = 0` : XOSC HF 48 MHz actif), 2 (tâches créées), 3 (watchdog armé). Après le lancement de l'ordonnanceur, seul l'octet `FE` du `SYS_RESET_IND` part, puis la ligne reste à zéro. Ni plantage signalé ni redémarrage du watchdog : FreeRTOS tournait.

**Cause** : la tâche idle exécute `WFI` ; en mode *sleep*, le CC26x2 applique des portes d'horloge distinctes du mode *run*. Seules les portes *run* étaient ouvertes : l'UART perdait son horloge au premier sommeil. **Correctif** : portes *run*, *sleep* et *deep sleep* pour UART0, GPIO, TRNG et CRYPTO (`hal/hal_sys.c`).

## Séance 3 — validation

| Élément | Résultat sur le MCU |
|---|---|
| Démarrage, jalons 1–3, `SYS_RESET_IND` | OK |
| `SYS_PING` (capacité SYS), `SYS_VERSION` (produit 0xFE, 0.2.0), IEEE | OK |
| Commande NV Z-Stack (`SYS_OSAL_NV_LENGTH`) | Refusée par erreur RPC, comme prévu |
| 5 tâches ; marges de pile (mots libres) | host 343/512, radio 482/512, superv 231/256, Tmr Svc 234/256, IDLE 110/128 |
| 1 000 `SYS_PING` à 115 200 bauds | médiane 3,66 ms, p99 3,91 ms, max 4,23 ms (aller-retour PC compris) |
| Récupération après bruit, FCS faux, troncature | OK ; aucune erreur matérielle UART, aucun débordement |
| Durée max de l'ISR UART | 904 cycles (18,8 µs) |
| Watchdog à quorum | 40 alimentations en 10 s, 0 manque |
| Charge CPU | 0,1 % moyenne, pic 2,1 % |
| **Radio : démarrage du cœur RF** (patch CPE, `RADIO_SETUP_PA`, RAT, synthé) | **OK**, aucun échec, aucune perte de verrou |
| **Radio : réception passive canal 15, 20 s** | **807 trames** (635 data, 81 ACK, 91 commandes MAC), toutes à CRC correct, 29 trames CRC invalides éliminées par le matériel, RSSI médian −74 dBm, ISR CPE max 38 cycles |
| `SYS_RESET_REQ` | Reset réel, nouveau `SYS_RESET_IND` |
| Journal post-mortem en SRAM après reset à chaud | **Non conservé** : la SRAM n'est pas fiable comme stockage post-mortem sur ce MCU ; l'enregistrement est désormais aussi émis sur l'UART avant le reset |

La cause de reset lue après `SYS_RESET_REQ` vaut 7 (*warm reset*). La table de correspondance vers `SYS_RESET_IND` a été corrigée en conséquence.

## Séance 4 — émission et première arrivée d'un équipement réel

Autorisée explicitement par l'utilisateur (émission comprise). Image avec le coordinateur de laboratoire. Tous les essais de base repassent (ping médian 3,67 ms, 40 alimentations du watchdog en 10 s, 0 manque ; l'enregistrement de plantage est bien émis sur l'UART avant un reset).

| Essai | Résultat |
|---|---|
| Écoute passive canal 15, 20 s | 1 301 trames, toutes à CRC correct, RSSI médian −80 dBm |
| **A — 3 *beacon requests* sur le canal 15** (PA standard, 0 dBm) | **3 émissions réussies** (CSMA puis émission) ; **31, 26 et 25 balises reçues** en réponse, de 2 réseaux distincts |
| **B — coordinateur de laboratoire**, canal 25, PAN `0x6A5B`, arrivées ouvertes 180 s | Démarrage OK ; 3 balises émises ; 2 associations acceptées (la seconde est une réassociation du même appareil, même adresse) ; 2 réponses d'association servies sur *data poll* (le bit *frame pending* posé par le *source matching* matériel a fonctionné) ; **2 *Transport Key* acquittés** ; **3 *Device Announce* authentifiés** ; **1 appareil arrivé** (adresse `0x9790`, capacité `0x8E` : routeur alimenté) |
| Trafic de l'appareil après l'arrivée | **185 trames NWK chiffrées, toutes authentifiées** avec la clé réseau générée par le TRNG, 0 échec, 0 trame non chiffrée |
| Émissions du coordinateur | 10 tentatives, 10 réussies, 0 sans ACK, 0 canal occupé, aucune erreur interne |

Aucune donnée APS applicative n'a été remontée (`aps_data_up = 0`) : les trames reçues de l'appareil sont des commandes NWK et des échanges APS sécurisés (vraisemblablement la demande de clé de lien unique, *Request Key*, non implémentée), que le coordinateur ignore encore. L'appareil a donc rejoint le réseau, mais sa clé de lien unique n'a pas été établie ; il a ensuite été perdu avec ce réseau volatil et doit être réappairé à Zigbee2MQTT.

## Séances 5 et 6 — arrivée complète et pilotage ZCL d'un appareil réel

Image avec clé de lien unique, données APS, ACK APS et serveur ZDO (commit `bd2d362`). Coordinateur de laboratoire volatil, canal 25, PAN `0x6A5B`, arrivées ouvertes 240 s, puis 4 commandes ZCL *Toggle* envoyées par l'hôte à l'extrémité 1 du premier appareil arrivé.

**Séance 5** : aucune arrivée. Le coordinateur a reçu 15 trames chiffrées de l'appareil de la séance 4, qui se croyait toujours membre de l'ancien réseau de laboratoire (même canal, même PAN, ancienne clé) ; elles ont été rejetées. L'appareil devait être réinitialisé.

**Séance 6** (appareil réinitialisé) :

| Étape | Résultat |
|---|---|
| Association, *Transport Key* (clé réseau) | 1 acceptée, 1 clé envoyée et acquittée ; appareil `0x2697`, routeur alimenté |
| *Device Announce* | 3, authentifiées |
| **Clé de lien unique** (*Request Key* → *Transport Key* TC → *Verify Key* → *Confirm Key*) | **Établie à 4,8 s**, hachage de vérification correct, aucun échec |
| ZDO | 1 requête de l'appareil vers le coordinateur, 1 réponse envoyée |
| **4 *Toggle* ZCL envoyés par l'hôte** | **4 acceptés, 4 ACK APS reçus, 4 *Default Response* ZCL (commande 0x0B) remontées** ; **bascule de l'appareil confirmée visuellement par l'utilisateur** |
| ACK APS envoyés par le coordinateur | 5 |
| Trafic NWK reçu | 43 trames, toutes authentifiées ; 0 rejeu APS, 0 échec APS |
| Radio | 19 émissions : 16 acquittées du premier coup, 3 sans ACK récupérées par les retransmissions MAC (3) ; 0 canal occupé, 0 erreur |

Restauration de 2026111802 vérifiée dans les deux séances (relecture identique à la sauvegarde, NV et CCFG inchangés). L'appareil de test reste membre du réseau de laboratoire disparu : il doit être réinitialisé puis réappairé à Zigbee2MQTT.

## Séance 7 — persistance sur le matériel

Journal `nv_log` en Flash (`0x48000–0x4FFFF`, la NV de production n'est pas touchée). Arrivée d'un appareil réel (`0xACF5`), clé de lien unique établie et enregistrée, 2 *Toggle* acquittés ; puis **redémarrage réel du MCU** (`SYS_RESET_REQ`) et relance du coordinateur **depuis le stockage** (arrivées fermées) : réseau restauré, 1 appareil restauré avec sa clé unique, **2 nouveaux *Toggle* acquittés (ACK APS + *Default Response* ZCL) sans réappairage**, bascules confirmées visuellement. Aucun échec d'authentification NWK après redémarrage : le compteur de trames sortant a bien repris au-dessus de la borne réservée. Restauration de 2026111802 vérifiée.

## Séance 8 — recette ZNP avec le vrai herdsman (dongle laissé sous FreeRTOS)

À partir de cette séance, le dongle reste sous FreeRTOS à la demande de l'utilisateur (le réseau de production est hors ligne ; sa sauvegarde reste restaurable). La NV Z-Stack et le CCFG ont été vérifiés identiques à la sauvegarde de production avant chaque programmation.

- **Firmware ZNP, 115200 bauds** : le dongle répond produit 1, révision 20261118, capacités SYS/AF/ZDO/UTIL/APP_CNF.
- **`tools/znp_sim_acceptance.cjs --serial COM8`** (vrai `ZStackAdapter` herdsman 11.2.1) : **17 étapes réussies**. Dans l'ordre : formation (`reset`) ; sauvegarde (≈ 6 s, tables de 200 entrées) ; reset MCU réel puis `resumed` ; boucle AF ; `restoreBackup` avec 2 appareils (clé dérivée de la graine, clé explicite) restitués à l'identique ; `resumed`.

## Séance 9 — appairage par le `Controller` herdsman (Zigbee2MQTT sans MQTT)

`tools/znp_pair_test.cjs` : réseau de laboratoire PAN 0xC01B, canal 25.

| Étape | Résultat |
|---|---|
| Arrivée | Ampoule TS0505A (`_TZ3000_dbou1ap4`), adresse 0xBEC9 : `tcDeviceInd` puis annonce |
| Interview herdsman | **Réussie** : routeur secteur, extrémité 1 (Basic, Identify, Groups, Scenes, On/Off, Level, Color, Touchlink) et 242 (Green Power) |
| Pilotage | **4 *Toggle* acquittés**, lecture `onOff` = 1 |

## Séance 10 — routage, PA 20 dBm et 1 Mbaud (soir)

Le dongle était revenu sous Z-Stack de production. Avec l'accord de l'utilisateur, une nouvelle capture double a été faite dans la même session ROM (`private/production-backup-20261118-evening`), puis l'image FreeRTOS a été programmée.

| Essai | Résultat |
|---|---|
| Démarrage herdsman sur un store effacé par la production | Stratégie **`restored`** : herdsman a restauré la sauvegarde du réseau de laboratoire dans le firmware. L'ampoule est restée membre **sans réappairage**, ce qui est le chemin de la migration avec un appareil réel. |
| Maillage observé | L'ampoule (routeur) est **voisine** : 19 *Link Status* reçus, 20 émis, *Many-to-One Route Requests* émises, *Route Requests* de l'ampoule reçues |
| **Défaut trouvé : appareils importés traités comme endormis** | Une sauvegarde ne porte pas la capacité : les trames partaient en file indirecte et expiraient. **Corrigé** : capacité inconnue = récepteur actif, un voisin routeur est toujours servi en direct, un premier *poll* reclasse l'appareil en endormi (test de non-régression) |
| **Défaut trouvé : PA 20 dBm** | `CMD_RADIO_SETUP_PA` refusée (`ERROR_PAR` 0x0803) : le lien vers le segment d'overrides TX20 omettait le code de région mémoire de `NEW_OVERRIDE_SEGMENT`. **Corrigé**, test aligné sur la macro TI ; attente de coupure réelle du domaine radio ; repli sur le PA standard en cas d'échec, puissance réellement appliquée renvoyée à l'hôte |
| **PA 20 dBm** (`stackTune` 5 → 20 → 5 → 20 → 5) | **20 dBm appliqués**, 0 échec de configuration radio, 6/6 lectures à chaque palier ; le LQI du coordinateur vu par l'ampoule monte à 20 dBm (187 → 212, puis 205 → 225) |
| **Image 1 000 000 bauds** + `Controller` herdsman | Réseau repris (`resumed`), **4 bascules acquittées**, lecture `onOff` |

## Ce qui reste non validé sur matériel

Endurance (plusieurs jours), appareils endormis réels, plusieurs appareils simultanés, et le routage **multi-sauts** réel : un appareil derrière un routeur, avec Update Device et tunnel. Il faudrait pour cela un second appareil de test, éloigné du dongle.

## Fin de journée

À la demande de l'utilisateur, le dongle a été remis sous **Z-Stack de production 2026111802** depuis `private/production-backup-20261118-night`, relu octet pour octet. Les développements suivants n'ont été exécutés qu'en simulation : intégrité du store et des compteurs, réparation du maillage, appareils endormis, mesures et accélérateur AES. Voir [status.md](status.md).
