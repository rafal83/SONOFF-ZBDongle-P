# Banc de performances : notre firmware face aux chiffres publiés de ZeroStack et Z-Stack

19 novembre 2026. **Seul notre firmware a été mesuré.** Les chiffres de ZeroStack et du « Z-Stack stock » viennent de la documentation publique de SMLIGHT ([dépôt `smlight-tech/zerostack`](https://github.com/smlight-tech/zerostack), WHITEPAPER.md), mesurés par SMLIGHT sur un CC1352P7 à 48 MHz. Ils n'ont pas été reproduits ici. Ni Z-Stack ni ZeroStack n'ont été exécutés : ce sont des binaires fermés, impossibles à émuler sans le CC2652P, sa ROM et son cœur radio. Aucun dongle n'a été utilisé ou flashé.

## Méthode

- Nous reproduisons, sur notre code compilé pour Cortex-M4F, les charges décrites par SMLIGHT. Le banc est `tools/bench_freertos.py` et les scénarios sont dans `firmware/freertos/tests/arm_perf.c`.
- Les instructions sont exécutées dans Unicorn 2.1.3, sans périphérique ni RTOS. Les chiffres bruts sont dans `build/freertos/arm-test/benchmarks.json`.
- **Conversion en cycles (estimation) :** le même code a été mesuré sur le dongle (DWT) et dans l'émulateur. Le rapport vaut 1,464 cycle par instruction, à l'identique pour le bloc AES (6 870 cycles / 4 693 instructions) et pour la trame CCM\* (74 769 / 51 064). Nous l'appliquons partout, à 48 MHz.
- **Dans l'émulateur, l'AES est logiciel.** L'AES matériel de l'image de production n'est mesuré que sur le dongle, pour une trame CCM\* de 40 octets.
- SMLIGHT ne publie ni la taille des trames, ni le nombre d'itérations, ni la version de Z-Stack comparée. **Les rapports ci-dessous sont des ordres de grandeur, pas une comparaison à protocole identique.**

## Résultats

| Charge (définition SMLIGHT) | Notre firmware | ZeroStack (publié) | Z-Stack « stock » (publié par SMLIGHT) |
|---|---|---|---|
| Sécurité NWK d'une trame (CCM\* seul) | **21 609 cycles (0,45 ms)** avec AES matériel, **mesuré sur le dongle** ; 74 769 cycles (1,56 ms) en logiciel | 3 824 à 3 968 cycles (≈ 80 µs) | 532 182 cycles (11,1 ms, « NV vieillie ») |
| Réception complète d'un rapport ZCL sécurisé (MAC, NWK, APS) | 44 820 instructions ≈ 65 600 cycles (1,37 ms), AES logiciel, table de 200 appareils ; 44 592 avec 51 appareils | — | — |
| Appareil inconnu cherché dans la table pleine | **172 instructions ≈ 252 cycles (5,2 µs)**, 200 entrées | 326 cycles, 300 entrées | 1 486 228 cycles |
| Allocation mémoire | **sans objet : aucun tas**, tout est statique | 218 cycles | 2 550 cycles |
| Écriture identique de 100 octets, × 48 | 8 701 instructions ≈ 12 700 cycles (265 µs), **0 octet écrit** | 3 770 cycles (78,5 µs), 0 octet | 100 714 cycles, 48 écritures |
| Écritures de 100 octets avant la 1re compaction | 147 (store vide) ; **40** avec 200 enregistrements vivants de 48 octets | 1 013 | 69 |
| Coupures de courant testées | 1 628 points de coupure (`nv_log` 858, store complet 770), 0 perte ; ≈ 2 millions d'entrées de fuzzing sur stores corrompus | 22 959, 0 perte | 1 conflit, récupération de 62 pages |
| Écho d'un broadcast relayé par un routeur (50 routeurs, 1 000 échos) | 34 994 instructions ≈ 51 200 cycles (1,07 ms) par écho, **AES logiciel** ; total ≈ 1,07 s | 7 076 cycles (147 µs) ; total 151 ms | 143 865 cycles (3 ms) ; total 3 s |
| Trame rejouée | **1 696 instructions ≈ 2 480 cycles (52 µs)**, rejetée avant tout déchiffrement | 1 550 cycles | 143 802 cycles |
| MIC falsifié | 43 601 instructions ≈ 63 800 cycles (1,33 ms), AES logiciel : il faut déchiffrer pour vérifier | 4 760 cycles | 143 058 cycles |
| En-tête de sécurité tronqué | **1 144 instructions ≈ 1 675 cycles (35 µs)** | 320 à 6 631 cycles | défaut distant ≈ 3 ms |
| Trames malformées | 0 plantage sur ≈ 208 millions d'entrées fuzzées (ASan + UBSan) | 0 plantage, 12 classes et 3 255 vecteurs | défaut distant |
| Bruit UART | non mesuré en temps de reprise | 0 ping perdu, reprise en 3 à 55 ms | 8,84 s d'interruption |

Lecture :

- **Avantage clair sur le Z-Stack décrit par SMLIGHT :**
  - aucune lecture NV sur le chemin d'une trame ;
  - recherches indexées, à coût constant de 51 à 200 appareils ;
  - aucun tas ;
  - écritures identiques sans usure ;
  - rejeu et en-têtes invalides rejetés avant tout déchiffrement.
- **En retrait de ZeroStack sur tout ce qui déchiffre :** la trame CCM\* coûte environ 5 fois plus que leur chiffre, même avec l'AES matériel. Notre backend chiffre bloc par bloc par DMA, puis contrôle le premier bloc en logiciel (≈ 6 900 cycles). Leurs 4 000 cycles indiquent l'usage du mode CCM matériel natif du moteur AES. C'est la principale piste de gain, mais elle doit garder un contrôle équivalent à notre vérification logicielle.
- **L'écho d'un broadcast est déchiffré avant d'être reconnu comme doublon.** Un contrôle de la table anti-doublon avant déchiffrement le ramènerait au coût d'un rejeu, soit 20 fois moins. En contrepartie, l'accusé passif reposerait alors sur une trame non authentifiée. Ce choix reste à trancher.
- **Le store est petit :** 2 × 16 Kio, alors que l'image n'occupe que 87 Kio sur 288 Kio disponibles. Avec un réseau de 200 appareils, une compaction survient toutes les 40 écritures. Agrandir les bancs espacerait ces compactions, mais toucher à la géométrie du store exige une migration.

## Défaut trouvé par ce banc, corrigé

Le coordinateur n'inscrivait pas **ses propres broadcasts** dans sa table de transactions de broadcast (Zigbee PRO §3.6.5, ce que fait Z-Stack). Conséquences, reproduites dans l'émulateur avec des échos rechiffrés comme le fait un vrai routeur :

1. **La première copie relayée par un routeur était acceptée comme une trame nouvelle et remontée à l'hôte.** Les broadcasts de données et ZDO émis par Zigbee2MQTT (commandes de groupe, permit join…) revenaient donc une fois sous forme d'`AF_INCOMING_MSG` en provenance de 0x0000. Les commandes NWK (route request many-to-one) étaient déjà filtrées.
2. Cette première copie n'était pas comptée comme accusé passif. **Chaque broadcast était donc réémis deux fois de plus**, soit trois fois le temps d'antenne nécessaire.

Correction dans `nwk_tx` (`zb_coord.c`) : le broadcast émis est inscrit dans la table anti-doublon. Le test de non-régression, dans `test_mesh.c`, fait relayer le broadcast par deux routeurs et vérifie trois points : rien n'est remonté, deux doublons sont comptés, aucun échec d'authentification n'apparaît. Il échoue sans la correction. Avec elle, les 18 programmes de tests PC passent, ainsi que la recette ZNP herdsman 11.2.1 sur le simulateur (19 étapes). **Ce correctif n'est pas sur le dongle.**

## Capacités et mémoire

| | Notre firmware | Z-Stack (Koenkk, `firmware.patch`) | ZeroStack (publié) |
|---|---|---|---|
| Appareils sécurisés (Trust Center) | 200 | 200 (`ZDSECMGR_TC_DEVICE_MAX`) | 200 sur CC2652P |
| Voisins | 32 | 50 | non publié |
| Routes AODV | 32 | 150 | non publié |
| Routes sources | 200 | 250 | « 200+ » |
| Table anti-doublon des broadcasts | 32 | 30 (`MAX_BCAST`) | 40 |
| Réessais MAC / NWK | 3 / — | 7 / 4 | non publié |
| Flash occupée | 86 556 o (image `--hw-aes`) | 178 669 o (20240710) ; 194 949 o (build 2026111802) | non publié |
| SRAM | 69 576 o statiques, **sans tas**, 12 344 o libres | build 2026111801 : 42 228 o statiques + tas dynamique de 33 371 o (toute la SRAM) | 88 Ko de SRAM sur CC2652P, sans précision |

Nos tables de voisins et de routes AODV sont plus petites que celles de Koenkk. Le maillage simulé de 80 routeurs passe ses 10 phases de charge sans les saturer, mais un réseau réel bien plus dense n'a pas été essayé.

## Ce qui n'est pas mesuré

- **Le débit ou la latence radio de bout en bout,** par exemple les « 180 messages/s » annoncés par SMLIGHT : cela exige le matériel. La seule comparaison valable est une campagne physique entrelacée sur un second dongle (`tools/znp_campaign.cjs`).
- Le coût de nos chemins avec l'AES matériel hors de la trame CCM\* isolée.
- Les temps réels d'effacement et de programmation de la Flash pendant une compaction : la Flash est modélisée en RAM.
