# Travail sur les trois moteurs finaux

17 novembre 2026. Les modules `core_indirect`, `core_broadcast` et `core_security` sont des implémentations indépendantes testées sur PC et sur leurs instructions Cortex-M4F compilées par TI. **Ils sont raccordés au coordinateur ZNP depuis le candidat 2026111701 à 1 Mbaud.** Le 18 novembre, leur [variante 2026111802 sous SDK 8.33](sdk-migration.md) est installée sur le dongle ; les tests de moteurs isolés décrits ici restent une campagne distincte sans accès matériel. Voir les [raccordements, preuves et limites](core-integration.md).

Le [rapport reproductible](core-engine-validation-20261117.json) donne les empreintes des sources, des tests et de l'ELF de test. Le programme installé, son paquet et les sources du commit `f26734e` sont conservés séparément dans `private/release-20261117-frozen/`.

## Implémentations et portée actuelle

| Objectif | Code exécuté | Intégration et limites |
|---|---|---|
| Comptabilité indirecte bornée | Pool de 24 payloads, limite de trois unicasts par enfant, compteur global conservant les envois en cours, FIFO par destination, une transmission en cours par enfant, retry et confirmation, jetons avec génération contre les confirmations d'une ancienne allocation. Trois broadcasts partagés peuvent conserver chacun un masque de 128 destinataires, sans recopier leur payload. | Allocations, listes, polls, confirmations et libérations raccordés aux objets TI. Source matching intégré. Certains comptages et parcours d'expiration natifs restent linéaires. |
| Broadcasts et échos | Index source/séquence sur 40 transactions, reconnaissance des doublons, acquittement passif compté une seule fois par voisin, budget de retransmissions, confirmation MAC, deux roues de 128 positions avec indicateur de tour pour les délais de 1 à 255 ticks, retrait et réutilisation. | Chemins `RTG_*` raccordés, voisins/enfants traduits en masques, changements de membres pris en compte. Le rafraîchissement de ces instantanés parcourt encore les tables. |
| Sécurité | Chemin de trames **NWK ENC-MIC-32** : validation des en-têtes, source IEEE et compteur du nonce, deux clés réseau, index de 200 identités, anti-rejeu publié après authentification, réservation TX durable avant usage, refus du débordement et nettoyage des temporaires. | NV native, checkpoint et pilote AES-CCM TI raccordés. APS, Trust Center, dérivation/transport de clés et Green Power restent TI. |


L'index commun est un trie Patricia sur identifiants de 64 bits, sans allocation dynamique ni chaîne de collisions. Une recherche visite au plus 64 branches, indépendamment du nombre d'entrées. Cela borne le travail en fonction de la largeur d'un identifiant ; ce n'est pas une promesse de nombre de cycles identique. Les copies et AES restent proportionnels à la longueur de trame. Un tick broadcast ne parcourt que ses événements dus : traiter simultanément k expirations coûte O(k).

Les doublons broadcast continuent à contribuer aux acquittements passifs après validation de sécurité. Les appels aux moteurs sont sérialisés par la tâche Zigbee ; ce ne sont pas des API à appeler concurremment depuis une ISR.

## Preuves obtenues

- Index : 100 000 mutations, vérification de toutes les entrées présentes à chaque étape, capacité entière, réutilisation, identifiants nuls et chaque bit distinctif.
- Indirects : limites 24/3, conservation du quota en vol, refus des anciens jetons, 10 000 cycles de réutilisation et 30 000 opérations comparées à un modèle FIFO indépendant. Les broadcasts pour enfants endormis vérifient échec MAC, reprise, livraison unique et destinataire 127.
- Broadcasts : saturation des 40 places, répétition des échos, voisin 127, budget de retry, annulation, expiration prioritaire et passage du compteur de ticks par zéro ; 1 000 cycles de saturation/réutilisation.
- Sécurité native : 1 105 trames de charge variable, six variantes d'en-têtes optionnels NWK, 1 245 trames altérées refusées, rejeu, clé alternative, échec de réservation ou de crypto, reprise à la borne réservée et refus du compteur maximal. Le backend vérifie aussi le [vecteur 1 du RFC 3610](https://www.rfc-editor.org/rfc/rfc3610.html#section-8).
- ARM : exécution du véritable code compilé par TI Arm Clang 4.0.2.LTS, convention AAPCS pour les jetons 64 bits, trames sécurisées avec AES-CCM réel sur l'hôte, et 1 500 entrées malformées au bord d'une page mémoire. Un dépassement de cette entrée déclenche une faute de l'émulateur.

