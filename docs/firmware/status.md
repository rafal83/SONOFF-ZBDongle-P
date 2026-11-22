# État par fonction : implémenté, testé en simulation, validé sur matériel

Quatre niveaux, jamais confondus :

- **Implémenté** : le code existe, compile sans avertissement (`-Werror`) pour l'hôte et pour le Cortex-M4F.
- **Testé en simulation** : exercé par les tests hôtes (C11, UBSan), l'émulation ARM (Unicorn), le simulateur ZNP avec le vrai herdsman 11.2.1, ou la répétition de migration. Aucune radio réelle n'intervient.
- **Validé sur matériel** : essai dédié sur le ZBDongle-P de production, résultat consigné dans [hardware-session-20261118.md](hardware-session-20261118.md) (séances 1 à 10).
- **Observé en production** : constaté dans le journal de Zigbee2MQTT sur le réseau réel, sans mesure dédiée.

## Production

Le réseau de production (Z2M 2.14.3, herdsman 11.2.0, 1 000 000 bauds) a été **migré sur FreeRTOS le 18 novembre au soir** par `restoreBackup`. Il a tourné le 19 novembre au matin sur l'image du commit `969fb13` : redémarrage de Z2M à 07:54 (stratégie `resumed`), puis **40 appareils entendus en 66 minutes et aucune erreur ni avertissement** dans le journal.

Défauts vus en production dans la nuit, avant `969fb13` :

| Défaut | Cause | Correction | Sur le dongle |
|---|---|---|---|
| Commandes Z2M en `NWK_NO_ROUTE`, états non remontés (lumière commandée par un bouton) | Commandes NWK sans IEEE source, ignorées par les routeurs réels ; *Link Status* trop long au-delà de 25 voisins, jamais émis | `969fb13` | **Oui** (image actuelle) |
| Sauvegarde Z2M en échec à l'arrêt (lecture des TCLK sans réponse) | Files ZNP qui refusaient au lieu d'attendre | `0d5987e` | **Oui** (image `1c92c11`, 19 novembre) |

**Le 19 novembre à 09:27**, la production est passée sur l'image **`--hw-aes` du commit `1c92c11`** (SHA-256 `068b1e5f…`), store conservé. Elle inclut `0d5987e` et les corrections de [stabilization-20261119.md](stabilization-20261119.md). L'image logicielle du même commit avait été programmée à 09:20 : Z2M l'avait reprise (`resumed`) et avait écrit sa sauvegarde à l'arrêt, ce qui n'arrivait plus avec `969fb13`. Z2M a repris l'image `--hw-aes` à 09:29 (`resumed`, 75 appareils, aucune erreur).

Relevés `DIAG` sur le dongle, avant le redémarrage de Z2M :

