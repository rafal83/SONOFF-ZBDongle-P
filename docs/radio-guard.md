# Protection structurelle avant l'entrée NWK

Le callback TI `MAC_CbackEvent` vérifiait seulement que la taille du MSDU était non nulle avant de lire ses octets 0 et 1, puis son octet 6 dans le chemin broadcast. La variante ajoute un contrôle avant ces lectures et avant le callback applicatif. Elle protège aussi l'index de la table des événements MAC et le pointeur du callback.

`firmware/src/radio_guard.c` est une implémentation indépendante sans allocation. Elle borne les en-têtes NWK et leurs champs optionnels, la liste de relais, l'en-tête de sécurité et le MIC. Le niveau de sécurité utilisé pour la longueur du MIC provient de la NIB ; le niveau transporté dans la trame ne remplace pas cette configuration. Les formats viennent des en-têtes TI épinglés et des sections 3.3.1, 4.3.1 et 4.5.1 de la [spécification Zigbee R23 publiée par la CSA](https://csa-iot.org/wp-content/uploads/2023/04/05-3474-23-csg-zigbee-specification-compressed.pdf).

Un paquet rejeté est libéré par la même API MAC et selon le même transfert de propriété que le chemin TI existant pour un DATA_IND vide. Aucun accusé de réception, paquet, résultat de sécurité ou succès réseau n'est fabriqué. Les compteurs ne contiennent pas de payload ou de clé.

La protection ne déchiffre pas, n'authentifie pas, ne supprime pas les doublons et ne remplace pas le moteur NWK/APS. Les trames Green Power admissibles passent vers le chemin TI distinct ; leurs champs ne sont pas validés comme un en-tête NWK ordinaire. Les champs Stub APS ont des bornes adaptées, y compris l'adresse de groupe.

Le diagnostic en lecture seule SYS `0xF0` expose le nombre de rejets et un mot décrivant le dernier rejet : taille (bits 0..7), classe (8..15), deux octets de contrôle NWK (16..31). Aucun payload, adresse, compteur de sécurité ou clé n'est retenu. Il expose aussi les échecs d'allocation RX, réessais du pilote TX, abandons TX et erreurs UART. Ces observations ont accompagné le [véritable essai radio HG06106C](radio-lab.md), sans rejet observé lors de sa séance réussie ; elles ne constituent pas une campagne radio malformée.

## Vérifications exécutées

Le programme C natif place chaque buffer en fin de page, suivi d'une page inaccessible sous Windows. Il teste les options et troncatures NWK, les indices/tailles de relais invalides, les formats Stub APS/GP, les tailles du MIC et deux millions de cas pseudo-aléatoires. Les entrées ne sont pas modifiées. Le préfixe réellement généré pour le callback est également testé avec les dépendances de propriété simulées.

`tools/test_arm_radio.py` charge l'ELF TI réel dans Unicorn, puis exécute exclusivement les instructions de `port_radio_guard_accept` et du callback public `MAC_CbackEvent`. La disposition des champs des structures MAC et de la NIB est lue dans le DWARF de cet ELF, sans la recopier depuis le modèle natif. Chaque troncature est placée contre une frontière de mémoire non mappée. Le test vérifie le retour du garde, la libération unique d'un rejet et le passage d'une trame admissible jusqu'au premier appel MAC natif.

Deux entrées de la table ROM publique sont remplacées par des stubs : libération et lecture d'attribut MAC. Aucune instruction de ROM ou de bibliothèque NWK/sécurité fermée n'est exécutée. Le test s'arrête pour une trame admissible avant le traitement natif : ce résultat ne prouve pas que la trame sera authentifiée ou traitée avec succès. Le rapport lie les résultats au SHA-256 de l'ELF et du script de test.

Les outils Unicorn 2.1.3 et pyelftools 0.32 restent dans `.tooling/arm-tests`, avec versions et empreintes de téléchargement verrouillées. Ils ne sont pas liés au firmware. L'émulation ne reproduit ni les horloges, ni les interruptions du SoC, ni les DMA, ni la radio. Aucun résultat de test physique n'en est déduit.
