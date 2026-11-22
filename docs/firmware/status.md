# État par fonction : implémenté, testé en simulation, validé sur matériel

Quatre niveaux, jamais confondus :

- **Implémenté** : le code existe, compile sans avertissement (`-Werror`) pour l'hôte et pour le Cortex-M4F.
- **Testé en simulation** : exercé par les tests hôtes (C11, UBSan), l'émulation ARM (Unicorn), le simulateur ZNP avec le vrai herdsman 11.2.1, ou la répétition de migration. Aucune radio réelle n'intervient.
- **Validé sur matériel** : essai dédié sur le ZBDongle-P de production, puis production sous FreeRTOS (voir ci-dessous).
- **Observé en production** : constaté dans le journal de Zigbee2MQTT sur le réseau réel, sans mesure dédiée.

## Production

Le réseau de production (Zigbee2MQTT 2.14.3, herdsman 11.2.0, 1 000 000 bauds, environ 130 appareils dont 28 routeurs) tourne sous FreeRTOS depuis le 18 novembre 2026. Il a été migré par `restoreBackup`, sans réappairage.

Image en service : `--hw-aes` à 1 Mbaud, SHA-256 `8ee183be…`, sources du commit `5194d96` (diagnostic par éléments NV virtuels, messages reçus longs), révision ZNP 20261122. Elle a été programmée le 22 novembre à 08:51. Le store (disposition 2, 2 × 56 Kio) est conservé à chaque programmation, avec une capture complète prise avant dans `private/flash-*`.

**Mesures sur le dongle :**

| Mesure | Valeur |
|---|---|
| Rafales simultanées sur tous les routeurs joignables (20 novembre) | 224 / 224 réussies (8 rafales de 28) |
| Lecture ZCL isolée | médiane 34,4 ms, p95 60,4 ms |
| CCM\* natif, trafic réel | 1 119 cycles en moyenne (environ 40 µs par trame de 40 octets, contre 1,56 ms en logiciel) |
| Autotest AES matériel | réussi (`hw_aes_state = 1`), aucun défaut, désaccord ni délai dépassé |
| Charge CPU | 0,2 à 0,4 % réseau calme ; pic de 31,2 % sous rafale |
| Marge de pile sous charge (mots) : radio / hôte / superviseur / idle | 420 / 308 / 232 / 109 |
| Store | `restored`, plancher de compteurs présent ; 32 planchers anti-rejeu v2 restaurés au redémarrage |

Ne jamais réécrire une ancienne capture complète après exploitation du réseau : voir [sauvegardes et restauration](restauration.md).

## Matrice