| Mesure | Image logicielle | Image `--hw-aes` |
|---|---|---|
| Bloc AES logiciel | 6 870 cycles (143 µs) | 6 863 cycles |
| Trame CCM\* (22 o d'AAD, 40 o de données) | 74 769 cycles (**1,56 ms**) | **21 609 cycles (0,45 ms)**, contrôle du premier bloc compris |
| Autotest AES | — | Réussi (`hw_aes_state = 1`) ; 16 opérations matérielles, 0 défaut, 0 désaccord, 0 délai dépassé |
| Store | `restored`, plancher présent | Idem |
| Charge CPU, réseau à l'arrêt | 0,4 % | 0,2 % (pic 1 %) |
| Pile libre (mots) : radio / hôte / superviseur / idle | 935 / 328 / 232 / 109 | 943 / 324 / 232 / 110 |

Ces piles sont relevées avant tout trafic radio : les valeurs sous charge restent à relever, Z2M arrêté.

**Le 20 novembre en fin d'après-midi**, la production est passée sur l'image **`--hw-aes` à 1 Mbaud du commit `666f804`** (SHA-256 `cf7814a2…`), store de disposition 2 (2 × 56 Kio) conservé.
Cette version intègre :
- Le moteur CCM\* natif allégé (1 119 cycles DWT en moyenne en trafic réel, ~40,6 µs par trame de 40 octets contre 1,56 ms en logiciel).
- La priorité absolue aux routes explicites (`nwk_source_route` et `nwk_table_route`) sur les voisins directs dans `next_hop()`, résolvant les liens radio asymétriques.
- La découverte de route avec temporisation d'attente (`disc_fresh_for`) pour laisser arriver les réponses multi-sauts.
- L'extension de `recent_tx` à 32 trames avec réémissions graduées et suppression du backoff `0xcd` sur les retours internes.
- La priorisation des `APS ACK` en tête de file directe MAC.

**Images suivantes (store conservé à chaque fois, capture complète avant chaque programmation dans `private/flash-*`) :**

| Date | Image | Contenu principal |
|---|---|---|
| 21 novembre, 07:24 | `d5f5fa1` | Touchlink, index compact des appareils, compteurs NWK entrants persistés |
| 21 novembre, 09:50 | `25538e6` | Planchers anti-rejeu v2 (empreinte IEEE de 32 bits), sécurité Touchlink, réponses de réassociation hors limite indirecte |
| 21 novembre, 10:30 | `62a7194` | Récupération radio bornée en fin de séance Touchlink, compteurs Touchlink dans le DIAG |
| 22 novembre, 08:30 | `62a7194` et modifications suivantes (image `a783dd45`) | Diagnostic lisible par éléments NV virtuels (extension Zigbee2MQTT), journal des événements réseau, compteurs de fiabilité, commit source dans `DIAG_INFO`, révision ZNP 20261122 |

Le 22 novembre, la lecture du diagnostic après redémarrage a restauré 32 planchers anti-rejeu au format v2 : première confirmation sur le matériel de cette persistance.

**Résultats mesurés sur le réseau réel (banc de production, 20 novembre) :**
- Rafales simultanées sur tous les routeurs joignables : **100 % de réussite (8 rafales sur 8 à 28/28, soit 224 / 224, 0 échec)**.
- Lecture séquentielle isolée : **médiane de 34,4 ms** (p95 à 60,4 ms).
- Piles observées sous charge : radio marge 420 mots, hôte marge 308 mots, superviseur 232 mots, idle 109 mots.
- Charge CPU pic sous rafale : 31,2 %, 0 erreur DMA/AES, intégrité complète du store conservée.

Diagnostic du « retard d'une lumière » (journal Z2M en debug, 19 novembre) :
- Le firmware répondait `NWK_NO_ROUTE` à la première commande après plusieurs minutes d'inactivité, puis herdsman réessayait 2 s plus tard. C'est corrigé par `7c24ecc`.
- **Cause principale, hors firmware** : Z2M avait reconfiguré une lumière le 19 novembre à 06:59 avec un intervalle minimal de *reporting* de 60 s (on/off et niveau). Ses changements faits au bouton remontaient donc jusqu'à une minute plus tard ; des changements rapprochés étaient fusionnés. Après reconfiguration du *reporting*, la synchronisation fonctionne.
- Deux routeurs ne répondent pas, avec `NWK_NO_ROUTE` après la découverte : ils ne sont plus entendus depuis le 19 septembre et le 2 novembre, soit bien avant la migration.

Ne jamais réécrire une ancienne capture complète après exploitation du réseau : voir [sauvegardes et restauration](restauration.md).

## Matrice

| Fonction | Implémenté | Testé en simulation | Validé sur matériel |
|---|---|---|---|
| Démarrage, horloges, UART MT 115 200 et 1 000 000 bauds, watchdog, FreeRTOS | Oui | — | **Oui** (séances 1 à 10 ; production à 1 Mbaud) |
| Radio 802.15.4 : RX, TX, CSMA, ACK, *source matching* | Oui | `test_radio_logic` | **Oui** |
| PA 20 dBm (overrides TX20, lien de segment avec code de région) | Oui | `test_radio_logic` | **Oui** (séance 10) |
| MAC coordinateur : balises, association, *data poll*, files indirectes | Oui | `test_mac_core` (603 937 vérifications) | **Oui** |
| MAC : `kick()` différé pendant les *callbacks* (récursion supprimée) | Oui (`d9297f9`) | `test_mac_core` (500 renvois en chaîne, profondeur 1) | Non |
| Réserve de tampons MAC pour le trafic direct (24, dont 16 indirects au plus) | Oui | `test_mac_core`, `test_sleepy` | Non |
| Trust Center : *Transport Key*, clé unique (graine + décalage), codes d'installation | Oui | `test_join_e2e`, `test_nwk_join` | **Oui** |
| Persistance : réseau, appareils, compteurs réservés avant usage | Oui | `test_join_e2e`, `test_store_integrity` | **Oui** (séances 7 à 10) |
| Compteurs v2 liés à la clé, restauration refusée si absents, malformés, d'une autre clé ou incohérents | Oui | `test_store_integrity` A | Non |
| **Plancher de compteurs** au montage, récupération des compteurs d'un store corrompu, marge de restauration | Oui (`677a0d8`) | `test_store_integrity` D, répétition de migration | Non |
| Montage strict du store, `DIAG 0x19` (format 2), `DIAG 0x1A WIPE` (plancher conservé) | Oui | `test_nv_log`, `test_store_integrity` | Non |
| Coupures électriques pendant écriture, compaction, réservation de compteurs | Oui | `test_nv_log` (858 coupures), `test_store_integrity` (770 coupures) | Non |
| ZNP (SYS, NV synthétisée, UTIL, ZDO, AF, APP_CNF) | Oui | Recette herdsman, 17 étapes (simulateur, aussi en CI) | **Oui** (recette sur le dongle ; production) |
| Files ZNP sans perte (attente plutôt que refus) | Oui (`0d5987e`) | Recette herdsman | **Observé en production** : sauvegarde Z2M écrite à l'arrêt (09:19 et 09:25) |
| `restoreBackup` herdsman (migration) | Oui | Répétition sur la sauvegarde de production | **Oui** : production migrée sans réappairage |
| Maillage : *Link Status* (fractionné), *many-to-one*, *Route Record*, routage par la source, commandes NWK avec IEEE | Oui | `test_mesh`, `test_mesh_scale` (80 routeurs) | **Observé en production** (appareils derrière des routeurs entendus et pilotés) ; pas de mesure dédiée |
| Réparation de routes : relais en échec, voisin retiré, *Network Status*, MTORR anticipé (différé s'il est limité), vieillissement | Oui | `test_mesh_scale` | Non |
| Congestion : échecs CSMA remontés sans casser les routes | Oui (`0d72557`) | `test_mesh_scale` phase 7 | Non |
| Saturation des files, tempête de broadcasts | Oui | `test_mesh_scale` phases 8 et 9 | Non |
| Redémarrage d'un routeur voisin détecté par *Link Status* | Oui (`0d72557`) | `test_mesh_scale` phase 10 (reprise en 1 s au lieu de 119 s) | Non |
| *Route Request*/*Reply*, relais unicast et broadcast avec *passive ACK*, conflits d'adresses | Oui | `test_mesh`, `test_mesh_scale` | Non |
| *Update Device* et tunnel APS | Oui | `test_mesh` | Non |
| Appareils endormis : *poll*, quotas, expiration, copies de broadcast, *End Device Timeout* | Oui | `test_sleepy` | Observé en production (capteurs sur pile entendus) ; pas de mesure dédiée |
| Endormis : 12 réponses de *poll* en file (réveils simultanés), 12 transactions IEEE (réassociations en masse après perte d'un parent), appareil qui nous interroge = notre enfant, capacité MAC apprise du Node Descriptor | Oui (20 novembre, soir) | `test_reliability`, banc `bench_throughput.py` (10 à 40 endormis, parent perdu, routeur congestionné) | Non |
| Enfants qui partent : vieillissement selon l'*End Device Timeout* demandé (256 min par défaut), enfant entendu via un routeur retiré, ZNP `assocRemove` / `assocAdd` servis (reprise de herdsman, auparavant erreur RPC) | Oui (20 novembre, soir) | `test_reliability`, banc (enfant qui change de parent sans réassociation : 95,0 → 99,75 % des lectures) | Non |
| Tables : 48 voisins (bitmap d'acquittement passif sur 64 bits) et 48 routes AODV au lieu de 32 ; `Mgmt_Rtg_rsp` produit sans copie des tables sur la pile | Oui (20 novembre, soir) | `test_reliability` (45 voisins, pagination de `Mgmt_Rtg`), `test_mesh`, `test_mesh_scale` | Non |
| Compteurs NWK entrants persistés tous les 1 024 (16 appareils par enregistrement, aucun octet de SRAM) : rejeu après redémarrage limité à cette fenêtre ; état anti-rejeu remis à zéro quand un appareil rejoint de zéro (avant : un appareil réappairé sans redémarrage du coordinateur voyait ses trames refusées) | Oui (20 novembre, soir) | `test_reliability` (redémarrage, rejeu refusé, réappairage), `test_store_integrity` | Non |
| SRAM : files ZNP de 4 trames au lieu de 8 (herdsman n'envoie qu'une requête à la fois ; banc à 115 200 bauds : 4 au plus en attente), file du renifleur à 4 : **3 384 o libres** au lieu de 832 | Oui (20 novembre, soir) | Banc de débit | Non |
| **Touchlink** (inter-PAN) : `interPanCtl`, `dataRequestExt` vers un autre PAN, trames reçues en `AF_INCOMING_MSG_EXT`, radio sur le canal Touchlink pendant la séance | Oui (21 novembre) | `test_reliability` (trames produites et reçues), `znp_sim_touchlink.cjs` (vrai herdsman 11.2.1 : scan, identify, reset, retour au canal) | Non |
| Planchers des compteurs NWK entrants, format v2 : empreinte IEEE de 32 bits (mélange des 64 bits) au lieu de 16 bits dont 1 forcé ; un plancher resté dans un emplacement repris par un autre appareil (effacement en échec) ne lui est plus appliqué. Les enregistrements v1 sont ignorés (planchers reconstruits aux trames suivantes) | Oui (21 novembre) | `test_reliability` (deux IEEE de même empreinte v1, contrôle positif), `test_store_integrity` | Non |
| Touchlink : fin de séance automatique après 30 s sans activité inter-PAN (radio rendue au canal du réseau si l'hôte disparaît) ; seules les trames ZLL (profil 0xC05E, cluster 0x1000) remontent, les autres trames inter-PAN, non authentifiées, sont ignorées ; échec signalé si la radio ne revient pas. **Récupération** si le retour au canal du réseau échoue : 6 réessais espacés de 0,1 à 3,2 s, puis redémarrage du cœur radio, puis redémarrage du firmware (le store garde le réseau), soit 13 s au plus sans écoute. Pas de nouvelle séance ni de déplacement Green Power pendant la récupération ; compteurs en fin du DIAG 0x1B (`freertos_perf.py`) | Oui (21 novembre) | `test_reliability` (retour au 3e essai ; jamais : 6 réessais, redémarrage radio, redémarrage), `znp_sim_touchlink.cjs` (vrai herdsman 11.2.1) | Non |
| Réponses de réassociation (indirectes, adressées par IEEE) hors de la limite des 16 trames indirectes de données, comme les réponses d'association : des données en attente ne font plus échouer une réassociation | Oui (21 novembre) | `test_mac_core`, `test_reliability` (16 trames en attente pour des enfants, 17e refusée, réponse de réassociation remise au *poll*), banc simulé | Non |
| Réappairage : le plancher persisté est effacé par l'association directe et par l'Update Device « nouvelle adhésion » relayé par un routeur, y compris après redémarrage | Oui (21 novembre) | `test_reliability` (deux redémarrages, compteurs repartis de zéro acceptés) | Non |
| Index des appareils : deux tableaux d'emplacements triés (IEEE, adresse courte) au lieu de deux index Patricia avec copie des clés : **8 656 o de SRAM libres** au lieu de 3 384, recherche en 8 étapes au plus | Oui (21 novembre) | `test_nwk_join` (30 000 opérations aléatoires contre une recherche linéaire), toute la suite | Non |
| *Handles* MAC uniques tant qu'une confirmation est attendue (plus de confirmation remise à la mauvaise requête) | Oui (20 novembre, soir) | `test_reliability` (600 trames pendant une attente indirecte) | Non |
| Réémission ZCL par lecture (destination + numéro ZCL), réponse en double filtrée, pas de réémission vers un endormi connu, route reconstruite seulement sans réponse récente, budget d'une réémission par seconde | Oui (20 novembre, soir) | `test_reliability`, banc de débit (réponses en double vers l'hôte 452 → 4 sur les 9 scénarios endormis, 21 novembre) | Non |
| Numéro de séquence NWK d'un broadcast jamais réutilisé tant qu'il est dans les tables anti-doublon ; *backoff* de découverte de 4 s (au lieu de 30 s), doublé jusqu'à 5 min | Oui (20 novembre, soir) | `test_reliability`, `test_mesh` | Non |
| CCM\* natif : revérification logicielle des rejets dans un budget (8, puis 1 toutes les 16 opérations réussies) | Oui (20 novembre, soir) | `test_reliability` (200 trames falsifiées : 11 revérifications ; panne silencieuse détectée en 144 opérations) | Non |
| Tâche radio : un seul *tick* de la pile par milliseconde | Oui (20 novembre, soir) | Banc de débit | Non |
| Accélérateur AES (`--hw-aes`) : attentes bornées, reprise logicielle, contrôle du premier bloc, audit | Oui (`935eb29`) | `test_crypto`, `test_hal_aes` (pilote réel contre un modèle de registres, 9 pannes) | **Partiel** : autotest et banc de démarrage réussis sur le dongle (×3,5) ; **en production depuis le 19 novembre à 09:29**, compteurs sous charge à relever. Chemins de panne non éprouvés sur le silicium |
| Analyse statique des piles (build bloquant sous 256 o de marge) | Oui (`d9297f9`) | Build | Relevés `DIAG 0x1B` à faire |
| Retour arrière `rollback` sans régression de compteur ; `restore` refusé après exploitation | Oui (`f69636a`) | Tests Python (store écrit par le code C) | **Non** : jamais exécuté |
| Mesures embarquées (`DIAG 0x1B`, `0x18`, `0x19` ; `freertos_perf.py --interval`) | Oui | — | Non |

## Mesures

| Mesure | Valeur | Nature |
|---|---|---|
| Bloc AES logiciel | 4 693 instructions | Émulation ARM (instructions, pas cycles) |
| CCM\* : 22 octets d'AAD, 40 octets de données | 50 954 instructions | Émulation ARM |
| Réception d'un rapport ZCL chiffré (MAC, NWK, APS, remontée) | 44 398 instructions | Émulation ARM |
| Envoi d'une commande ZCL | 35 912 instructions | Émulation ARM |
| `AF_DATA_REQUEST` ZNP jusqu'à la file MAC | 35 703 instructions | Émulation ARM |
| Pile : `AF_DATA_REQUEST` / réception jusqu'à ZNP / *Link Status* | 2 264 / 1 720 / 1 624 o | Émulation ARM |
| Pile radio, pire cas statique (logiciel / `--hw-aes`) | 4 344 / 4 656 o sur 5 120 | Analyse de l'ELF |
| Flash / SRAM statique (image 1 Mbaud, `d5f5fa1`, logiciel / `--hw-aes`) | 95 972 / 101 520 o ; 71 968 / 73 264 o sur 81 920 | ELF lié |

À 48 MHz, et à raison d'environ un cycle par instruction, une trame coûte de 0,7 à 1 ms, dont plus de 90 % en AES logiciel. Le gain de l'accélérateur, la latence radio, la charge CPU et les piles réelles seront mesurés par la recette [hardware-campaign.md](hardware-campaign.md), et par elle seule.