Les tests natifs indirect/broadcast/index tournent en 32 et 64 bits sous Windows. Les fonctions de persistance sont des modèles avec injection d'échec ; aucun test de coupure Flash ni d'accélérateur crypto réel n'est revendiqué. La restauration des compteurs entrants reprend explicitement des valeurs attestées : elle ne transforme pas une sauvegarde ancienne en état anti-rejeu actuel.

## Budget SRAM mesuré

| Contexte Cortex-M4F | Octets |
|---|---:|
| Indirects, y compris broadcasts pour enfants | 1 056 |
| Transactions broadcast et roues de timers | 2 016 |
| Sécurité NWK pour 200 identités | 5 720 |
| Total des trois contextes | **8 792** |

La campagne ARM des moteurs isolés observe un pic de pile de **400 octets**. L'intégration complète observe 776 octets dans ses parcours testés. Ce ne sont pas des bornes démontrées pour toutes les tâches du coordinateur.

Le nouveau candidat offre 32 980 octets de capacité de tas avant allocations système. Les contextes, index de handles et adaptateurs réservent ensemble 13 520 octets à l'initialisation du tas TLSF, plus huit octets de métadonnées. Il reste **19 452 octets immédiatement après réservation**, avant les allocations système suivantes ; un minimum de 16 384 est contrôlé à ce point. Le contrôle statique de 32 Kio avant allocations n'a pas été abaissé. Le tas libre après démarrage réel et son endurance restent à mesurer ; voir [le budget complet](core-integration.md).

L'ELF `build/core-engines/core-test-only.elf` sert uniquement aux tests. Il ne contient ni table de vecteurs de démarrage ni CCFG SONOFF, n'est pas exporté en HEX et n'est pas un firmware coordinateur.

## Contrats TI couverts par les adaptateurs

L'analyse des en-têtes du SDK épinglé et des relocalisations ARM retrouve les points d'entrée publics. Elle montre aussi des écritures directes sur les états depuis plusieurs objets. Remplacer seulement `nwkbufs_hold` laisserait des compteurs incohérents lorsque les autres chemins changent un état.

1. **Cycle de vie des buffers.** Couvrir `nwkDB_CreateHdr`, `nwkDB_Add`, `nwkDB_RemoveFromList`, `nwkDB_DeleteRecAll`, `nwkbufs_reset` et les transitions effectuées par `nwk_MacDataBuffersAdd`, `RTG_SendNextEDMsg`, `RTG_TimerEvent` et le timer broadcast. Respecter les propriétaires du payload et de la requête MAC. Une réinitialisation du moteur exige que les anciennes transactions MAC soient drainées.
2. **Transactions broadcast.** Remplacer les chemins `RTG_BcastAdd`, `RTG_BcastChk`, `RTG_BcastTimerHandler` et la rétention des enfants ensemble. Le masque historique `bcastHoldAckMask` a des mots de 32 bits ; il ne peut pas être traité comme le masque de 128 destinataires du nouveau moteur. Une association supprimée ou réutilisée doit invalider ses destinataires encore en attente avant toute réaffectation d'identifiant. Traduire les unités des délais et rattraper les ticks coalescés.
3. **Sécurité SSP/NWK/APS.** Relier les deux clés réseau, les identités et compteurs aux enregistrements NV natifs ; ne pas écrire un nouveau schéma à la place de celui utilisé par herdsman. L'installation d'une clé et l'admission d'une identité sont des opérations authentifiées du gestionnaire, jamais une conséquence automatique d'une trame reçue. Garder les cas d'association, rejoin, changement de clé et récupération après panne dans la validation. Le module actuel ne traite ni sécurité APS complète ni Green Power.


## Reproduction locale

```powershell
python -m pip install -r tools/core-tests-requirements.txt
python tools/test_core_engines.py --arm --publish-report docs/core-engine-validation-20261117.json
python tools/build_coordinator.py --experimental-cores --baudrate 1000000
python tools/finalize_core_candidate.py
```

La commande utilise le compilateur Zig local pour les tests PC, TI Arm Clang déjà installé pour l'ELF ARM et les dépendances Unicorn/pyelftools épinglées du dépôt. Sans `--arm`, les tests PC peuvent aussi tourner en CI Linux. Aucun accès série, MQTT ou réseau de production n'est effectué.