| Fonction | Implémenté | Testé en simulation | Validé sur matériel |
|---|---|---|---|
| Démarrage, horloges, UART MT 115 200 et 1 000 000 bauds, watchdog, FreeRTOS | Oui | — | **Oui** (production à 1 Mbaud) |
| Radio 802.15.4 : RX, TX, CSMA, ACK, *source matching* | Oui | `test_radio_logic` | **Oui** |
| PA 20 dBm (overrides TX20, lien de segment avec code de région) | Oui | `test_radio_logic` | **Oui** |
| MAC coordinateur : balises, association, *data poll*, files indirectes | Oui | `test_mac_core` (603 937 vérifications) | **Oui** |
| MAC : `kick()` différé pendant les *callbacks* (récursion supprimée) | Oui | `test_mac_core` (500 renvois en chaîne, profondeur 1) | Non |
| Réserve de tampons MAC pour le trafic direct (24, dont 16 indirects au plus) | Oui | `test_mac_core`, `test_sleepy` | Non |
| Trust Center : *Transport Key*, clé unique (graine + décalage), codes d'installation | Oui | `test_join_e2e`, `test_nwk_join` | **Oui** |
| Persistance : réseau, appareils, compteurs réservés avant usage | Oui | `test_join_e2e`, `test_store_integrity` | **Oui** (redémarrages en production sans réappairage) |
| Compteurs v2 liés à la clé, restauration refusée si absents, malformés, d'une autre clé ou incohérents | Oui | `test_store_integrity` A | Non |
| **Plancher de compteurs** au montage, récupération des compteurs d'un store corrompu, marge de restauration | Oui | `test_store_integrity` D, répétition de migration | Non |
| Montage strict du store, `DIAG 0x19` (format 2), `DIAG 0x1A WIPE` (plancher conservé) | Oui | `test_nv_log`, `test_store_integrity` | Non |
| Coupures électriques pendant écriture, compaction, réservation de compteurs | Oui | `test_nv_log` (858 coupures), `test_store_integrity` (770 coupures) | Non |
| ZNP (SYS, NV synthétisée, UTIL, ZDO, AF, APP_CNF) | Oui | Recette herdsman, 19 étapes (simulateur, aussi en CI) | **Oui** (recette sur le dongle ; production) |
| Files ZNP sans perte (attente plutôt que refus) | Oui | Recette herdsman | **Observé en production** : sauvegarde Z2M écrite à chaque arrêt |
| `restoreBackup` herdsman (migration) | Oui | Répétition sur la sauvegarde de production | **Oui** : production migrée sans réappairage |
| Maillage : *Link Status* (fractionné), *many-to-one*, *Route Record*, routage par la source, commandes NWK avec IEEE | Oui | `test_mesh`, `test_mesh_scale` (80 routeurs) | **Observé en production** (appareils derrière des routeurs entendus et pilotés) ; pas de mesure dédiée |
| Réparation de routes : relais en échec, voisin retiré, *Network Status*, MTORR anticipé (différé s'il est limité), vieillissement | Oui | `test_mesh_scale` | Non |
| Congestion : échecs CSMA remontés sans casser les routes | Oui | `test_mesh_scale` phase 7 | Non |
| Saturation des files, tempête de broadcasts | Oui | `test_mesh_scale` phases 8 et 9 | Non |
| Redémarrage d'un routeur voisin détecté par *Link Status* | Oui | `test_mesh_scale` phase 10 (reprise en 1 s au lieu de 119 s) | Non |
| *Route Request*/*Reply*, relais unicast et broadcast avec *passive ACK*, conflits d'adresses | Oui | `test_mesh`, `test_mesh_scale` | Non |
| *Update Device* et tunnel APS | Oui | `test_mesh` | Non |
| Appareils endormis : *poll*, quotas, expiration, copies de broadcast, *End Device Timeout* | Oui | `test_sleepy` | Observé en production (capteurs sur pile entendus) ; pas de mesure dédiée |
| Endormis : 12 réponses de *poll* en file (réveils simultanés), 12 transactions IEEE (réassociations en masse après perte d'un parent), appareil qui nous interroge = notre enfant, capacité MAC apprise du Node Descriptor | Oui | `test_reliability`, banc `bench_throughput.py` (10 à 40 endormis, parent perdu, routeur congestionné) | Non |
| Enfants qui partent : vieillissement selon l'*End Device Timeout* demandé (256 min par défaut), enfant entendu via un routeur retiré, ZNP `assocRemove` / `assocAdd` servis (reprise de herdsman, auparavant erreur RPC) | Oui | `test_reliability`, banc (enfant qui change de parent sans réassociation : 95,0 → 99,75 % des lectures) | Non |
| Tables : 48 voisins (bitmap d'acquittement passif sur 64 bits) et 48 routes AODV au lieu de 32 ; `Mgmt_Rtg_rsp` produit sans copie des tables sur la pile | Oui | `test_reliability` (45 voisins, pagination de `Mgmt_Rtg`), `test_mesh`, `test_mesh_scale` | Non |
| Compteurs NWK entrants persistés tous les 1 024 (16 appareils par enregistrement, aucun octet de SRAM) : rejeu après redémarrage limité à cette fenêtre ; état anti-rejeu remis à zéro quand un appareil rejoint de zéro (avant : un appareil réappairé sans redémarrage du coordinateur voyait ses trames refusées) | Oui | `test_reliability` (redémarrage, rejeu refusé, réappairage), `test_store_integrity` | Non |
| SRAM : files ZNP de 4 trames au lieu de 8 (herdsman n'envoie qu'une requête à la fois ; banc à 115 200 bauds : 4 au plus en attente), file du renifleur à 4 : **3 384 o libres** au lieu de 832 | Oui | Banc de débit | Non |
| **Touchlink** (inter-PAN) : `interPanCtl`, `dataRequestExt` vers un autre PAN, trames reçues en `AF_INCOMING_MSG_EXT`, radio sur le canal Touchlink pendant la séance | Oui | `test_reliability` (trames produites et reçues), `znp_sim_touchlink.cjs` (vrai herdsman 11.2.1 : scan, identify, reset, retour au canal) | Non |
| Planchers des compteurs NWK entrants, format v2 : empreinte IEEE de 32 bits (mélange des 64 bits) au lieu de 16 bits dont 1 forcé ; un plancher resté dans un emplacement repris par un autre appareil (effacement en échec) ne lui est plus appliqué. Les enregistrements v1 sont ignorés (planchers reconstruits aux trames suivantes) | Oui | `test_reliability` (deux IEEE de même empreinte v1, contrôle positif), `test_store_integrity` | Non |
| Touchlink : fin de séance automatique après 30 s sans activité inter-PAN (radio rendue au canal du réseau si l'hôte disparaît) ; seules les trames ZLL (profil 0xC05E, cluster 0x1000) remontent, les autres trames inter-PAN, non authentifiées, sont ignorées ; échec signalé si la radio ne revient pas. **Récupération** si le retour au canal du réseau échoue : 6 réessais espacés de 0,1 à 3,2 s, puis redémarrage du cœur radio, puis redémarrage du firmware (le store garde le réseau), soit 13 s au plus sans écoute. Pas de nouvelle séance ni de déplacement Green Power pendant la récupération ; compteurs en fin du DIAG 0x1B (`freertos_perf.py`) | Oui | `test_reliability` (retour au 3e essai ; jamais : 6 réessais, redémarrage radio, redémarrage), `znp_sim_touchlink.cjs` (vrai herdsman 11.2.1) | Non |
| Réponses de réassociation (indirectes, adressées par IEEE) hors de la limite des 16 trames indirectes de données, comme les réponses d'association : des données en attente ne font plus échouer une réassociation | Oui | `test_mac_core`, `test_reliability` (16 trames en attente pour des enfants, 17e refusée, réponse de réassociation remise au *poll*), banc simulé | Non |
| Réappairage : le plancher persisté est effacé par l'association directe et par l'Update Device « nouvelle adhésion » relayé par un routeur, y compris après redémarrage | Oui | `test_reliability` (deux redémarrages, compteurs repartis de zéro acceptés) | Non |
| Index des appareils : deux tableaux d'emplacements triés (IEEE, adresse courte) au lieu de deux index Patricia avec copie des clés : **8 656 o de SRAM libres** au lieu de 3 384, recherche en 8 étapes au plus | Oui | `test_nwk_join` (30 000 opérations aléatoires contre une recherche linéaire), toute la suite | Non |
| *Handles* MAC uniques tant qu'une confirmation est attendue (plus de confirmation remise à la mauvaise requête) | Oui | `test_reliability` (600 trames pendant une attente indirecte) | Non |
| Réémission ZCL par lecture (destination + numéro ZCL), réponse en double filtrée, pas de réémission vers un endormi connu, route reconstruite seulement sans réponse récente, budget d'une réémission par seconde | Oui | `test_reliability`, banc de débit (réponses en double vers l'hôte 452 → 4 sur les 9 scénarios endormis, 21 novembre) | Non |
| Numéro de séquence NWK d'un broadcast jamais réutilisé tant qu'il est dans les tables anti-doublon ; *backoff* de découverte de 4 s (au lieu de 30 s), doublé jusqu'à 5 min | Oui | `test_reliability`, `test_mesh` | Non |
| CCM\* natif : revérification logicielle des rejets dans un budget (8, puis 1 toutes les 16 opérations réussies) | Oui | `test_reliability` (200 trames falsifiées : 11 revérifications ; panne silencieuse détectée en 144 opérations) | Non |
| Tâche radio : un seul *tick* de la pile par milliseconde | Oui | Banc de débit | Non |
| Accélérateur AES (`--hw-aes`) : attentes bornées, reprise logicielle, contrôle du premier bloc, audit | Oui | `test_crypto`, `test_hal_aes` (pilote réel contre un modèle de registres, 9 pannes) | **Partiel** : autotest et banc de démarrage réussis sur le dongle (×3,5) ; **en production depuis le 19 novembre**. Chemins de panne non éprouvés sur le silicium |
| Analyse statique des piles (build bloquant sous 256 o de marge) | Oui | Build | **Oui** : piles relevées sous charge en production |
| Retour arrière `rollback` sans régression de compteur ; `restore` refusé après exploitation | Oui | Tests Python (store écrit par le code C) | **Non** : jamais exécuté |
| Mesures embarquées (`DIAG 0x1B`, `0x18`, `0x19` ; `freertos_perf.py --interval`) | Oui | — | **Oui** : lues en production par l'extension Zigbee2MQTT |
| *Parent_annce* (R21) : enfants terminaux annoncés aux routeurs après une restauration (9 par trame, 10 à 20 s après le démarrage), enfant libéré par une réponse dans les 60 s ; liste d'un routeur redémarré : enfants entendus dans les 5 dernières minutes renvoyés (*Parent_annce_rsp*) | Oui | `test_reliability` (17 enfants en 2 trames, réponse, libération, réponse tardive ignorée) | Non |
| Liens qui perdent des réponses (`DIAG 0x1E`, NV `0x0FA8`) : 8 destinations et 8 premiers relais, relais signalé quand au moins une lecture sur trois passée par lui a dû être réémise ; routage inchangé | Oui | `test_reliability`, banc (résultats identiques à la référence ; relais signalé dans les scénarios « relais à pertes » et « routeur congestionné ») | Non |
| Diagnostic dans Home Assistant : dernier redémarrage et plantage, voisins, enfants directs et endormis, file indirecte, trames sans ACK et échecs d'accès au canal (% sur les 30 dernières minutes, rien pendant les 10 minutes qui suivent un redémarrage du dongle), échecs d'authentification, rejeux, rejoins Trust Center, lectures sans réponse et relais signalés (avec les noms des appareils en attributs) | Oui | Maquette de l'extension sur des blocs lus du dongle | Non |

## Mesures

| Mesure | Valeur | Nature |
|---|---|---|
| Bloc AES logiciel | 4 693 instructions | Émulation ARM (instructions, pas cycles) |
| CCM\* : 22 octets d'AAD, 40 octets de données | 50 954 instructions | Émulation ARM |
| Réception d'un rapport ZCL chiffré (MAC, NWK, APS, remontée) | 44 398 instructions | Émulation ARM |
| Envoi d'une commande ZCL | 35 912 instructions | Émulation ARM |
| `AF_DATA_REQUEST` ZNP jusqu'à la file MAC | 35 703 instructions | Émulation ARM |
| Pile : `AF_DATA_REQUEST` / réception jusqu'à ZNP / *Link Status* | 2 264 / 1 720 / 1 624 o | Émulation ARM |
| Pile radio, pire cas statique (`--hw-aes`) | 4 656 o sur 5 120 | Analyse de l'ELF |
| Flash / SRAM statique (image `--hw-aes` à 1 Mbaud) | 106 840 o ; 75 272 o sur 81 920 | ELF lié |

Ces chiffres d'émulation comptent l'AES logiciel ; sur le dongle, le CCM\* natif ramène une trame à environ 1 100 cycles (voir Production et [benchmarks.md](benchmarks.md)). Le multi-sauts contrôlé, les endormis et l'endurance restent à qualifier par la recette [hardware-campaign.md](hardware-campaign.md).
