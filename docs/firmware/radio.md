# Couche radio IEEE 802.15.4

## Choix

Le driver `firmware/freertos/drivers/radio/rf_ieee.c` pilote directement le cœur radio du CC2652P par son interface de commandes (doorbell), décrite dans le TRM CC13x2/CC26x2 et les en-têtes BSD de TI (`rf_mailbox.h`, `rf_common_cmd.h`, `rf_ieee_cmd.h`). Il n'utilise **ni** `rf_multiMode_cc26x2.a`, **ni** les DPL, **ni** le Power driver. L'ordre de démarrage reproduit celui, observable dans `RFCC26X2_multiMode.c` (BSD) :

1. `AON_RTC_CTL.RTC_UPD_EN`, mot de boot `RFCBITS = 0xE0000011`, `RFCMODESEL = AUTO`.
2. Domaine `RFCORE` sous tension, horloges RFC, interruptions CPE remises à zéro.
3. Attente bornée de `BOOT_DONE`, application du patch CPE IEEE (`rf_patch_cpe_ieee_802_15_4`).
4. `CMD_BUS_REQUEST`, puis chaîne `CMD_RADIO_SETUP_PA → CMD_SYNC_START_RAT → CMD_FS`.
5. `RFCBITS |= 0x80`, configuration du commutateur d'antenne, interruptions CPE.

Les overrides sont ceux liés dans l'image installée 2026111802 (SmartRF Studio 2.32.0 via SysConfig) ; le PA 20 dBm est sélectionné comme le fait TI : `txPower = 0xFFFF`, gain dans `TX20_POWER_OVERRIDE`, segment Tx20 chaîné à la liste de base. La table de puissance −20…+20 dBm est celle du profil installé (entrées 6–13 dBm du patch Koenkk). Par défaut, le diagnostic utilise le PA standard ; la puissance est un paramètre de `DIAG_RADIO_START`.

## Fonctions

| Fonction | État |
|---|---|
| Mise sous tension, patch, réglage, synthé | Implémenté, **non exécuté** |
| Canaux 11–26 (`2405 + 5 (c − 11)` MHz) | Implémenté, testé (logique) |
| Réception continue `CMD_IEEE_RX`, 4 entrées circulaires, RSSI, corrélation, horodatage RAT | Implémenté ; analyse des éléments testée sur PC et ARM |
| CRC/FCS | Calculé et vérifié par le matériel ; trames CRC invalides éliminées (`bAutoFlushCrc`) et comptées |
| Filtrage d'adresses, ACK automatique, mode coordinateur | Implémenté (`frameFiltEn`, `autoAckEn`, `bPanCoord`) ; mode promiscuité pour le diagnostic |
| CSMA-CA (BE 3–5, 4 backoffs), TX, attente d'ACK 1 ms | Chaîne `CSMA → TX → RX_ACK` implémentée |
| Retransmissions MAC | Assurées par `mac_core` (3 essais) ; le driver rapporte `NO_ACK`/`BUSY` |
| Bit « frame pending » de l'ACK | Rapporté |
| Source matching pour enfants endormis | Implémenté : 24 entrées courtes (= quota indirect global) et 4 IEEE, `autoPendEn` limité aux *data requests*, mises à jour par `CMD_IEEE_MOD_SRC_MATCH` pendant la réception ; gestion des tables testée sur PC |
| Statistiques | RX, CRC, ignorées, saturation, redémarrages RX, TX ok/no-ack/busy/erreur, erreurs internes, perte de verrou synthé, durée max d'ISR |
| Débordement | Le RX de fond terminé est relancé et compté ; les entrées non lues sont refusées par le matériel et comptées |
| Contraintes temporelles | ACK matériel par le cœur RF ; le CPU n'est pas sur le chemin critique de l'ACK |

Toutes les attentes sont bornées (50 ms) et renvoient un code d'étape lisible par `DIAG_RADIO_STATS`.

## Ce qui reste à vérifier sur un dongle de laboratoire

1. `DIAG_RADIO_START` renvoie 0 ; `setup_failures = 0`, `synth_no_lock = 0`.
2. En mode promiscuité sur le canal d'un réseau de test, des trames apparaissent avec un RSSI plausible et `crc_ok`.
3. `DIAG_RADIO_TX` d'une *beacon request* (`03 08 seq FF FF FF FF 07`) provoque des balises reçues.
4. ~~Le PA 20 dBm~~ : **validé le 18 novembre (séance 10)**. `CMD_RADIO_SETUP_PA` refusait la configuration (`ERROR_PAR`), faute du code de région mémoire dans le lien de segment d'overrides TX20 ; une fois corrigé, 20 dBm sont appliqués et le LQI vu par un appareil réel augmente. Une mesure de puissance conduite reste à faire.

Points 1 et 2 **validés le 18 novembre** (séance r3) : démarrage du cœur RF sans échec, 807 trames reçues en 20 s sur le canal 15, toutes à CRC correct, RSSI médian −74 dBm. Point 3 **validé le 18 novembre** (séance r4) : 3 *beacon requests* émises, 25 à 31 balises reçues à chaque fois ; en mode coordinateur, 10 émissions sur 10 acquittées et *source matching* opérationnel. Point 4 (PA 20 dBm) validé en séance 10 (voir ci-dessus).

## Rust

Évalué et différé. Aucune chaîne Rust n'est installée dans l'environnement (ni `rustc`, ni `cargo`) ; le PAC `cc13x2_26x2` ne fournit aucun pilote radio ; le cœur du travail radio est la suite de commandes du CPE, déjà décrite par des en-têtes C BSD. Un avantage concret de Rust apparaîtrait dans les parseurs de trames et la gestion d'état MAC (absence de dépassements par construction). Conditions pour le reprendre : chaîne `thumbv7em-none-eabihf` épinglée, bibliothèque statique `no_std` sans allocateur, FFI C limitée à des tampons bornés, et démonstration que le code généré ne dépasse pas le budget RAM. Les codecs C actuels sont de toute façon couverts par des tests de fuzz bornés.
