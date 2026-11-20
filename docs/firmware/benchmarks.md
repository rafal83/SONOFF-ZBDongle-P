# Performances : notre firmware face à Z-Stack (Koenkk) et ZeroStack (SMLIGHT)

Dernière mise à jour : **20 novembre 2026**. Ce document est la référence chiffrée ; le [README](../../README.md#comparatif) en reprend le résumé. Il est mis à jour à chaque nouvelle mesure (voir [Mettre à jour](#mettre-à-jour)).

Trois sources, à ne pas confondre :

| Source | Ce qui est mesuré | Notre firmware | Z-Stack Koenkk | ZeroStack |
|---|---|---|---|---|
| **Réseau réel** (dongle de production, 130 appareils) | lectures Zigbee de bout en bout | mesuré | **mesuré** (image 20250321, même dongle, même réseau) | non disponible |
| **Dongle, compteur DWT** | cycles CPU d'une trame CCM\* | mesuré | — | — |
| **Émulateur** (Unicorn, Cortex-M4F) | instructions de nos chemins de code | mesuré | — | — |
| **Documentation SMLIGHT** | leurs chiffres, sur CC1352P7 à 48 MHz | — | « Z-Stack stock » selon SMLIGHT | publié, **non reproduit** |

ZeroStack est un binaire fermé : il n'a été ni exécuté ni émulé ici.

## 1. Réseau réel

Banc en lecture seule `tools/znp_prod_bench.cjs`, dongle de production sur COM8, Zigbee2MQTT arrêté. Chaque routeur reçoit une lecture de l'attribut Basic ; les rafales envoient une lecture à chaque routeur en même temps, comme Zigbee2MQTT au démarrage. Liste blanche de commandes ZNP, aucune écriture vers les appareils.

| Image | Conditions | Lecture isolée, médiane / p95 | Rafales réussies | Part des routeurs joignables |
|---|---|---|---|---|
| **Notre 72bc038** (CCM\* natif) | 29 routeurs, routeurs morts retirés, 20 dBm, réessais herdsman, délai 10 s | 78 ms / 226 ms | 207 / 232 | **89,2 %** |
| Notre 57f17bc | 32 routeurs dont 4 morts, 20 dBm, réessais, 10 s | 66 ms / 218 ms | 209 / 256 | 93,3 % (209 / 224) |
| Notre 975a2b0 (file MAC de 32) | 32 routeurs dont 4 morts, 5 dBm, sans réessai, 4 s | 74 ms / 125 ms | 103 / 160 | 73,6 % |
| Notre 4a9e55f (AES matériel par blocs) | idem | 43 ms / 87 ms | 71 / 160 | 50,7 % |
| Notre ed08b88 (AES logiciel) | idem | 74 ms / 95 ms | 68 / 160 | 48,6 % |
| **Z-Stack Koenkk 20250321** | 32 routeurs dont 4 morts, 115 200 bauds, **sans réessai, 4 s** | **51 ms / 127 ms** | 140 / 160 | **100 %** (28 / 28 à chaque rafale) |

Lecture :

- **Z-Stack Koenkk reste devant en rafale.** Il a répondu à 100 % des routeurs joignables, dans des conditions plus dures pour lui : sans réessai et avec un délai de 4 s. Notre meilleure image est entre 89 et 93 %.
- **Les progrès d'aujourd'hui :** passer de 49 % à 89–93 % est venu de quatre changements :
  - une file MAC plus grande ;
  - des tours de retransmission MAC ;
  - la coalescence des Route Request ;
  - la puissance d'émission à 20 dBm, la valeur que Zigbee2MQTT impose.
- **Les échecs restants de 72bc038 ne sont pas internes au coordinateur.** Tous sont des « pas de réponse après une confirmation réussie ». Ils se concentrent sur deux routeurs : Lumière garage 0x7491 et Routeur dans portail 0x6454. Les compteurs du coordinateur ne montrent aucune perte interne.
- **La lecture isolée varie avec l'état du réseau,** de 43 à 78 ms selon la session. L'écart avec Koenkk (51 ms) est du même ordre que cette variation. Une comparaison serrée exige des sessions entrelacées.
- **La durée d'une rafale n'est pas comparable :** le délai d'attente est de 10 s chez nous et de 4 s pour Koenkk.

Données brutes : `private/perf-20261120-prod-117e5ba/bench-*/bench.json`, hors Git.

## 2. CPU : trame CCM\* (sécurité NWK) mesurée sur le dongle

Cycles DWT pour une trame de 22 octets d'en-tête authentifié et 40 octets de charge, à 48 MHz.

| Implémentation | Cycles | Durée |
|---|---|---|
| AES logiciel | 74 769 | 1,56 ms |
| Moteur AES par blocs (DMA) et contrôle logiciel | 21 609 | 0,45 ms |
| **CCM\* natif du moteur AES** (72bc038, en production) | **8 984** | **0,19 ms** |
| ZeroStack (publié par SMLIGHT) | 3 824 à 3 968 | ≈ 80 µs |
| Z-Stack « stock » selon SMLIGHT (« NV vieillie ») | 532 182 | 11,1 ms |

Le mode natif garde des contrôles que ZeroStack ne décrit pas :

- la clé chargée dans le moteur est vérifiée toutes les 16 opérations ;
- une opération sur 64 à 191, tirée au hasard, est recalculée en logiciel ;
- chaque trame rejetée est revérifiée en logiciel.

Le contrôle aléatoire coûte en moyenne environ 600 cycles par trame. Une trame à MIC falsifié coûte donc l'opération native plus une opération logicielle.

**En attente de mesure sur le dongle :** la version qui suit 7455a80 n'est pas sur le dongle. Elle apporte trois changements :

- la copie des données pendant le traitement de l'en-tête par le moteur ;
- l'effacement des tampons DMA par mots de 32 bits ;
- l'espacement des contrôles logiciels : 1 sur 64 à 191, au lieu de 1 sur 32 à 95.

Elle expose aussi le profil par phase du moteur dans le DIAG (`ccm_prof_*`). Aucun gain n'est revendiqué avant cette mesure.

## 3. CPU : charges décrites par SMLIGHT, sur notre code émulé

Banc `tools/bench_freertos.py`, scénarios dans `firmware/freertos/tests/arm_perf.c`, résultats dans `build/freertos/arm-test/benchmarks.json`.

- **Conversion en cycles (estimation) :** le rapport mesuré sur le dongle vaut 1,464 cycle par instruction, pour le même code logiciel (bloc AES, trame CCM\*). Il est appliqué à 48 MHz.
- **Dans l'émulateur, l'AES est logiciel.** Les lignes qui déchiffrent surestiment donc le coût de l'image de production : voir la section 2 pour le CCM\* natif.
- **SMLIGHT ne publie ni la taille des trames, ni le nombre d'itérations, ni la version de Z-Stack comparée.** Les rapports ci-dessous sont des ordres de grandeur.

| Charge (définition SMLIGHT) | Notre firmware | ZeroStack (publié) | Z-Stack « stock » (selon SMLIGHT) |
|---|---|---|---|
| Écho d'un broadcast relayé (50 routeurs, 1 000 échos) | **≈ 2 555 cycles (53 µs) par écho, total ≈ 53 ms** : doublon reconnu avant tout déchiffrement | 7 076 cycles (147 µs), total 151 ms | 143 865 cycles (3 ms), total 3 s |
| Trame rejouée | ≈ 2 474 cycles (52 µs), rejetée avant déchiffrement | **1 550 cycles** | 143 802 cycles |
| En-tête de sécurité tronqué | ≈ 1 682 cycles (35 µs) | 320 à 6 631 cycles | défaut distant ≈ 3 ms |
| MIC falsifié | ≈ 63 825 cycles en AES logiciel ; en production : CCM\* natif plus revérification logicielle | **4 760 cycles** | 143 058 cycles |
| Réception d'un rapport ZCL sécurisé (MAC, NWK, APS), 200 appareils | ≈ 65 850 cycles (1,37 ms) en AES logiciel | — | — |
| Appareil inconnu cherché dans une table pleine | **≈ 252 cycles (5,2 µs)**, 200 entrées | 326 cycles, 300 entrées | 1 486 228 cycles |
| Allocation mémoire | **sans objet : aucun tas**, tout est statique | 218 cycles | 2 550 cycles |
| Écriture identique de 100 octets, × 48 | ≈ 4 537 cycles (94,5 µs), 0 octet écrit | **3 770 cycles (78,5 µs)**, 0 octet | 100 714 cycles, 48 écritures |
| Écritures de 100 octets avant la 1re compaction | 512 (store vide) ; **405** avec 200 enregistrements vivants de 48 octets | 1 013 | 69 |
| Coût d'une compaction (200 enregistrements vivants) | ≈ 19,6 ms de CPU et 7 effacements de secteur (durée réelle non mesurée) | non publié | non publié |
| Coupures de courant testées | **27 980**, 0 perte : 25 900 pendant des compactions, 858 sur le journal, 770 sur le store complet, 452 pendant la migration de disposition | 22 959, 0 perte | 1 conflit, récupération de 62 pages |
| Trames malformées | 0 plantage sur ≈ 208 millions d'entrées fuzzées (ASan + UBSan) | 0 plantage, 12 classes et 3 255 vecteurs | défaut distant |
| Bruit UART (64 octets collés à une commande) | 0 ping perdu, reprise en 40 ms | 0 ping perdu, reprise en 3 à 55 ms | 8,84 s d'interruption |

Ce qui reste en retrait de ZeroStack :

- **Le déchiffrement,** environ 2,3 fois leur chiffre en mode natif. L'écart vient de nos copies et attentes DMA, ainsi que de nos contrôles. Le profil par phase servira à le localiser.
- **Le rejet d'un MIC falsifié :** la revérification logicielle est un choix de sûreté.
- **L'endurance du store :** 405 écritures entre deux compactions contre 1 013 annoncées, avec un store vide chez eux.

## 4. Capacités et mémoire

| | Notre firmware | Z-Stack (Koenkk, `firmware.patch`) | ZeroStack (publié) |
|---|---|---|---|
| Appareils sécurisés (Trust Center) | 200 | 200 (`ZDSECMGR_TC_DEVICE_MAX`) | 200 sur CC2652P |
| Voisins | 32 | 50 | non publié |
| Routes AODV | 32 | 150 | non publié |
| Routes sources | 200 | 250 | « 200+ » |
| Table anti-doublon des broadcasts | 32 | 30 (`MAX_BCAST`) | 40 |
| Réessais MAC | 3 par tour, 3 tours (file de 32 trames) | 7 | non publié |
| Store non volatil | **2 × 56 Kio** (disposition 2) | NV Z-Stack (géométrie non comparée ici) | non publié |
| Flash occupée | 94 644 o (image `--hw-aes`) | 178 669 o (20240710) | non publié |
| SRAM | 76 808 o statiques, **sans tas** | 42 228 o statiques + tas dynamique de 33 371 o | 88 Ko de SRAM sur CC2652P, sans précision |

## Ce qui n'est pas mesuré

- **Le débit radio de bout en bout,** par exemple les « 180 messages/s » annoncés par SMLIGHT.
- **ZeroStack sur notre réseau :** l'image fermée n'a pas été flashée.
- **Les durées réelles d'effacement de la Flash pendant une compaction.**

## Mettre à jour

Après chaque mesure sur le dongle ou chaque changement des chemins mesurés :

1. Banc émulé : `python tools/build_freertos.py arm`, puis `python tools/bench_freertos.py build/freertos/arm-test/freertos-portable-test-only.elf build/freertos/arm-test/benchmarks.json`.
2. Réseau réel, avec Zigbee2MQTT arrêté et une copie de sa base : `node tools/znp_prod_bench.cjs --port COM8 --baudrate 1000000 --database <copie>/database.db --backup <copie>/coordinator_backup.json --expect-ieee 0x... --out private/perf-AAAAMMJJ-libellé --reads 3 --bursts 8 --tx-power 20 --herdsman-retries 1 --timeout 10000`.
3. CCM\* natif : DIAG 0x1B, avec `ccm_hw_block_frame_cycles` et les `ccm_prof_*`, via `tools/freertos_perf.py`.
4. Reporter les nouvelles valeurs ici et dans le tableau du README, en changeant la date des deux.
