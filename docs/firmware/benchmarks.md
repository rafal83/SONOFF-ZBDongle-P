# Performances : firmware FreeRTOS face à Z-Stack (Koenkk) et ZeroStack (SMLIGHT)

Dernière mise à jour : **21 novembre 2026**. Ce document est la référence chiffrée ; le [README](../../README.md#comparatif) en reprend le résumé. Il est mis à jour à chaque nouvelle mesure (voir [Mettre à jour](#mettre-à-jour)).

Trois sources, à ne pas confondre :

| Source | Ce qui est mesuré | Firmware FreeRTOS | Z-Stack Koenkk | ZeroStack |
|---|---|---|---|---|
| **Réseau réel** (dongle de production, 130 appareils) | lectures Zigbee de bout en bout | mesuré | **mesuré** (image 20250321, même dongle, même réseau) | non disponible |
| **Dongle, compteur DWT** | cycles CPU d'une trame CCM\* | mesuré | — | — |
| **Émulateur** (Unicorn, Cortex-M4F) | instructions des chemins de code | mesuré | — | — |
| **Documentation SMLIGHT** | leurs chiffres, sur CC1352P7 à 48 MHz | — | « Z-Stack stock » selon SMLIGHT | publié, **non reproduit** |

ZeroStack est un binaire fermé : il n'a été ni exécuté ni émulé ici.

## 1. Réseau réel

Banc en lecture seule `tools/znp_prod_bench.cjs`, dongle de production sur COM8, Zigbee2MQTT arrêté. Chaque routeur reçoit une lecture de l'attribut Basic ; les rafales envoient une lecture à chaque routeur en même temps, comme Zigbee2MQTT au démarrage. Liste blanche de commandes ZNP, aucune écriture vers les appareils.

| Image | Conditions | Lecture isolée, médiane / p95 | Rafales réussies | Part des routeurs joignables |
|---|---|---|---|---|
| Commit ffd4fa0, 21 novembre vers 7 h 30 : 1er passage, juste après le flash | 29 routeurs dont 1 adresse périmée (`0x2ebd` : ce routeur a changé d'adresse depuis la copie de la base utilisée, datée de la veille), 20 dBm, réessais herdsman, délai 10 s | 51,3 ms / 362 ms | 224 / 232 | 100 % (8 rafales sur 8 à 28 / 28) |
| Commit ffd4fa0, passages suivants (3, enchaînés) | idem | 39,0 à 46,1 ms / 94 à 106 ms | 203, 215 et 218 / 232 | 90,6 %, 96,0 % et 97,3 % |
| Essai écarté, 21 novembre vers 9 h 15 : ffd4fa0 + réémissions ZCL en direct vers les voisins | **dongle déplacé dans la maison : non comparable** ; base fraîche, 29 routeurs | 51 à 57 ms / 183 à 519 ms | 193, 198 et 192 / 232 | 83 à 85 % ; MAC sans ACK ×2 à ×3 : changement retiré, ffd4fa0 reflashé |
| **Commit 9e523a4** (priorité routes, recent_tx 32, reroutage) | 28 routeurs joignables, 20 dBm, réessais herdsman, délai 10 s | **34,4 ms / 60,4 ms** | **224 / 224** | **100 %** (8 rafales sur 8 à 28 / 28) |
| Commit a351549 (reroutage Network Status, CCM\* optimisé) | 29 routeurs, 20 dBm, réessais herdsman, délai 10 s | 68 ms / 168 ms | 223 / 232 | 96,1 % (3 rafales sur 8 à 29 / 29) |
| Commit 0e96866 (store 2 × 56 Kio, CCM\* natif allégé) | idem | 65 ms / 221 ms | 221 / 232 | 95,3 % (2 rafales sur 8 à 29 / 29) |
| Commit 9bbe52d (CCM\* natif) | idem | 78 ms / 226 ms | 207 / 232 | 89,2 % |
| Commit 4f547e6 | 32 routeurs dont 4 morts, 20 dBm, réessais, 10 s | 66 ms / 218 ms | 209 / 256 | 93,3 % (209 / 224) |
| Commit d57c7a6 (file MAC de 32) | 32 routeurs dont 4 morts, 5 dBm, sans réessai, 4 s | 74 ms / 125 ms | 103 / 160 | 73,6 % |
| Commit b12e6d9 (AES matériel par blocs) | idem | 43 ms / 87 ms | 71 / 160 | 50,7 % |
| Commit ed08b88 (AES logiciel) | idem | 74 ms / 95 ms | 68 / 160 | 48,6 % |
| **Z-Stack Koenkk 20250321** | 32 routeurs dont 4 morts, 115 200 bauds, **sans réessai, 4 s** | **51 ms / 127 ms** | 140 / 160 | **100 %** (28 / 28 à chaque rafale) |

Lecture :

- **Le coordinateur atteint 100 % de réussite en rafale sur tous les routeurs joignables (28 / 28 à chaque rafale, 224 / 224), égalant Z-Stack Koenkk tout en abaissant la médiane de lecture isolée à 34,4 ms.**
- **Les facteurs clés du résultat :**
  - Priorité des routes explicites (source routes / table routes) sur les voisins directs dans `next_hop()`, évitant les liaisons asymétriques (+20 dBm coordinateur contre faible puissance routeur).
  - Découverte de route avec temporisation d'attente (`disc_fresh_for`) pour laisser aux réponses multi-sauts le temps d'arriver avant repli.
  - Table `recent_tx` portée à 32 trames et réservée aux données applicatives avec réémission graduée en cas d'absence de réponse ZCL.
  - Priorisation des `APS ACK` en tête de file MAC directe (`direct_q`).
- **ffd4fa0 (21 novembre) est en retrait sur les rafales enchaînées :** 100 % au premier passage, puis 90,6 à 97,3 %. Les échecs sont des réponses jamais reçues, surtout sur les derniers routeurs de chaque rafale (`0x68d`, `0x5459`, `0x25a4`, `0x1331`, `0x5e32`) ; ces routeurs répondent en lecture isolée. Les réémissions par rafale sont du même ordre qu'avec 9e523a4 (6 à 18 contre 3 à 24). Les compteurs du dongle ne montrent rien d'anormal : 0 perte de file ZNP, 0 faute AES, pic CPU 35 %, 416 mots de pile radio libres. **Analyse (21 novembre après-midi) : la cause est radio, pas le firmware.**
  - **Au niveau MAC, les deux versions se valent :** 15 à 22 % de trames sans ACK, 84 à 106 réémissions par passage.
  - **Aucune réponse arrivée n'est jetée par le coordinateur :** 0 échec d'authentification NWK (aucune trame refusée comme rejeu), autant de doublons APS (14 à 22) et de trames remontées (262 à 318). Les réponses manquantes n'arrivent jamais.
  - **Les échecs suivent les relais :** sur 24 rafales, les routeurs joints via un moteur de volet échouent 9,2 % du temps, contre 2,7 % pour les autres. Quatre des cinq routeurs les plus touchés passent par le même volet ; ils sont aussi en fin de liste.
  - **Les deux mécanismes du firmware soupçonnés sont mis hors de cause en simulation** (suite `bursts`, section 5) : un budget de réémissions de 32 au lieu de 8, ou une route reconstruite à chaque deuxième lecture manquée, donnent les mêmes échecs au premier essai à ±1 près, pour trois relais défaillants sur quatre.
  - **L'écart avec 9e523a4** s'explique par des routes qui ne passaient pas par ce volet la veille.
- **La lecture isolée varie avec l'état du réseau,** de 43 à 78 ms selon la session. L'écart avec Koenkk (51 ms) est du même ordre que cette variation. Une comparaison serrée exige des sessions entrelacées.
- **La durée d'une rafale n'est pas comparable :** le délai d'attente est de 10 s pour le firmware FreeRTOS et de 4 s pour Koenkk.

**Par Zigbee2MQTT, en production (ffd4fa0, 21 novembre, 3 minutes après le démarrage) :** 5 rafales de lectures `get` (`state`, `position` ou `brightness`) envoyées en même temps aux 28 routeurs par MQTT, à 20 s d'intervalle. **Aucun échec dans le journal de Zigbee2MQTT pour ces 140 lectures.** 138 réponses sont publiées en moins de 12 s ; les 2 autres (deux routeurs) arrivent un peu plus tard. Médiane de 0,8 à 1,3 s par rafale, et 5 s pendant les reconfigurations de démarrage lancées par Zigbee2MQTT : la file de herdsman, qui garde 16 requêtes en vol, domine. Les seules erreurs du journal sur cette période concernent des lectures d'attributs Tuya et une reconfiguration lancées par Zigbee2MQTT, surtout vers des boutons endormis.

**Lectures en rafale par Zigbee2MQTT, image corrigée (`238aee53`, 21 novembre, 30 s après le démarrage) :** même protocole. 22 à 27 routeurs sur 28 publient en moins de 12 s, médiane de 0,6 à 2,0 s par rafale. **Aucun échec journalisé sur les 140 lectures** : les autres réponses arrivent plus tard. Non comparable au matin : le dongle est à un autre emplacement et le réseau sortait d'environ 50 minutes sans coordinateur, donc les routes se reconstruisaient. Six minutes plus tard, réseau stabilisé : 26 à 27 sur 28 en moins de 12 s, médiane de 0,9 à 1,1 s, toujours aucun échec journalisé. Le routeur qui manque le plus souvent (`0x3db6`, joint via des moteurs de volets) répond à chaque fois environ 14 s après la requête : la première tentative est perdue, et la reprise de herdsman passe.

**Lectures en rafale par Zigbee2MQTT, commit `2f8a9f2` (21 novembre, 10 h 31, 15 s après le démarrage) :** même protocole. Première série : 21, 28, 26, 28 puis **2** sur 28 en moins de 12 s. Lors de la rafale effondrée, Zigbee2MQTT n'a journalisé **aucun échec** et 21 routeurs n'ont rien publié en 70 s. Pourtant, juste après, trois lectures isolées vers des routeurs restés muets ont répondu en 0,1 à 1,3 s. Ces requêtes n'ont donc vraisemblablement jamais été exécutées par Zigbee2MQTT ; la cause n'est pas établie. Deuxième série, 2 minutes plus tard : 24 à 28 sur 28 par rafale, médiane de 0,9 à 1,7 s, aucun échec journalisé. L'effondrement ne s'est pas reproduit.

**Commandes en rafale par Zigbee2MQTT (ffd4fa0, 21 novembre) :** les 13 lumières à un seul canal sont inversées toutes ensemble, puis remises dans leur état d'origine, 2 cycles, soit 52 commandes. **2 échecs** (délai de 10 s dépassé), tous deux au premier cycle, sur deux routeurs (`0x3db6` et `0x68d`) : ce sont aussi les routeurs les plus souvent en échec au banc. Les remises en état du premier cycle ont attendu jusqu'à 13 s derrière ces délais. Médiane de 0,3 à 1,5 s par rafale. **À la fin, toutes les lumières sont dans leur état d'origine.**

Données brutes : `private/perf-20261120-prod-117e5ba/bench-*/bench.json` et `private/perf-20261121-d5f5fa1*/bench.json`, hors Git. Rafales MQTT : `private/z2m-burst-20261121/`.

## 2. CPU : trame CCM\* (sécurité NWK) mesurée sur le dongle

Cycles DWT pour une trame de 22 octets d'en-tête authentifié et 40 octets de charge, à 48 MHz.

| Implémentation | Cycles | Durée |
|---|---|---|
| AES logiciel | 74 769 | 1,56 ms |
| Moteur AES par blocs (DMA) et contrôle logiciel | 21 609 | 0,45 ms |
| CCM\* natif, 9bbe52d : la mesure incluait un contrôle logiciel | 8 984 | 0,19 ms |
| CCM\* natif, 0e96866 : banc de 16 trames, hors contrôle logiciel | 3 171 | 66 µs |
| **CCM\* natif, a351549** (en production) : banc de 16 trames, hors contrôle logiciel | **1 950** | **40,6 µs** |
| **CCM\* natif, a351549 en trafic réel**, contrôles logiciels amortis | **≈ 1 650** | ≈ 34 µs |
| ZeroStack (publié par SMLIGHT) | 3 824 à 3 968 | ≈ 80 µs |
| Z-Stack « stock » selon SMLIGHT (« NV vieillie ») | 532 182 | 11,1 ms |

**Pourquoi l'écart entre 8 984 et 3 171 cycles.** L'ancienne mesure enchaînait 16 opérations par blocs puis 16 natives, sur le même compteur de contrôle. La 32e opération déclenchait donc un recalcul logiciel complet, environ 75 000 cycles, compté dans la mesure native. Le nouveau banc fait une opération de chauffe et espace les contrôles. Le gain propre aux optimisations est donc modeste :
- la copie des données pendant le traitement de l'en-tête ;
- l'effacement des tampons par mots.

**Trafic réel**, après un banc de production de 2 077 opérations natives sous a351549 :

| Mesure | Valeur |
|---|---|
| Temps moyen passé dans le moteur | 1 119 cycles (contre 1 734 sous 0e96866) |
| — préparation (clé en cache, registres IV directs) | 546 cycles (-36 %) |
| — en-tête authentifié (AAD, mots 32-bit) | 164 cycles |
| — données | 235 cycles |
| — fin (SAVED_CONTEXT_RDY direct, lecture tag 32-bit, recopie mots) | 174 cycles (-60 %) |
| Contrôles logiciels | 15 sur 2 077, soit 1 sur 138 : environ 540 cycles amortis par trame |
| Écarts détectés | 0 |
| Erreurs DMA ou de clé | 0 |
| Trames repassées en logiciel | 0 |

Le reste des 3 171 cycles du banc vient de la couche de contrôle autour du moteur : verrou, vérification de MIC suspect et effacement.

Le mode natif garde des contrôles que ZeroStack ne décrit pas :
- la clé chargée dans le moteur est vérifiée toutes les 16 opérations ;
- une opération sur 64 à 191, tirée au hasard, est recalculée en logiciel ;
- chaque trame rejetée est revérifiée en logiciel.

Une trame à MIC falsifié coûte donc l'opération native plus une opération logicielle. **Résultat : un coût moyen du même ordre que les 3 824 à 3 968 cycles publiés par ZeroStack.** Leur protocole de mesure n'est pas publié ; ce n'est pas une comparaison à conditions identiques.

## 3. CPU : charges décrites par SMLIGHT, sur le code FreeRTOS émulé

Banc `tools/bench_freertos.py`, scénarios dans `firmware/freertos/tests/arm_perf.c`, résultats dans `build/freertos/arm-test/benchmarks.json`.

- **Conversion en cycles (estimation) :** le rapport mesuré sur le dongle vaut 1,464 cycle par instruction, pour le même code logiciel (bloc AES, trame CCM\*). Il est appliqué à 48 MHz.
- **Dans l'émulateur, l'AES est logiciel.** Les lignes qui déchiffrent surestiment donc le coût de l'image de production : voir la section 2 pour le CCM\* natif.
- **SMLIGHT ne publie ni la taille des trames, ni le nombre d'itérations, ni la version de Z-Stack comparée.** Les rapports ci-dessous sont des ordres de grandeur.

| Charge (définition SMLIGHT) | Firmware FreeRTOS | ZeroStack (publié) | Z-Stack « stock » (selon SMLIGHT) |
|---|---|---|---|
| Écho d'un broadcast relayé (50 routeurs, 1 000 échos) | **≈ 1 701 cycles (35 µs) par écho, total ≈ 35 ms** : doublon reconnu avant tout déchiffrement | 7 076 cycles (147 µs), total 151 ms | 143 865 cycles (3 ms), total 3 s |
| Trame rejouée | **≈ 1 458 cycles (30 µs)**, rejetée avant déchiffrement | 1 550 cycles | 143 802 cycles |
| En-tête de sécurité tronqué | **≈ 1 101 cycles (23 µs)** | 320 à 6 631 cycles | défaut distant ≈ 3 ms |
| MIC falsifié | ≈ 62 822 cycles en AES logiciel ; en production : CCM\* natif plus revérification logicielle | **4 760 cycles** | 143 058 cycles |
| Réception d'un rapport ZCL sécurisé (MAC, NWK, APS), 200 appareils | ≈ 65 740 cycles (1,37 ms) en AES logiciel | — | — |
| Appareil inconnu cherché dans une table pleine | **≈ 265 cycles (5,5 µs)**, 200 entrées (recherche dichotomique, 8 étapes au plus) | 326 cycles, 300 entrées | 1 486 228 cycles |
| Allocation mémoire | **sans objet : aucun tas**, tout est statique | 218 cycles | 2 550 cycles |
| Écriture identique de 100 octets, × 48 | ≈ 4 537 cycles (94,5 µs), 0 octet écrit | **3 770 cycles (78,5 µs)**, 0 octet | 100 714 cycles, 48 écritures |
| Écritures avant la 1re compaction (store vide) | **1 024** (écritures 44 o) / **512** (100 o) | 1 013 (store vide) | 69 |
| Écritures avant la 1re compaction (200 appareils vivants) | **756** (mises à jour réelles 48 o) / **405** (100 o) | non publié | non supporté |
| Coût d'une compaction (200 enregistrements vivants) | ≈ 19,5 ms de CPU et 7 effacements de secteur (durée réelle non mesurée) | non publié | non publié |
| Coupures de courant testées | **27 980**, 0 perte : 25 900 pendant des compactions, 858 sur le journal, 770 sur le store complet, 452 pendant la migration de disposition | 22 959, 0 perte | 1 conflit, récupération de 62 pages |
| Trames malformées | 0 plantage sur ≈ 208 millions d'entrées fuzzées (ASan + UBSan) | 0 plantage, 12 classes et 3 255 vecteurs | défaut distant |
| Bruit UART (64 octets collés à une commande) | 0 ping perdu, reprise en 40 ms | 0 ping perdu, reprise en 3 à 55 ms | 8,84 s d'interruption |

Ce qui reste en retrait de ZeroStack :
- **le rejet d'un MIC falsifié :** les rejets du moteur sont revérifiés en logiciel, dans un budget de 8 puis 1 toutes les 16 opérations réussies : c'est un choix de sûreté face aux pannes silencieuses ;
- **l'écriture identique (sans écriture flash) :** 4 537 cycles contre 3 770 cycles chez ZeroStack.

Ce qui dépasse désormais ZeroStack :
- **la trame rejouée :** 1 458 cycles contre 1 550 annoncés ;
- **l'écho de broadcast :** 1 701 cycles par écho (35 ms total) contre 7 076 cycles (151 ms) ;
- **l'endurance du store :** 1 024 écritures avant compaction sur store vide contre 1 013 publiées par ZeroStack.

## 4. Capacités et mémoire

| | Firmware FreeRTOS | Z-Stack (Koenkk, `firmware.patch`) | ZeroStack (publié) |
|---|---|---|---|
| Appareils sécurisés (Trust Center) | 200 | 200 (`ZDSECMGR_TC_DEVICE_MAX`) | 200 sur CC2652P |
| Voisins | 48 (32 avant le 20 novembre au soir) | 50 | non publié |
| Routes AODV | 48 (32 avant) | 150 | non publié |
| Routes sources | 200 | 250 | « 200+ » |
| Table anti-doublon des broadcasts | 32 | 30 (`MAX_BCAST`) | 40 |
| Réessais MAC | 3 par tour, 3 tours (file de 32 trames) | 7 | non publié |
| Store non volatil | **2 × 56 Kio** (disposition 2) | NV Z-Stack (géométrie non comparée ici) | non publié |
| Flash occupée | 102 032 o (image `--hw-aes` 1 Mbaud de l'arbre de travail du 21 novembre, non flashée ; 96 516 o en AES logiciel ; en production, 316ec3c : 101 776 o) | 178 669 o (20240710) | non publié |
| SRAM | 73 328 o statiques sur 81 920 (8 592 libres), piles et MSP comprises, **sans tas** | 42 228 o statiques + tas dynamique de 33 371 o | 88 Ko de SRAM sur CC2652P, sans précision |

## 5. Débit soutenu et appareils endormis : SIMULATION

**Ce ne sont pas des mesures physiques.** Banc `tools/bench_throughput.py`, qui pilote `firmware/freertos/sim/net_bench.c` : la vraie pile portable (ZNP, `zb_coord`, MAC, sécurité NWK) tourne sur le PC dans une simulation à événements discrets, à la microseconde. Aucun port série, aucune radio.

Ce qui est modélisé :
- **Hôte :** modèle de zigbee-herdsman 11 (adaptateur Z-Stack 3.x). 16 transactions au plus en même temps, chaque créneau étant tenu jusqu'à la réponse ZCL. Une seule SREQ à la fois sur l'UART. Attente de 10 s de la réponse. Reprises de herdsman : 2 s de pause après un échec, 5 tentatives au plus, une nouvelle tentative après une réponse manquante.
- **Liaison série :** UART à 1 Mbaud, ou 115 200 bauds pour une série. Files ZNP de 4 trames, comme le firmware ; la tâche radio se bloque quand la file de sortie est pleine, comme `znp_send()`.
- **Tâche radio :** un seul CPU, avec l'ordre des étapes de `radio_task.c`. Chaque étape coûte les instructions mesurées sur le code ARM compilé (`tools/bench_transaction_cpu.py`, CCM\* exclu) × 1,464 cycle par instruction. S'y ajoute chaque opération CCM\* : 1 650 cycles en natif, moyenne mesurée en production.
- **Cœur radio :** 4 tampons de réception, ACK matériel et bit *frame pending* issu de la table de *source match* : adresses courtes, et autant d'entrées IEEE que le firmware simulé (12 ; 4 pour la référence).
- **Canal 802.15.4 :** CSMA-CA non slotté, ACK, collisions entre toutes les émissions qui se chevauchent, 1 % de pertes aléatoires. Par défaut, un seul domaine de collision (cas défavorable). L'option de réutilisation spatiale sépare les sous-arbres.
- **Routeurs :** 28 au total, 16 à 1 saut, 9 à 2 sauts et 3 à 3 sauts. Chacun a sa file, ses réessais MAC et NWK et sa sécurité refaite à chaque saut. Il envoie un Route Record après chaque Many-to-One, renvoie ses broadcasts deux fois et répond aux lectures en 2 à 10 ms. Il garde les trames de ses enfants endormis.
- **Appareils endormis :** *poll* toutes les 2 s, avec attente de la trame annoncée et *polls* rapides ensuite. Après une perte de parent, ils se réassocient par le coordinateur.

Médiane de 3 graines × 30 s par point. « Avant » = sources du commit `cd6f3ae` ; « après » = arbre de travail du 21 novembre (ffd4fa0 et les corrections de sécurité, Touchlink et MAC de l'après-midi : identique à ffd4fa0 sur tous les scénarios sauf « parent perdu », 98,25 → 98,5 % et 6,2 → 4,8 s). Les deux tournent sur la même version du simulateur. Résultats bruts : `summary.json` et `runs/` dans le dossier `--out`.

Le 21 novembre, le modèle de *source match* est passé de 8 entrées IEEE fixes au nombre d'entrées du firmware. Avec 8 entrées, au-delà de 8 appareils en attente d'une réponse de réassociation, le simulateur n'annonçait plus la trame alors que le firmware l'aurait fait : le scénario « parent perdu » de l'après tombait à 96,5 %. Les chiffres des endormis publiés avant cette date ne sont pas comparables.

### Transactions complètes

Une transaction est une lecture d'attribut ZCL, de l'`AF_DATA_REQUEST` à la réponse remontée à l'hôte.

| Débit offert | Réussite avant → après | Obtenu (/s) | Service p50 / p95 / p99 avant | après | Réessais herdsman avant → après |
|---|---|---|---|---|---|
| 25 /s | 100 → 100 % | 24,9 | 18 / 38 / 59 ms | 18 / 37 / 58 ms | 0 → 0 |
| 50 /s | 100 → 100 % | 50,0 | 29 / 124 / 190 ms | 29 / 119 / 178 ms | 11 → 1 |
| 100 /s | 99,97 → 99,87 % | 99,9 → 99,8 | 54 / 192 / 10 127 ms | 70 / 225 / 10 088 ms | 51 → 42 |
| 150 /s | 93,3 → 91,9 % | 139,8 → 137,9 | 52 / 177 / 10 101 ms | 70 / 223 / 4 621 ms | — |
| 180 /s | 77,3 → 76,6 % | 139,2 → 137,9 | 56 / 173 / 10 086 ms | 72 / 224 / 8 342 ms | — |

Le **débit maximal stable** est le plus haut débit qui garde au moins 99,9 % de réussite et un p95 de bout en bout de 1 s au plus. Il est cherché par dichotomie, à ±6 /s près. Dans l'arbre, il varie de 45 à 56 /s pour quelques instructions de plus ou de moins sur le chemin d'émission (même firmware, coûts CPU décalés de 3 instructions) : c'est une plage, pas une valeur au /s près.

| Conditions | Avant | Après | p99 de service au maximum (après) |
|---|---|---|---|
| Arbre de 28 routeurs, un domaine de collision | 56 /s | 56 /s | 1,2 s (avant : 10 s) |
| 28 routeurs à 1 saut | 103 /s | 103 /s | 316 ms (avant : 241 ms) |
| Arbre, réutilisation spatiale | 42 /s | 45 /s | 1,1 s (avant : 140 ms) |
| Arbre, UART à 115 200 bauds | 59 /s | 59 /s | 10 s (avant : 10 s) |

Lecture :
- **La limite n'est ni le CPU ni les files du coordinateur.** La tâche radio reste sous 3,2 % de CPU à 180 /s. La file MAC ne dépasse pas 17 / 32 trames, les tampons RX 1 / 4, la file de sortie ZNP 2 / 4. Les limites sont l'occupation du canal (≈ 67 % à 138 /s dans un seul domaine) et la **fenêtre de 16 transactions de herdsman**. Chaque réponse perdue y bloque un créneau 10 s : au-delà de ≈ 60 /s, la latence de bout en bout passe à plusieurs secondes, alors que le temps de service reste sous 250 ms au p95.
- **Les corrections ne changent pas le débit maximal**, à la précision près. Elles réduisent les réessais herdsman (11 → 1 à 50 /s) et la queue de latence au débit maximal dans l'arbre (p99 de 10 s à 1,2 s). La médiane augmente un peu au-delà de 50 /s : les lectures récupérées ajoutent des trames. Les réémissions sont limitées à une par seconde, avec des rafales de 8. Sans cette limite, le débit servi à 180 /s tombait à 122 /s.

### Rafales comme le banc réel

Suite `bursts` : une lecture vers chacun des 28 routeurs, toutes ensemble, toutes les 12 s, pendant 120 s, sans fenêtre de 16 (comme `znp_prod_bench.cjs`). Les échecs au premier essai correspondent à ce que compte le banc réel.

| Conditions | Lectures | Échecs au 1er essai | Réussite finale | Bout en bout p95 |
|---|---|---|---|---|
| Aucun relais défaillant | 280 | 0 à 1 | 100 % | 0,6 à 1,1 s |
| Un relais à 1 saut perd 30 % de ce qu'il relaie | 280 | 3 à 8 | 99,6 à 100 % | 1,1 s |

Sur 6 graines × 280 lectures, un relais défaillant donne 22 à 107 échecs au premier essai selon sa position. Les deux variantes du firmware testées n'y changent rien (voir section 1).

### Messages entrants

Ce sont des rapports d'attributs spontanés : l'essentiel du trafic d'un réseau Zigbee2MQTT, et le sens habituel de « messages par seconde ».

| Rapports offerts | Arbre : livrés, latence p99 | 1 saut : livrés, latence p99 | CPU (1 saut) |
|---|---|---|---|
| 50 /s | 100 %, 24 ms | 100 %, 7 ms | 1,3 % |
| 100 /s | 99,97 %, 58 ms | 100 %, 8 ms | 1,9 % |
| 150 /s | 72,4 % (canal saturé à 83 %) | 100 %, 16 ms | 2,6 % |
| **180 /s** | 51,1 % (canal à 98 %) | **99,80 %, 54 ms** | 3,1 % |
| 250 /s | 29 % | 56,4 % (canal saturé) | 2,7 % |

**Plafond**, mesuré par pas de 10 /s avec 3 graines :

| Topologie | Débit soutenu (≥ 99,8 % livrés) | Débit livré au maximum | Au-delà |
|---|---|---|---|
| 28 routeurs à 1 saut | **190 /s** (99,81 à 99,95 %, p99 55 à 61 ms) | ≈ 197 /s (200 offerts, 98,1 à 98,7 % livrés) | Effondrement par collisions : ≈ 150 /s livrés à 240 offerts |
| Arbre à 3 niveaux | ≈ 110 /s (99,3 à 99,8 %) | ≈ 123 /s (130 offerts) | ≈ 114 /s livrés à 140 offerts |

Une trame reçue coûte ≈ 7 100 cycles au coordinateur, CCM\* natif compris (5 474 cycles de pile et 1 650 de CCM\*). **Le CPU suffirait donc pour ≈ 6 700 messages/s : à 180 /s, la limite est l'air.** Les messages relayés occupent le canal à chaque saut.

### Comparaison avec ZeroStack

Les **180 messages/s** de SMLIGHT sont annoncés sans topologie, sans taille de trame et sans dire s'il s'agit de messages entrants ou de transactions. Ici, en simulation, le firmware FreeRTOS :
- livre 180 à 190 rapports/s à 99,8 % quand les 28 routeurs sont à 1 saut ;
- plafonne entre 100 et 150 rapports/s dans un arbre à 3 niveaux sur un seul domaine de collision ;
- sert ≈ 138 transactions complètes/s au plus, et 56 à 103 /s en régime stable, car le modèle herdsman ne garde que 16 transactions en vol.

Ces chiffres ne sont pas comparables à ceux de SMLIGHT à conditions égales.

### Appareils endormis

Scénarios : 20 transactions/s, dont 30 % vers les appareils endormis (50 % pour les deux derniers), la moitié enfants du coordinateur et l'autre derrière des routeurs.

| Scénario | Réussite avant → après | Bout en bout p50 avant → après | Réponses en double vers l'hôte avant → après |
|---|---|---|---|
| 10 endormis, réveils étalés | 99,8 → 99,8 % | 37 → 48 ms | 31 → 0 |
| 20 endormis, réveils simultanés | 99,8 → 100 % | 4,0 → 3,9 s | 66 → 1 |
| 40 endormis, réveils simultanés | 100 → 100 % | 5,1 → 6,5 s | 70 → 1 |
| 20 endormis derrière un routeur congestionné (file de 2, 4 trames indirectes) | 69,7 → 74,5 % | — | 24 → 0 |
| 10 endormis, un enfant passe derrière un routeur sans réassociation entendue (herdsman : `assocRemove`) | 95,0 → 99,75 % | — | 40 → 0 |
| 20 endormis, parent perdu à 8 s, réassociation | 97,5 → 98,5 % | 7,1 → 4,8 s | 53 → 1 |

Les réponses en double disparaissent dans tous les scénarios. La latence médiane avec 40 réveils simultanés se dégrade (5,1 → 6,5 s) : les lectures attendent davantage leur créneau dans la fenêtre de 16 de herdsman, alors que le temps de service médian reste à ≈ 40 ms. Le scénario « parent perdu » est très dispersé selon la graine. Sur 6 graines, l'après donne 97,2 à 98,5 % et une médiane de 3,1 à 6,6 s ; la référence donne 96,8 à 98,5 % et 1,8 à 10,1 s.

Pour l'enfant qui change de parent, la version d'avant répondait à `assocRemove` par une erreur RPC : herdsman abandonnait la commande. Ses trames s'accumulaient dans la file indirecte du coordinateur jusqu'à expirer.

Les latences de plusieurs secondes sont celles du *poll* (2 s) et de la fenêtre herdsman, pas du coordinateur.

**Essayé et écarté (21 novembre) :** interroger soi-même (`Node_Desc_req`) un appareil de capacité inconnue après une lecture sans réponse, pour ne plus réémettre vers un endormi. Le pire cas du routeur congestionné passait de 67 à 75 %, mais la latence médiane se dégradait dans les scénarios légers (36 ms → 2,25 s avec 10 réveils simultanés) : la requête occupe elle-même la file du parent et le *poll* suivant.

### Blocages Flash

Une compaction du store efface 7 secteurs. Pendant ce temps, le CPU, qui exécute depuis la Flash, est bloqué ; la durée réelle n'est pas mesurée et ≈ 76 ms est retenu. Le cœur radio continue de recevoir et d'acquitter. Scénario : 15 blocages, 100 rapports/s et 20 transactions/s.

| Tampons RX | Trames perdues faute de tampon (sans ACK) | Rapports livrés | Latence p99 |
|---|---|---|---|
| 4 (firmware) | 149 | 99,87 % | 69 ms |
| 8 | 19 | 100 % | 62 ms |

Les émetteurs réessaient les trames non acquittées, donc presque rien n'est perdu. La compaction n'arrive que toutes les quelques heures en charge. **Les 4 tampons sont gardés :** 4 de plus coûteraient ≈ 580 o de SRAM pour un gain marginal (8,6 Kio libres au 21 novembre).

## Ce qui n'est pas mesuré

- **Le débit sur le matériel.** La section 5 est une simulation. Le seul dongle est le coordinateur de production : une mesure intensive demande un réseau d'essai isolé, avec un autre dongle et quelques routeurs.
- **ZeroStack sur banc physique :** l'image fermée n'a pas été flashée.
- **Les durées réelles d'effacement de la Flash pendant une compaction.**

## Mettre à jour

Après chaque mesure sur le dongle ou chaque changement des chemins mesurés :

1. Banc émulé : `python tools/build_freertos.py arm`, puis `python tools/bench_freertos.py build/freertos/arm-test/freertos-portable-test-only.elf build/freertos/arm-test/benchmarks.json`.
2. Réseau réel, avec Zigbee2MQTT arrêté et une copie de sa base : `node tools/znp_prod_bench.cjs --port COM8 --baudrate 1000000 --database <copie>/database.db --backup <copie>/coordinator_backup.json --expect-ieee 0x... --out private/perf-AAAAMMJJ-libellé --reads 3 --bursts 8 --tx-power 20 --herdsman-retries 1 --timeout 10000`.
3. CCM\* natif : DIAG 0x1B, avec `ccm_hw_block_frame_cycles` et les `ccm_prof_*`, via `tools/freertos_perf.py`.
4. Débit et appareils endormis (simulation) : `python tools/build_freertos.py arm`, puis `python tools/bench_transaction_cpu.py build/freertos/arm-test/freertos-portable-test-only.elf build/freertos/arm-test/transaction-cpu.json`, puis `python tools/bench_throughput.py`. Ajouter `--quick` pour un passage rapide.
5. Reporter les nouvelles valeurs ici et dans le tableau du README, en changeant la date des deux.
