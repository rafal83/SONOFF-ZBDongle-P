# Phases D et E — pile Zigbee native

Le coordinateur natif (`zigbee/nwk/zb_coord.c`, `nwk_route.c`) assure l'arrivée des appareils, le Trust Center (clé réseau, clés de lien uniques à la Z-Stack 3.x, *Update Device*, tunnel APS), le routage maillé en concentrateur et les devoirs de routeur, ainsi que la persistance (`storage/nv_log.c`). Il est piloté par l'hôte via ZNP ([znp-compat.md](znp-compat.md)). Un appareil réel a rejoint, établi sa clé unique, survécu à un reset MCU et été piloté, d'abord par les diagnostics, puis par herdsman. Le maillage multi-sauts est validé en simulation (`test_mesh`).

## Réalisé : briques testées

| Couche | Module | Contenu | Preuves |
|---|---|---|---|
| MAC | `ieee802154/mac/mac_frame.c` | Trames 2003/2006 et 2015 sans IE ; compression PAN (tables 2006 et 2015) ; en-tête de sécurité MAC ; FCS ; balise Zigbee ; réponse d'association | Valeur de contrôle CRC-16/KERMIT, aller-retour, troncature à chaque octet, 200 000 trames aléatoires sous UBSan |
| NWK | `zigbee/nwk/nwk_frame.c` | En-tête PRO v2 : IEEE source/destination, multicast, *source route* (≤ 30 relais, index vérifié), en-tête auxiliaire, nonce | Aller-retour de toutes les options, troncatures, encodages réservés, fuzz |
| APS | `zigbee/aps/aps_frame.c` | Data/commande/ACK, unicast/broadcast/groupe, format d'ACK, en-tête étendu (fragmentation) | Idem |
| Sécurité | `zigbee/security/aes128.c`, `ccm_star.c` | AES-128, CCM* L=2, M ∈ {0,4,8,16}, vérification de MIC à temps constant, effacement du clair si échec | Vecteur FIPS-197, RFC 3610 n°1 et 9 vecteurs OpenSSL ; chaque bit de MIC altéré refusé |
| Sécurité | `zb_mmo.c` | Hachage MMO, HMAC-MMO, clés *key-transport*/*key-load*, CRC et dérivation des codes d'installation | Exemple publié 83FE…C3B5 → 66B6…02BB ; 13 longueurs MMO et 3 HMAC contre Python |
| MAC coordinateur | `ieee802154/mac/mac_core.c` | File directe (3 retransmissions sur NO_ACK, échec CSMA remonté sans reprise), transactions indirectes par `core_indirect` (24 / 3 par enfant) et petite table IEEE, bit *frame pending*, *source matching*, expiration à 7,68 s, balise Zigbee, association (indication/réponse), *data request*, orphelin + réalignement, purge, rejet des doublons (index `core_index`), filtrage logiciel | Scénarios complets avec radio simulée, 200 000 opérations aléatoires sous invariants, retour à vide ; parcours balise/association/poll exécuté en instructions ARM |
| NWK | `zigbee/nwk/nwk_devices.c` | Table de 200 équipements indexée IEEE et adresse courte, attribution stochastique 0x0001–0xFFF7, réutilisation à la réassociation, refus réseau fermé ou plein, apprentissage avec changement d'adresse | Unicité sur 200 attributions, saturation, suppression/réutilisation, 50 000 mutations aléatoires avec cohérence des deux index |
| APS / Trust Center | `zigbee/aps/aps_security.c`, `zigbee/tc/trust_center.c` | Sécurité APS ENC-MIC-32 (identifiants de clé 0 à 3), trame *Transport Key* (clé réseau standard) chiffrée avec la clé de transport dérivée du TCLK | Trame **identique octet pour octet** à la référence Python/OpenSSL ; clé de transport de « ZigBeeAlliance09 » égale à la valeur publiée `4BAB0F17…`; chaque octet altéré refusé |
| Arrivée d'équipement | `zigbee/nwk/zb_coord.c` | Formation, *permit join* temporisé reflété dans la balise, politique d'association (réseau fermé, clé de lien requise), *Transport Key* avec TCLK par défaut ou code d'installation par équipement, livraison directe ou indirecte selon *RX on when idle*, authentification NWK avec anti-rejeu, *Device Announce* (expéditeur authentifié = IEEE annoncée), remontée des données APS | Test de bout en bout avec équipements simulés écrits uniquement à partir des codecs : routeur, terminal endormi, code d'installation, mauvaise clé, réseau fermé, TCLK par défaut interdit, rejeu, MIC altéré, usurpation d'IEEE, source inconnue, NWK non chiffré, expiration du *permit join* |
| Sécurité NWK | `firmware/src/core_security.c` + `zb_ccm_backend.c` | Moteur existant (ENC-MIC-32, anti-rejeu indexé, réservation durable des compteurs) désormais alimenté par le CCM* logiciel | Ouverture d'une trame scellée par OpenSSL, rejet du rejeu, de chaque octet altéré et d'une mauvaise clé ; 3 000 aller-retours |

Les mêmes vecteurs sont rejoués sur les **instructions Cortex-M4 compilées** (Unicorn) : 58 vérifications, dont la MAC avec une radio simulée de test. Mesures d'émulation (instructions, pas cycles) : bloc AES logiciel ≈ 4 700, ouverture d'une trame NWK de 21 octets ≈ 42 600, analyse d'une balise ≈ 380 ; réponse à une *beacon request* ≈ 1 800 ; service d'un *data poll* jusqu'à l'émission ≈ 860, soit environ 18 µs à 48 MHz. À 48 MHz, l'AES logiciel coûterait de l'ordre de 0,1 ms par bloc : l'accélérateur AES du CC2652P sera nécessaire pour le débit cible.

## Réutilisation des moteurs existants

| Moteur | Rôle dans la future pile | Travail restant |
|---|---|---|
| `core_security` | Chiffrement/anti-rejeu NWK | Backend AES matériel ; persistance des compteurs dans le futur stockage |
| `core_indirect` | File des messages pour enfants endormis (quotas 24/3) | Raccorder au *data poll* MAC et au *source matching* radio |
| `core_broadcast` | Table de transactions broadcast, ACK passifs, roues de temporisation | Raccorder au relais NWK |
| `core_index` | Index Patricia (voisins, clés, adresses) | Tables de voisins/routes/adresses |
| `npi` | Transport MT | Déjà lié |
| `port_heap` (TLSF) | Tas borné pour les tampons de pile | Décider s'il est nécessaire ou si des pools fixes suffisent |
| `nv_cache`/`nv_index` | Cache RAM du stockage | Nouveau format natif à définir |
| `radio_guard` | Contrôle structurel avant NWK | Placer avant le parseur NWK natif |

## Feuille de route et critères

| Jalon | Contenu | Critère d'acceptation | État |
|---|---|---|---|
| D1 MAC | File TX, retransmissions, *data request*, association, balises, *source matching* | Tests hôtes, puis échange réel | **Fait, validé sur le matériel** (scans ED/actif non faits) |
| D2 NWK | Adressage stochastique, voisins (*Link Status*), many-to-one, *Route Record* et routage par la source, *Route Request/Reply* (émises, reçues, relayées), relais unicast et broadcast avec passive ACK, *Network Status*, conflits d'adresses, *Leave*, *rejoin* | Tests multi-nœuds simulés | **Fait** (`test_mesh`) ; multi-sauts réel à éprouver |
| D3 Sécurité | Trust Center centralisé, *Transport Key*, clés uniques (graine + décalage, compatibles avec les sauvegardes Z-Stack), codes d'installation, *Update Device* + tunnel, compteurs persistants | Vecteurs + appareil R21 réel | **Fait** ; clé unique validée sur un appareil réel ; rotation de la clé réseau non faite |
| D4 APS | Data/ACK, doublons, groupes, chiffrement APS à la demande | Tests hôtes | **Fait** ; fragmentation et retransmissions APS non faites (herdsman ne demande pas d'ACK APS) |
| D5 ZDO | Descripteurs, annonce, *mgmt LQI/Rtg* locaux, requêtes vers les appareils | Tests hôtes + herdsman | **Fait** ; *mgmt NWK update* (canal) non fait |
| D6 Stockage | Journal à coupure sûre, réservation durable des compteurs | Coupures simulées, reset réel | **Fait** (`test_nv_log`, persistance validée sur le dongle) |
| D7 ZNP | Familles SYS/UTIL/ZDO/AF/APP_CNF/NV attendues par herdsman | Recette herdsman | **Fait** : voir [znp-compat.md](znp-compat.md) |

Ordre retenu : D1 → D4 (codec) → D2 → D3 → D5 → D6 → D7, chaque jalon sur matériel de laboratoire avant le suivant. Aucune conformité ni certification Zigbee n'est revendiquée.

## Optimisations (phase E) : position actuelle

- **Mémoire** : aucune allocation dynamique dans la cible ; pools et anneaux de taille fixe ; comportement de saturation défini (refus compté).
- **Recherche** : les index existants bornent le travail à 64 branches ; ce n'est pas un O(1) en cycles. Les parcours de tables restants seront signalés comme tels.
- **Sécurité** : la clé est posée par trame (`aes128_init` à chaque appel du backend logiciel) ; un cache de clés étendues et l'AES matériel sont prévus.
- **Radio** : aucune copie dans l'ISR ; une copie entrée → structure de trame dans la tâche radio, une vers la file hôte. La copie vers la file hôte disparaîtra quand la MAC consommera directement les entrées.
