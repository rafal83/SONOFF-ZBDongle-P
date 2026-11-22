# Pile Zigbee native

Le coordinateur natif est écrit pour ce firmware ; il n'utilise aucune bibliothèque fermée de TI. Il se trouve dans `zigbee/nwk/zb_coord.c`, avec `nwk_route.c` et `nwk_devices.c`. Il assure :
- l'arrivée des appareils ;
- le Trust Center ;
- le routage maillé en concentrateur et les devoirs de routeur ;
- la persistance, dans `storage/nv_log.c`.

L'hôte le pilote par ZNP ([znp-compat.md](znp-compat.md)). Depuis le 18 novembre 2026, il est en production sur le dongle avec environ 130 appareils, dont 28 routeurs et des appareils à plusieurs sauts.

## Couches

| Couche | Module | Contenu | Preuves |
|---|---|---|---|
| MAC, trames | `ieee802154/mac/mac_frame.c` | Trames 2003/2006 et 2015 sans IE, compression PAN, en-tête de sécurité MAC, FCS, balise Zigbee, réponse d'association | CRC-16/KERMIT, aller-retour, troncature à chaque octet, 200 000 trames aléatoires sous UBSan |
| MAC coordinateur | `ieee802154/mac/mac_core.c` | 48 tampons. File directe de 32 trames : 3 réessais par tour, 3 tours. File prioritaire pour les réponses aux *polls*. Indirect : 16 trames par adresse courte (`core_indirect`) et 12 trames adressées par IEEE (réponses d'association et de réassociation, hors limite des 16). Bit *frame pending* et *source match*, expiration à 7,68 s, association, orphelin et réalignement, purge, rejet des doublons | Scénarios avec radio simulée, 634 000 contrôles, parcours balise/association/*poll* en instructions ARM |
| NWK, trames | `zigbee/nwk/nwk_frame.c` | En-tête PRO v2 : IEEE source et destination, multicast, *source route* (30 relais au plus, index vérifié), en-tête auxiliaire, nonce | Aller-retour de toutes les options, troncatures, fuzz |
| NWK, appareils | `zigbee/nwk/nwk_devices.c` | 200 appareils dans deux tableaux d'emplacements triés (IEEE et adresse courte, recherche dichotomique en 8 étapes au plus), adresses stochastiques 0x0001 à 0xFFF7, réutilisation à la réassociation | 10 millions de contrôles, dont 30 000 opérations aléatoires comparées à une recherche linéaire |
| NWK, routage | `zigbee/nwk/nwk_route.c` | 48 voisins (*Link Status*, coûts entrant et sortant), 48 routes AODV, 200 routes sources (*Route Record*), table anti-doublon de 32 broadcasts | `test_mesh`, `test_mesh_scale` |
| APS, trames | `zigbee/aps/aps_frame.c` | Données, commandes, ACK ; unicast, broadcast, groupe ; en-tête étendu (fragmentation) | Aller-retour, troncatures |
| Sécurité | `zigbee/security/aes128.c`, `ccm_star.c`, `zb_mmo.c` | AES-128, CCM\* (MIC vérifié à temps constant, clair effacé si échec), MMO, HMAC-MMO, codes d'installation | FIPS-197, RFC 3610, vecteurs OpenSSL |
| Sécurité NWK | `firmware/src/core_security.c`, `zb_ccm_backend.c` | Anti-rejeu par pair, réservation durable du compteur sortant. CCM\* natif de l'accélérateur AES, avec contrôle logiciel (clé vérifiée toutes les 16 opérations, recalcul logiciel aléatoire, revérification des rejets dans un budget) | `test_crypto`, `test_hal_aes` (pilote réel contre un modèle de registres, 9 pannes injectées) |

## Fonctions du coordinateur

**Arrivée et Trust Center**
- Formation, *permit join* temporisé reflété dans la balise, refus quand le réseau est fermé ou qu'aucune clé de lien n'est connue.
- *Transport Key* avec la clé de lien par défaut ou celle d'un code d'installation, livrée directement ou en indirect selon *RX on when idle*.
- Clés de lien uniques à la manière de Z-Stack 3.x (graine et décalage), compatibles avec les sauvegardes de herdsman ; *Request Key*, *Verify Key*.
- *Update Device* et tunnel APS pour les appareils qui rejoignent via un routeur ; réassociations sécurisées et Trust Center.
- Rotation de la clé réseau : diffusion de la nouvelle clé, puis bascule.

**Maillage**
- *Link Status* toutes les 15 s, *many-to-one* périodique et anticipé, routage par la source.
- *Route Request* et *Route Reply* émises, reçues et relayées ; relais unicast et broadcast ; *Network Status* ; conflits d'adresses ; départs (*Leave*).
- Changement de canal (`mgmtNwkUpdateReq`) : diffusion, puis bascule 10 s plus tard.

**Appareils endormis**
- Réponses aux *polls* en file prioritaire, avec 12 réponses à la fois.
- Un appareil qui interroge le coordinateur avec son adresse courte devient son enfant.
- Vieillissement des enfants selon l'*End Device Timeout* demandé (256 min par défaut).
- Enfant entendu via un routeur : il n'est plus traité comme enfant direct. `assocRemove` et `assocAdd` sont servis.
- *Parent_annce* (Zigbee R21, ZDO `0x001F` / `0x801F`). Après une restauration du réseau, le coordinateur liste ses enfants terminaux aux routeurs, 9 par trame, 10 à 20 s après le démarrage. Un routeur qui sert l'un d'eux répond, et l'enfant cesse d'être le nôtre. Dans les 60 s qui suivent l'annonce, une telle réponse libère l'enfant ; plus tard, elle est ignorée. À l'inverse, quand un routeur redémarré liste ses enfants, ceux que le coordinateur a entendus dans les 5 dernières minutes lui sont renvoyés (*Parent_annce_rsp*).

**APS et ZCL**
- Doublons filtrés, groupes, chiffrement APS sur demande de l'hôte.
- Fragmentation : envoi jusqu'à 230 octets, réception jusqu'à 512 octets (fenêtre d'un bloc, ACK APS de chaque bloc). Un message reçu plus long qu'une trame MT est remis à l'hôte comme le fait Z-Stack (`AF_INCOMING_MSG_EXT`, puis `AF_DATA_RETRIEVE`).
- Réémission par le coordinateur d'une lecture ZCL restée sans réponse : jusqu'à 3 fois en 4,5 s, dans un budget d'une par seconde par rafales de 8. La réponse en double qui en résulte est filtrée, et rien n'est réémis vers un appareil connu pour dormir.

**Green Power et Touchlink**
- Green Power : proxy de base et TempMaster.
- Touchlink : séance inter-PAN pilotée par `interPanCtl`. La séance se ferme seule après 30 s sans activité. Seules les trames ZLL remontent. Si la radio ne revient pas au canal du réseau, la récupération est bornée : réessais, redémarrage radio, puis redémarrage, soit 13 s au plus sans écoute.

**Persistance**
- Journal `nv_log` sûr face aux coupures : 2 banques de 56 Kio, disposition 2.
- Réservation durable des compteurs sortants, NWK et APS.
- Planchers des compteurs NWK entrants (format v2, empreinte IEEE de 32 bits), écrits tous les 1 024 et effacés à chaque réappairage.

**Diagnostic**
- Sous-système MT privé `0x1F`, et les mêmes blocs en éléments NV virtuels `0x0FA0` à `0x0FA8`, lisibles pendant que Zigbee2MQTT tourne : identité de l'image (commit), performances, maillage, coordinateur, store, UART.
- Liens qui perdent des réponses (`DIAG 0x1E`, NV `0x0FA8`). Le bloc liste les 8 destinations dont des lectures ZCL restent sans réponse après toutes leurs réémissions, avec le premier relais de leur route. Il liste aussi 8 relais, avec le nombre de lectures passées par eux qui ont dû être réémises. Un relais est signalé pendant 10 min dès que 6 lectures au moins, et au moins une sur trois, ont dû être réémises. Le relais est noté au premier envoi de chaque lecture.
- Journal des 8 derniers événements réseau : départs, *Update Device*, réassociations, associations, conflits, enfants vieillis ou déplacés, retraits.
- Compteurs de fiabilité, enfants directs et endormis, trames en file indirecte (actuelles et pic). Lecture par l'extension Zigbee2MQTT [`tools/z2m/coordinator-diag.js`](../../tools/z2m/coordinator-diag.js).

## Limites

| Fonction | État |
|---|---|
| Scans d'énergie et actifs (MAC) | Non faits ; `mgmtNwkUpdateReq` ne sert que le changement de canal |
| Messages entrants de plus de 512 octets | Non remontés (jusqu'à 512 : `AF_INCOMING_MSG_EXT` puis `AF_DATA_RETRIEVE`) |
| Réémission APS d'un envoi simple avec ACK demandé par l'hôte | Non faite (herdsman ne demande pas d'ACK APS ; la fragmentation attend bien chaque ACK) |
| LED | Aucune LED pilotée (`ledControl` : erreur RPC, interceptée par herdsman) |
| SAPI, BDB *finding & binding* | Non faits |
| Contournement d'un relais qui perd des trames | Non fait : le relais est repéré et signalé dans le diagnostic, mais la route source apprise reste utilisée. En simulation, une découverte de route déclenchée par le signalement réduisait les échecs en rafale derrière un relais défaillant, mais dégradait un routeur congestionné qui sert des enfants endormis (de 76,4 à 70,0 % livrés). Décision reportée aux données de production (voir [benchmarks.md](benchmarks.md#1-réseau-réel)) |

Aucune conformité ni certification Zigbee n'est revendiquée.

## Mémoire et performances

- **Aucune allocation dynamique :** pools et anneaux de taille fixe, saturation définie (refus compté).
- **Image `--hw-aes` à 1 Mbaud :** 106 840 o de flash, 75 272 o de SRAM statique sur 81 920 (piles et MSP comprises).
- **Pile radio :** 4 656 o au pire cas statique sur 5 120.
- **CPU :** une trame sécurisée coûte environ 1 650 cycles en CCM\* natif. Le rejet d'un écho de broadcast ou d'un rejeu coûte environ 1 500 cycles, avant tout déchiffrement. Détail dans [benchmarks.md](benchmarks.md).
