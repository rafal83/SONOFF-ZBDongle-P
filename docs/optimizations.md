# Optimisations indépendantes intégrées au coordinateur



## Correspondance et limites

| Objectif | Implémentation de cette version | Limite réelle |
|---|---|---|
| Reprise UART après bruit/troncature | Fenêtre RX fixe de 255 octets, réanalyse des suffixes après FCS invalide, délais de 100 ms sans octet / 250 ms d'âge | Reprise testée physiquement à 115200 ; pas de garantie contre un débit entrant illimité |
| Moins d'allocation et de fragmentation TX | Pool de huit trames, sans les trois allocations supplémentaires de l'ancien chemin TX ; deux places réservées au contrôle | Les messages MT d'origine utilisent encore le tas global, maintenant TLSF |
| Allocation globale à métadonnées bornées | TLSF remplace le tas linéaire dans les callbacks TI-RTOS ; classes réduites pour le CC2652P | Fragmentation externe toujours possible ; copies de realloc et inspection diagnostique du tas ne sont pas O(1) |
| Timers sans tas | 24 entrées et objets ClockP statiques, sélection par bitmap, callbacks modifiables | Capacité bornée à valider en charge ; saturation explicite, pas de repli dynamique |
| Priorité aux réponses/confirmations | SRSP, AF dataConfirm, SYS resetInd prioritaires ; quota de quatre contrôles avant un événement normal en attente | Saturation comptée ; les trames refusées ne sont pas inventées ni annoncées réussies |
| Lectures NV en RAM | Cache associatif 4×64 octets et index de 1 024 entrées + huit de réserve | Construction/reconstruction linéaire ; repli natif si saturation ; lectures froides positives encore en Flash |
| Recherche de clés par contenu | Quatre formes apprises indexées, collisions vérifiées, mises à jour de compteurs sans reconstruction | Les autres formes et le moteur SSP/NWK restent TI ; voir [nv-directory.md](nv-directory.md) |
| Écritures inchangées sans usure | Comparaison de longueur et contenu avant recherche d'espace/compaction ; CRC existant vérifié | Les écritures modifiées peuvent coûter une recherche/lecture de plus ; aucun facteur de gain matériel affirmé |
| Cohérence après modification NV | Invalidation par génération avant chaque programmation/effacement, même en cas d'échec | Invalidation globale conservatrice, cache perdu au redémarrage ; mutations hors NVOCMP non prises en charge |
| Broadcasts | Table de 40 entrées et FIFO statique de réception de dix messages, ajout/retrait constants | Traitement radio des échos, sécurité et acquittements passifs TI conservés |
| Messages entre tâches | Ajout constant par offset de fin, sans agrandir la table ; réveil atomique à la réception | Recherche par événement et files MAC autonomes encore linéaires ; voir [queues.md](queues.md) |
| Limitation du trafic vers les enfants endormis | Quotas natifs TI : 3 messages par enfant, plafond global abaissé de 69 à 24 sur 72 buffers | Réserve d'emplacements, pas de RAM ; gestion TI conservée, application effective non testée sur radio |
| Reprise après blocage des tâches | Watchdog matériel, quorum des boucles NPI et pile Zigbee, expiration mémorisée en NMI | Démarre après initialisation ; ne détecte pas tous les défauts logiques et ne valide pas la reprise NV après reset |
| Envoi en saturation mémoire | `ZMacDataReqSec` refuse un MSDU non vide sans allocation, avec le statut MAC existant | Protection ciblée ; ne corrige pas tous les chemins mémoire ou radio |
| Trames NWK tronquées | Contrôles structurels avant le callback applicatif et le chemin NWK TI : champs optionnels, relais, auxiliaire et MIC | Pas de déchiffrement ni de validation complète APS/GP ; voir [radio-guard.md](radio-guard.md) |
| UART fixe ; autodétection différée | 115200 testé physiquement ; profils fixes 460800 et 1000000 compilés | Changement automatique en échec matériel et désactivé, voir [uart-autobaud.md](uart-autobaud.md) |

Les recherches de cache et d'index sont bornées, sans parcours du journal après construction. Les défauts de capacité ou requêtes non indexables utilisent le backend TI ; **la sécurité ou la pile entière ne devient pas O(1)**. L'invalidation normale change seulement une génération 16 bits ; son débordement après 2^16 mutations remet le tableau à zéro. Le test traverse ce débordement et vérifie l'absence d'entrée ancienne réutilisée.

## Intégration dans TI-RTOS

`tools/coordinator_overlays.py` génère treize variantes TI, dont le script de liaison, dans `build/coordinator-overlay/`, avec les empreintes de l'entrée et de la sortie, ainsi qu'une adaptation TLSF depuis le clone BSD épinglé. Il ne modifie pas le SDK ou les sources tierces et refuse un contexte inattendu. Les avis TI et TLSF restent dans le paquet.

Le parseur n'est utilisé que dans la tâche NPI. Son prochain délai borne l'attente du sémaphore lorsqu'un fragment est en cours ; le processeur ne tourne pas en boucle pour attendre un FCS. Le watchdog borne aussi l'attente des deux tâches à une seconde, pour observer leur progression même sans trafic. L'horloge 64 bits TI est convertie en millisecondes afin de gérer le débordement du compteur de ticks 32 bits.

Tous les accès au pool TX passent par les sections critiques TI, y compris les producteurs des autres tâches. Une trame reste sélectionnée si le driver refuse sa copie ; elle peut être retentée après 10 ms. Le slot n'est libéré qu'après acceptation de sa copie complète par le transport TI. Le chemin ne fragmente pas les trames MT et ne mélange pas leurs octets.

