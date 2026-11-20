# Performances : firmware FreeRTOS face à Z-Stack (Koenkk) et ZeroStack (SMLIGHT)

Dernière mise à jour : **20 novembre 2026**. Ce document est la référence chiffrée ; le [README](../../README.md#comparatif) en reprend le résumé. Il est mis à jour à chaque nouvelle mesure (voir [Mettre à jour](#mettre-à-jour)).

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
| **Commit 666f804** (priorité routes, recent_tx 32, reroutage) | 28 routeurs joignables, 20 dBm, réessais herdsman, délai 10 s | **34,4 ms / 60,4 ms** | **224 / 224** | **100 %** (8 rafales sur 8 à 28 / 28) |
| Commit 2f787a6 (reroutage Network Status, CCM\* optimisé) | 29 routeurs, 20 dBm, réessais herdsman, délai 10 s | 68 ms / 168 ms | 223 / 232 | 96,1 % (3 rafales sur 8 à 29 / 29) |
| Commit 1e7b645 (store 2 × 56 Kio, CCM\* natif allégé) | idem | 65 ms / 221 ms | 221 / 232 | 95,3 % (2 rafales sur 8 à 29 / 29) |
| Commit 72bc038 (CCM\* natif) | idem | 78 ms / 226 ms | 207 / 232 | 89,2 % |
| Commit 57f17bc | 32 routeurs dont 4 morts, 20 dBm, réessais, 10 s | 66 ms / 218 ms | 209 / 256 | 93,3 % (209 / 224) |
| Commit 975a2b0 (file MAC de 32) | 32 routeurs dont 4 morts, 5 dBm, sans réessai, 4 s | 74 ms / 125 ms | 103 / 160 | 73,6 % |
| Commit 4a9e55f (AES matériel par blocs) | idem | 43 ms / 87 ms | 71 / 160 | 50,7 % |
| Commit ed08b88 (AES logiciel) | idem | 74 ms / 95 ms | 68 / 160 | 48,6 % |
| **Z-Stack Koenkk 20250321** | 32 routeurs dont 4 morts, 115 200 bauds, **sans réessai, 4 s** | **51 ms / 127 ms** | 140 / 160 | **100 %** (28 / 28 à chaque rafale) |

Lecture :

- **Le coordinateur atteint 100 % de réussite en rafale sur tous les routeurs joignables (28 / 28 à chaque rafale, 224 / 224), égalant Z-Stack Koenkk tout en abaissant la médiane de lecture isolée à 34,4 ms.**
- **Les facteurs clés du résultat :**
  - Priorité des routes explicites (source routes / table routes) sur les voisins directs dans `next_hop()`, évitant les liaisons asymétriques (+20 dBm coordinateur contre faible puissance routeur).
  - Découverte de route avec temporisation d'attente (`disc_fresh_for`) pour laisser aux réponses multi-sauts le temps d'arriver avant repli.
  - Table `recent_tx` portée à 32 trames et réservée aux données applicatives avec réémission graduée en cas d'absence de réponse ZCL.
  - Priorisation des `APS ACK` en tête de file MAC directe (`direct_q`).
- **La lecture isolée varie avec l'état du réseau,** de 43 à 78 ms selon la session. L'écart avec Koenkk (51 ms) est du même ordre que cette variation. Une comparaison serrée exige des sessions entrelacées.
- **La durée d'une rafale n'est pas comparable :** le délai d'attente est de 10 s pour le firmware FreeRTOS et de 4 s pour Koenkk.

Données brutes : `private/perf-20261120-prod-117e5ba/bench-*/bench.json`, hors Git.

## 2. CPU : trame CCM\* (sécurité NWK) mesurée sur le dongle

Cycles DWT pour une trame de 22 octets d'en-tête authentifié et 40 octets de charge, à 48 MHz.

| Implémentation | Cycles | Durée |
|---|---|---|
| AES logiciel | 74 769 | 1,56 ms |
| Moteur AES par blocs (DMA) et contrôle logiciel | 21 609 | 0,45 ms |
| CCM\* natif, 72bc038 : la mesure incluait un contrôle logiciel | 8 984 | 0,19 ms |
| CCM\* natif, 1e7b645 : banc de 16 trames, hors contrôle logiciel | 3 171 | 66 µs |
| **CCM\* natif, 2f787a6** (en production) : banc de 16 trames, hors contrôle logiciel | **1 950** | **40,6 µs** |
| **CCM\* natif, 2f787a6 en trafic réel**, contrôles logiciels amortis | **≈ 1 650** | ≈ 34 µs |
| ZeroStack (publié par SMLIGHT) | 3 824 à 3 968 | ≈ 80 µs |
| Z-Stack « stock » selon SMLIGHT (« NV vieillie ») | 532 182 | 11,1 ms |

**Pourquoi l'écart entre 8 984 et 3 171 cycles.** L'ancienne mesure enchaînait 16 opérations par blocs puis 16 natives, sur le même compteur de contrôle. La 32e opération déclenchait donc un recalcul logiciel complet, environ 75 000 cycles, compté dans la mesure native. Le nouveau banc fait une opération de chauffe et espace les contrôles. Le gain propre aux optimisations est donc modeste :
- la copie des données pendant le traitement de l'en-tête ;
- l'effacement des tampons par mots.

**Trafic réel**, après un banc de production de 2 077 opérations natives sous 2f787a6 :

| Mesure | Valeur |
|---|---|
| Temps moyen passé dans le moteur | 1 119 cycles (contre 1 734 sous 1e7b645) |
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
| Écho d'un broadcast relayé (50 routeurs, 1 000 échos) | **≈ 1 692 cycles (35 µs) par écho, total ≈ 35 ms** : doublon reconnu avant tout déchiffrement | 7 076 cycles (147 µs), total 151 ms | 143 865 cycles (3 ms), total 3 s |
| Trame rejouée | **≈ 1 449 cycles (30 µs)**, rejetée avant déchiffrement | 1 550 cycles | 143 802 cycles |
| En-tête de sécurité tronqué | **≈ 1 082 cycles (23 µs)** | 320 à 6 631 cycles | défaut distant ≈ 3 ms |
| MIC falsifié | ≈ 62 792 cycles en AES logiciel ; en production : CCM\* natif plus revérification logicielle | **4 760 cycles** | 143 058 cycles |
| Réception d'un rapport ZCL sécurisé (MAC, NWK, APS), 200 appareils | ≈ 65 219 cycles (1,36 ms) en AES logiciel | — | — |
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
- **la trame rejouée :** 1 449 cycles contre 1 550 annoncés ;
- **l'écho de broadcast :** 1 692 cycles par écho (35 ms total) contre 7 076 cycles (151 ms) ;
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
| Flash occupée | 101 520 o (image `--hw-aes` 1 Mbaud de l'arbre de travail du 20 novembre, non flashée) | 178 669 o (20240710) | non publié |
| SRAM | 73 264 o statiques sur 81 920 (8 656 libres), piles et MSP comprises, **sans tas** | 42 228 o statiques + tas dynamique de 33 371 o | 88 Ko de SRAM sur CC2652P, sans précision |

## 5. Débit soutenu et appareils endormis : SIMULATION

**Ce ne sont pas des mesures physiques.** Banc `tools/bench_throughput.py`, qui pilote `firmware/freertos/sim/net_bench.c` : la vraie pile portable (ZNP, `zb_coord`, MAC, sécurité NWK) tourne sur le PC dans une simulation à événements discrets, à la microseconde. Aucun port série, aucune radio.

Ce qui est modélisé :
- **Hôte :** modèle de zigbee-herdsman 11 (adaptateur Z-Stack 3.x). 16 transactions au plus en même temps, chaque créneau étant tenu jusqu'à la réponse ZCL. Une seule SREQ à la fois sur l'UART. Attente de 10 s de la réponse. Reprises de herdsman : 2 s de pause après un échec, 5 tentatives au plus, une nouvelle tentative après une réponse manquante.
- **Liaison série :** UART à 1 Mbaud, ou 115 200 bauds pour une série. Files ZNP de 8 trames ; la tâche radio se bloque quand la file de sortie est pleine, comme `znp_send()`.
- **Tâche radio :** un seul CPU, avec l'ordre des étapes de `radio_task.c`. Chaque étape coûte les instructions mesurées sur le code ARM compilé (`tools/bench_transaction_cpu.py`, CCM\* exclu) × 1,464 cycle par instruction. S'y ajoute chaque opération CCM\* : 1 650 cycles en natif, moyenne mesurée en production.
- **Cœur radio :** 4 tampons de réception, ACK matériel et bit *frame pending* issu de la table de *source match*.
- **Canal 802.15.4 :** CSMA-CA non slotté, ACK, collisions entre toutes les émissions qui se chevauchent, 1 % de pertes aléatoires. Par défaut, un seul domaine de collision (cas défavorable). L'option de réutilisation spatiale sépare les sous-arbres.
- **Routeurs :** 28 au total, 16 à 1 saut, 9 à 2 sauts et 3 à 3 sauts. Chacun a sa file, ses réessais MAC et NWK et sa sécurité refaite à chaque saut. Il envoie un Route Record après chaque Many-to-One, renvoie ses broadcasts deux fois et répond aux lectures en 2 à 10 ms. Il garde les trames de ses enfants endormis.
- **Appareils endormis :** *poll* toutes les 2 s, avec attente de la trame annoncée et *polls* rapides ensuite. Après une perte de parent, ils se réassocient par le coordinateur.

Médiane de 3 graines × 30 s par point. « Avant » = sources du commit `3cda4b6` ; « après » = arbre de travail actuel. Les deux tournent sur la même version du simulateur. Résultats bruts : `summary.json` et `runs/` dans le dossier `--out`.

### Transactions complètes

Une transaction est une lecture d'attribut ZCL, de l'`AF_DATA_REQUEST` à la réponse remontée à l'hôte.

| Débit offert | Réussite avant → après | Obtenu (/s) | Service p50 / p95 / p99 avant | après | Réessais herdsman avant → après |
|---|---|---|---|---|---|
| 25 /s | 100 → 100 % | 25,0 | 18 / 37 / 57 ms | 18 / 36 / 59 ms | 0 → 0 |
| 50 /s | 100 → 100 % | 50,0 | 30 / 127 / 195 ms | 32 / 145 / 284 ms | 9 → 2 |
| 100 /s | 99,93 → 99,93 % | 100,0 | 59 / 192 / 10 136 ms | 75 / 232 / 10 076 ms | 57 → 44 |
| 150 /s | 93,1 → 92,0 % | 139,6 → 138,1 | 52 / 163 / 10 093 ms | 69 / 223 / 5 997 ms | — |
| 180 /s | 77,3 → 75,7 % | 139,1 → 136,5 | 54 / 171 / 10 104 ms | 70 / 211 / 10 066 ms | — |

Le **débit maximal stable** est le plus haut débit qui garde au moins 99,9 % de réussite et un p95 de bout en bout de 1 s au plus. Il est cherché par dichotomie, à ±6 /s près.

| Conditions | Avant | Après | p99 de service au maximum (après) |
|---|---|---|---|
| Arbre de 28 routeurs, un domaine de collision | 59 /s | 53 /s | 212 ms (avant : 10 s) |
| 28 routeurs à 1 saut | 103 /s | 103 /s | 319 ms |
| Arbre, réutilisation spatiale | 48 /s | 48 /s | 2,1 s |
| Arbre, UART à 115 200 bauds | 59 /s | 62 /s | 10 s |

Lecture :
- **La limite n'est ni le CPU ni les files du coordinateur.** La tâche radio reste sous 3,1 % de CPU à 180 /s. La file MAC ne dépasse pas 17 / 32 trames, les tampons RX 1 / 4, la file de sortie ZNP 3 / 8. Les limites sont l'occupation du canal (≈ 65 % à 140 /s dans un seul domaine) et la **fenêtre de 16 transactions de herdsman**. Chaque réponse perdue y bloque un créneau 10 s : au-delà de ≈ 60 /s, la latence de bout en bout passe à plusieurs secondes, alors que le temps de service reste sous 250 ms au p95.
- **Les corrections ne changent pas le débit maximal**, à la précision près. Elles réduisent les réessais herdsman (9 → 2 à 50 /s) et la queue de latence au débit maximal (p99 de 10 s à 0,2 s). La médiane augmente un peu au-delà de 50 /s : les lectures récupérées ajoutent des trames. Les réémissions sont limitées à une par seconde, avec des rafales de 8. Sans cette limite, le débit servi à 180 /s tombait à 122 /s.

### Messages entrants

Ce sont des rapports d'attributs spontanés : l'essentiel du trafic d'un réseau Zigbee2MQTT, et le sens habituel de « messages par seconde ».

| Rapports offerts | Arbre : livrés, latence p99 | 1 saut : livrés, latence p99 | CPU (1 saut) |
|---|---|---|---|
| 50 /s | 100 %, 24 ms | 100 %, 7 ms | 1,2 % |
| 100 /s | 99,97 %, 58 ms | 100 %, 8 ms | 1,9 % |
| 150 /s | 74,7 % (canal saturé à 82 %) | 100 %, 18 ms | 2,5 % |
| **180 /s** | 52,3 % (canal à 97 %) | **99,94 %, 50 ms** | 3,0 % |
| 250 /s | 29 % | 54,7 % (canal saturé) | 2,5 % |

**Plafond**, mesuré par pas de 10 /s avec 3 graines :

| Topologie | Débit soutenu (≥ 99,8 % livrés) | Débit livré au maximum | Au-delà |
|---|---|---|---|
| 28 routeurs à 1 saut | **190 /s** (p99 55 à 61 ms) | ≈ 195 /s (200 offerts, 97 à 99 % livrés) | Effondrement par collisions : 150 /s livrés à 240 offerts |
| Arbre à 3 niveaux | ≈ 110 /s (99,4 %) | ≈ 124 /s | 119 /s livrés à 140 offerts |

Une trame reçue coûte ≈ 6 800 cycles au coordinateur, CCM\* natif compris. **Le CPU suffirait donc pour ≈ 7 000 messages/s : à 180 /s, la limite est l'air.** Les messages relayés occupent le canal à chaque saut.

### Comparaison avec ZeroStack

Les **180 messages/s** de SMLIGHT sont annoncés sans topologie, sans taille de trame et sans dire s'il s'agit de messages entrants ou de transactions. Ici, en simulation, le firmware FreeRTOS :
- livre 180 rapports/s à 99,94 % quand les 28 routeurs sont à 1 saut ;
- plafonne entre 100 et 150 rapports/s dans un arbre à 3 niveaux sur un seul domaine de collision ;
- sert ≈ 140 transactions complètes/s au plus, et 53 à 103 /s en régime stable, car le modèle herdsman ne garde que 16 transactions en vol.

Ces chiffres ne sont pas comparables à ceux de SMLIGHT à conditions égales.

### Appareils endormis

Scénarios : 20 transactions/s, dont 30 % vers les appareils endormis (50 % pour les deux derniers), la moitié enfants du coordinateur et l'autre derrière des routeurs.

| Scénario | Réussite avant → après | Bout en bout p50 avant → après | Réponses en double vers l'hôte avant → après |
|---|---|---|---|
| 10 endormis, réveils étalés | 100 → 100 % | 43 → 28 ms | 103 → 0 |
| 20 endormis, réveils simultanés | 100 → 100 % | 3,8 → 2,5 s | 190 → 4 |
| 40 endormis, réveils simultanés | 100 → 99,7 % | 5,9 → 4,8 s | 244 → 1 |
| 20 endormis derrière un routeur congestionné (file de 2, 4 trames indirectes) | 66,6 → 77,3 % | — | 52 → 1 |
| 10 endormis, un enfant passe derrière un routeur sans réassociation entendue (herdsman : `assocRemove`) | 94,8 → 99,5 % | — | — |
| 20 endormis, parent perdu à 8 s, réassociation | 97,3 → 98,5 % | 8,1 → 1,5 s | 154 → 3 |

Pour l'enfant qui change de parent, la version d'avant répondait à `assocRemove` par une erreur RPC : herdsman abandonnait la commande. Ses trames s'accumulaient dans la file indirecte du coordinateur jusqu'à expirer.

Les latences de plusieurs secondes sont celles du *poll* (2 s) et de la fenêtre herdsman, pas du coordinateur.

### Blocages Flash

Une compaction du store efface 7 secteurs. Pendant ce temps, le CPU, qui exécute depuis la Flash, est bloqué ; la durée réelle n'est pas mesurée et ≈ 76 ms est retenu. Le cœur radio continue de recevoir et d'acquitter. Scénario : 15 blocages, 100 rapports/s et 20 transactions/s.

| Tampons RX | Trames perdues faute de tampon (sans ACK) | Rapports livrés | Latence p99 |
|---|---|---|---|
| 4 (firmware) | 138 | 99,97 % | 69 ms |
| 8 | 10 | 100 % | 61 ms |

Les émetteurs réessaient les trames non acquittées, donc presque rien n'est perdu. La compaction n'arrive que toutes les quelques heures en charge. **Les 4 tampons sont gardés :** 4 de plus coûteraient ≈ 580 o de SRAM pour un gain marginal (3,4 Kio libres au 20 novembre au soir).

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
