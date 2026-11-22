# Phase F — compatibilité ZNP (Zigbee2MQTT `zstack`)

Le firmware FreeRTOS expose l'interface MT/ZNP de Z-Stack attendue par zigbee-herdsman (adaptateur `zstack` de Zigbee2MQTT). Chaque réponse est produite à partir de l'état réel du coordinateur natif (`zigbee/nwk/zb_coord.c`) et de son stockage (`storage/nv_log.c`). Aucune réponse n'est fabriquée : une commande non prise en charge reçoit l'erreur MT RPC standard.

Code : [`transport/znp/znp.c`](../../firmware/freertos/transport/znp/znp.c). Dispositions binaires des structures NV : [`znp_structs.h`](../../firmware/freertos/transport/znp/znp_structs.h), **générées** par [`tools/gen_znp_structs.cjs`](../../tools/gen_znp_structs.cjs) à partir des classes `Structs` de herdsman 11.2.1. Les offsets sont mesurés en sérialisant chaque structure avec une valeur marqueur.

## Identification annoncée

`SYS_VERSION` annonce le produit **1 (Z-Stack 3.x.0)**, version 2.7.1, révision **20261122**. Ce numéro de produit choisit le **dialecte MT** que parle herdsman (commandes BDB, tables NV étendues). Il ne signifie pas une parité fonctionnelle avec Z-Stack : les écarts sont listés plus bas.

La révision ≥ 20210430 active la gestion de la LED côté herdsman ; `ledControl` répond par une erreur RPC, que herdsman intercepte. La révision ≥ 20201026 active `assocRemove`/`assocAdd` dans la récupération d'envoi de herdsman. Les trois commandes `assocGetWithAddress`, `assocRemove` et `assocAdd` sont servies. Avant le 20 novembre au soir, `assocRemove` renvoyait une erreur RPC : la commande en cours échouait au lieu d'être reprise.

## Commandes servies