Le cache NV est appelé sous le mutex existant de NVOCMP. Les entrées ne sont remplies qu'après une lecture TI réussie et sa vérification CRC. Les mutations passent par les fonctions physiques centrales de NVOCMP, donc suppression, compaction, effacement global et écriture échouée invalident aussi les données en RAM. Aucune écriture de compteur de sécurité n'est différée, et aucune clé ni donnée de cache n'est journalisée.

Les lectures NV ont aussi des contrôles d'offset avant addition 16 bits et des bornes de comparaison de contenu. Ces contrôles concernent l'API NV ; ils ne prétendent pas corriger toutes les classes de trames radio malformées.

## Tests réellement exécutés

Les quinze exécutables natifs comprennent aussi le quorum du watchdog avec échec d'ouverture et expiration pendant une réalimentation, ainsi que la vraie fonction `ZMacDataReqSec` extraite de la variante compilée. Cette dernière vérifie les échecs d'allocation/sécurité/MAC, la libération unique des buffers et la conservation du payload appelant. Le garde radio teste les en-têtes et deux millions de cas contre une page inaccessible. Après liaison, les tests des instructions ARM vérifient le garde et le préfixe de callback, en utilisant les champs ABI lus dans le DWARF réel. Le watchdog matériel et le respect des quotas sur radio ne sont pas simulés comme des succès.

`python tools/test_coordinator_optimizations.py` compile le **vrai NVOCMP modifié**, avec un backend NOR simulé imposant les transitions 1 vers 0 et les effacements de page. Le programme teste :

- 1 000 lectures par ID en cache : aucune lecture NOR supplémentaire.
- 1 000 recherches de contenu présentes et 1 000 absentes en cache : aucune lecture NOR supplémentaire ; une création ultérieure invalide le résultat absent.
- 1 000 écritures identiques : aucune programmation NOR ni effacement supplémentaire.
- Changement de longueur, lectures partielles, offsets invalides, suppression/recréation, saturation de page, compactage et redémarrage.
- 80 positions de coupure au cours d'une écriture : après redémarrage par la récupération TI, l'ancien ou le nouvel enregistrement complet est retrouvé.

Les tests supplémentaires couvrent 400 enregistrements NV, 74 appareils renseignés, 1 000 EUI absentes différentes sans lecture NOR, 40 changements de compteurs sans reconstruction, 40 coupures indexées et le repli sur un ancien doublon après corruption CRC. Les files broadcast passent 200 010 transferts ; les files de tâches passent Q=0..256 et 10 000 cycles sur 32/64 bits, sans modifier la taille de TaskEntry. Les instructions ARM des files et de la capture UART sont également exécutées avec périphériques simulés.

Le second exécutable teste le vrai `port_npi.c`, avec horloge/UART/allocateur simulés : bruit, FCS manquant, délais, débordement de ticks, allocation refusée, réserve TX, priorité, transport occupé et copie refusée. Les tests de quota et atomicité du pool commun restent dans `tests/test_npi.c`.

Les exécutables TLSF natifs 32 et 64 bits passent chacun 200 000 opérations aléatoires avec vérification des payloads, realloc, alignements 4 à 256, saturation, fusion et récupération intégrale. Le contrôle TLSF de la configuration 32 bits occupe 452 octets dans l'arène ; le build vérifie la capacité du tas après ce coût et l'alignement. Les tests du pool de timers passent 1 000 réutilisations, les refus de construction, les callbacks modifiés et la suppression depuis le callback. Tous ces tests sont exécutés avant chaque build optimisé.

Les API de timers par callback et par `(taskId, eventId)` partagent ce pool. Les timers ponctuels retirent leur objet de la file active puis déclenchent le nettoyage prévu dans la tâche Z-Stack ; aucun objet ClockP n'est détruit au milieu de sa propre exécution. Les timers périodiques, leur réarmement et leur conversion en timer ponctuel sont testés. La recherche d'un timer événement parcourt au maximum les 24 slots statiques ; elle ne dépend ni d'une liste sur le tas ni du nombre d'appareils réseau. Elle n'est pas présentée comme un index exhaustif O(1).


## Construire et comparer

```powershell
python -m pip install --target .tooling ziglang==0.13.0
python tools/setup_ti_toolchain.py
python tools/build.py host
node tests/herdsman_framing.mjs
python tools/build_coordinator.py
python -m unittest discover -s tests -v
```

Image optimisée : `build/coordinator-optimized/sonoff-znp-coordinator.hex`. Paquet : `build/sonoff-znp-optimized-20261114-lab.zip`. La commande `python tools/build_coordinator.py --baseline` conserve la base sans nos optimisations dans `build/coordinator/`, avec la révision `20250321`.

Pour comparer les temps CPU, accès Flash et trafic, suivre [benchmarks.md](benchmarks.md) sur un dongle séparé. Aucun flash, accès NV, arrêt de Z2M ou essai sur le coordinateur de production n'est autorisé par la construction de ces fichiers.


TLSF, les timers sans tas, les quotas indirects natifs et le watchdog sont maintenant intégrés. Restent une comptabilité des files indirectes entièrement O(1), le traitement O(1) complet des broadcasts et un moteur NWK/APS avec index exhaustifs. Certains chemins NWK/MAC/APS TI sont fournis seulement en bibliothèques ; ils ne sont pas remplacés ni modifiés en binaire. Les dépendances précises et les conditions d'une version finale sont dans [final-status.md](final-status.md).