| Famille | Commandes | Comportement |
|---|---|---|
| SYS | `resetReq`, `ping`, `version`, `getExtAddr` | Redémarrage réel du MCU ; `SYS_RESET_IND` émis après la remise en route de la pile |
| SYS NV | `osalNvItemInit/Read/ReadExt/Write/WriteExt/Delete/Length` | Petits items (`STARTUP_OPTION`, `PANID`, `CHANLIST`, `EXTENDED_PAN_ID`, `PRECFGKEY`, `ZNP_HAS_CONFIGURED`…) stockés tels quels dans `nv_log`. **Synthétisés depuis l'état réel** : `EXTADDR`, `NIB`, `NWK_ACTIVE/ALTERN_KEY_INFO`, `NWKKEY`, `TCLK_SEED`, table du gestionnaire de sécurité (item 76). Leur écriture est **appliquée** au coordinateur. Effacer `NIB` revient à quitter le réseau. Éléments `0x0FA0` à `0x0FA7` : diagnostic du firmware en lecture seule (voir [architecture.md](architecture.md#diagnostic-sur-luart)). |
| SYS NV étendue | `nvCreate/Length/Read/Write` (sys 1) | Tables à capacité fixe : gestionnaire d'adresses (1, 200 entrées = emplacements des appareils), TCLK (4, 200), clés APS (6, 16), matériel de sécurité NWK (7, 2). Lecture synthétisée, écriture appliquée (import des appareils, clés dérivées de la graine, compteurs relevés de façon durable). |
| SYS | `stackTune` (opération 0) | Puissance d'émission, appliquée immédiatement (redémarrage radio, bits *frame pending* réaffirmés) ; autres opérations : erreur RPC |
| UTIL | `getDeviceInfo`, `assocGetWithAddress`, `assocRemove`, `assocAdd` | État réel des enfants directs. `assocRemove` : l'appareil n'est plus traité comme notre enfant, il reste dans la table du Trust Center. `assocAdd` : enfant selon la relation Z-Stack 1 à 4. |
| APP_CNF | `bdbSetChannel`, `bdbStartCommissioning` (formation ; *steering* = ouverture 180 s), `bdbAddInstallCode` (code + CRC ou clé) | Formation depuis les items NV : premier canal du masque, PAN aléatoire si 0xFFFF, clé `PRECFGKEY` ou TRNG, graine TCLK tirée du TRNG |
| ZDO | `startupFromApp`, `extNwkInfo`, `extFindGroup`, `extAddGroup`, `extRemoveGroup`, `extRouteDisc`, `extUpdateNwkKey`, `extSwitchNwkKey`, `mgmtNwkUpdateReq` (changement de canal) | `extRouteDisc` émet une vraie *Route Request*. Rotation de la clé réseau : diffusion de la nouvelle clé, puis bascule. Changement de canal : diffusion, puis bascule 10 s plus tard. |
| ZDO requêtes | adresse NWK/IEEE, descripteurs nœud/simple/actifs, *match*, *server discovery*, *bind/unbind*, *mgmt LQI/Rtg/Bind/Leave/Permit join* | Coordinateur (0x0000) : réponse locale (LQI et routes réels) ; autres : trame ZDO envoyée, réponse remise en AREQ (format Z-Stack, y compris l'inversion *startIndex/numAssocDev*) |
| AF | `register`, `dataRequest`, `dataRequestExt` (16 bits, 64 bits, broadcast, groupe ; vers un autre PAN : inter-PAN), `interPanCtl` | Boucle locale vers 0x0000. Option `EN_SECURITY` = chiffrement APS avec la clé de lien du destinataire, refus explicite sans clé unique. Au-delà de 82 octets, fragmentation APS (230 octets au plus). **Messages reçus longs :** un message de plus de 233 octets (jusqu'à 512, APS fragmenté) est annoncé par `AF_INCOMING_MSG_EXT` avec sa longueur totale et sans données, puis lu par `AF_DATA_RETRIEVE` (horodatage, index, blocs de 240 octets ; longueur nulle = libération), comme avec Z-Stack. Green Power : proxy et TempMaster. **Touchlink** : `interPanCtl` déplace la radio sur le canal demandé (1), enregistre l'extrémité (2), la ramène sur le canal du réseau (0). Pendant cette séance, `dataRequestExt` vers un autre PAN part en inter-PAN depuis l'IEEE du coordinateur ; les trames inter-PAN reçues remontent en `AF_INCOMING_MSG_EXT` (source 64 bits, extrémité 0xFE, PAN source). Hors séance, rien n'est émis ni remonté. |
| AREQ émises | `SYS resetInd`, `ZDO stateChangeInd`, `tcDeviceInd`, `endDeviceAnnceInd`, `leaveInd`, `permitJoinInd`, réponses ZDO, `AF dataConfirm`, `AF incomingMsg`, `APP_CNF bdbComissioningNotification` | |

`dataConfirm` reflète la livraison MAC au premier saut. herdsman n'active jamais l'ACK APS (`options = 0`, sauf `EN_SECURITY`) : c'est aussi ce que fait Z-Stack dans ce cas.

## Non pris en charge (erreur RPC ou statut d'échec explicite)

| Fonction | Conséquence |
|---|---|
| Envoi de plus de 230 octets par l'hôte | Refusé (statut 0x02) : c'est la limite d'une trame MT |
| Message reçu de plus de 512 octets | Non remonté (compté) |
| `AF_DATA_REQUEST` avec `ACK_REQUEST` : réémission APS jusqu'à l'ACK | Le bit est transmis, sans réémission APS (herdsman ne le demande jamais : `options = 0`) |
| `mgmtNwkUpdateReq` : scan d'énergie, mise à jour du masque ou du gestionnaire | Statut d'échec, seul le changement de canal est servi |
| `ledControl` | Pas de LED pilotée ; herdsman l'intercepte |
| SAPI, BDB *finding & binding* | Statut d'échec ; herdsman n'envoie SAPI qu'à Z-Stack 1.2 |

Toutes les commandes ZNP que herdsman 11.2.1 envoie à un Z-Stack 3.x sont servies, sauf `ledControl` (tolérée par herdsman).

## Validation

| Niveau | Résultat |
|---|---|
| Simulateur hôte (`firmware/freertos/sim/znp_sim.c`, la pile portable du firmware en TCP) + **vrai** `ZStackAdapter` herdsman 11.2.1 ([`tools/znp_sim_acceptance.cjs`](../../tools/znp_sim_acceptance.cjs)) | **17 étapes réussies** : premier démarrage `reset` (formation), *permit join*, version, IEEE, paramètres réseau, descripteurs (extrémités 1 et 242), sauvegarde avec graine TCLK, reset, `resumed`, boucle AF, `restoreBackup` avec deux appareils (clé dérivée de la graine, clé explicite) **rendus à l'identique**, `resumed` |
| Dongle (COM8, 115200 bauds) avec le même script `--serial` | **17 étapes réussies**, redémarrages MCU réels |
| Dongle + `Controller` herdsman complet ([`tools/znp_pair_test.cjs`](../../tools/znp_pair_test.cjs)) | Ampoule TS0505A appairée, **interview complète**, 4 bascules acquittées, lecture `onOff` |
| Restauration de la **sauvegarde Z2M de production** en simulateur ([`tools/znp_migration_rehearsal.cjs`](../../tools/znp_migration_rehearsal.cjs)) | `restored` puis `resumed` ; IEEE, PAN, PAN étendu, canal, clé et séquence, graine TCLK identiques ; compteur NWK ≥ sauvegarde + 2 500 ; **130 appareils transmis identiques, 129 clés de lien et compteurs** ; voir [restauration.md](restauration.md) |

## Vérifications de reprise d'un réseau

| Condition | État |
|---|---|
| IEEE, PAN ID, PAN étendu, canal, clé réseau et séquence relus à l'identique | **Vérifié** sur la sauvegarde de production (répétition en simulateur) |
| Compteur NWK sortant strictement supérieur au dernier utilisé | **Vérifié** : sauvegarde + 2 500, réservé durablement avant le redémarrage |
| Clés de lien et compteurs APS repris | **Vérifié** : 129 sur 129 (graine + décalage), compteurs non décroissants |
| Appareils derrière des routeurs joignables | Routage maillé implémenté et testé en simulation (`test_mesh`) ; voisinage, *Link Status* et *many-to-one* observés sur le matériel avec un routeur réel ; **multi-sauts réel non éprouvé** (pas de second appareil) |
| Essai complet en laboratoire puis retour arrière vers Z-Stack | Fait sur le dongle unique : 7 séances avec restauration octet pour octet (voir [hardware-session-20261118.md](hardware-session-20261118.md)) |
| PA 20 dBm (`transmit_power: 20` en production) | **Validé sur le matériel** (séance 10, après correction du lien d'overrides TX20) |
| Image 1 000 000 bauds avec herdsman | **Validée sur le matériel** (séance 10) |
| Stockage et compteurs actuels pendant une restauration de firmware | Conservés par le [Web Flasher](restauration.md) ; une sauvegarde réseau native doit être traitée séparément |
