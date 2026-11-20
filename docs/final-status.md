# Qualification finale : état et dépendances restantes

Ce rapport historique concerne l'ancien coordinateur `2026111802`, candidat de laboratoire du 18 novembre. Pour le firmware maintenu, consulter l'[état FreeRTOS](firmware/status.md) et les [procédures actuelles](firmware/README.md).

Le candidat corrigé `2026111802` utilisait le SDK TI 8.33.00.16 à 1 Mbaud. Le précédent `2026111801` avait été rejeté pour MEM_ERROR. Les observations matérielles de cette campagne sont consignées dans le [rapport daté](production-validation-20261118-sdk833-r2.json) ; elles ne constituent pas une qualification de la pile FreeRTOS actuelle.

Les [trois moteurs indépendants](core-engines.md) sont maintenant **raccordés au ZNP dans le candidat complet 2026111701 à 1 Mbaud** : indirects, broadcasts et NWK ENC-MIC-32. Les [tests de l'ELF lié et le budget mémoire](core-integration.md) passent avec frontières matérielles modélisées. La sécurité APS et le Trust Center restent TI. Les moteurs et adaptateurs réservent 13 520 octets au démarrage du tas, qui garde 19 452 octets avant les allocations suivantes. **Ce nouveau candidat n'est pas flashé et n'a pas encore de validation radio/Z2M.** Le firmware installé n'est pas modifié par cette campagne.

La [variante 20261117](next-security.md) ajoute une transaction atomique pour les entrées de sécurité et une restauration conditionnée à la réservation durable des compteurs. L'utilisateur a autorisé son installation ; Z2M et les essais radio sur l'ampoule passent, avec les 75 associations conservées. L'export ZIP MQTT réussit après archivage réversible de fichiers historiques inutilisés, sans redémarrage de Z2M. Le dernier relevé observe 39 appareils ayant communiqué depuis l'installation. Cette variante ne remplace pas les trois moteurs complets listés plus bas.

Depuis le comparatif de laboratoire, l'utilisateur a autorisé l'[essai sur son réseau de 74 appareils](production-trial-20261117.md). Le profil fixe 1 Mbaud est installé, avec NV et CCFG laissés en place, et le service Z2M/MQTT a démarré avec les associations conservées. Le contrôle du coordinateur ne signale aucun routeur manquant ; la validation de tous les appareils et l'endurance restent ouvertes. L'original est sauvegardé mais n'est plus le firmware actuellement installé.

Un intérêt mesuré est désormais établi : le profil fixe 1 Mbaud lit 240 octets NV en **5,65 ms contre 26,38 ms** pour l'original 20240710. Les 3 600 transactions comparatives passent ; les échanges réels avec une HG06106C passent également à 1 Mbaud, y compris pendant 80 écritures NV modifiées, après reset et après restauration de sauvegarde. Voir [performance.md](performance.md) pour la méthode, les mesures et leurs limites. L'original et sa NV sont restaurés à l'issue des essais.

## Éléments intégrés et testés sur PC

- Coordinateur Zigbee TI complet, protocole MT/NPI `zstack`, profil radio/bootloader SONOFF.
- Parseur UART robuste, délais bornés et pool TX à priorités/réserve.
- Cache associatif NV 4×64 octets et index complet de 1 024 entrées + huit de réserve, recherches par identifiant et quatre formes de contenu. Les compteurs sont mis à jour sans reconstruction lorsque les champs recherchés ne changent pas. Les écritures idempotentes évitent la compaction ; les échecs CRC rendent la main au parcours natif. Voir [nv-directory.md](nv-directory.md).
- Allocateur global **TLSF** dans les callbacks de tas TI-RTOS, avec allocation/libération à métadonnées bornées et fusion des voisins libres. Les copies de payload lors d'un realloc restent proportionnelles à la taille copiée ; l'inspection des statistiques parcourt le tas hors chemin d'allocation normal.
- Pool statique de **24 timers OSAL**, commun aux API de callbacks et d'événements `(taskId, eventId)`, sans allocation de l'entrée ou de l'objet ClockP. Saturation signalée par `OsalPort_NO_TIMER_AVAIL`, sans repli silencieux sur le tas. Les destructions depuis un callback sont différées au nettoyage par la tâche.
- Capacité de la table broadcast portée à 40 ; FIFO de réception statique de dix messages avec ajout/retrait constants. Les références publiques sont redirigées au linker et contrôlées dans l'image liée.
- Ajout entre tâches sans parcours de la file, taille/offsets TI conservés. Réception et réveil atomiques, limite de file corrigée, saturation d'inscription des tâches bornée. Voir [queues.md](queues.md).
- Quotas indirects natifs TI configurés à **3 messages par enfant et 24 au total**, contre 69 au total dans la base ; les limites sont vérifiées dans les octets du HEX lié. Sur les 72 emplacements de buffers configurés, les messages indirects ne peuvent plus en retenir 69 selon cette limite native. Cela réserve des emplacements, pas de la mémoire physique, et l'application effective des quotas reste à vérifier sur radio.
- Watchdog matériel configuré à **10 secondes par étape**, démarré après l'initialisation de la pile/NPI. Il n'est réalimenté qu'après progression des deux boucles de tâches. Le callback NMI mémorise l'expiration sans appeler d'API, sans Flash ni journalisation. Une réalimentation déjà en cours lors du NMI peut finir ; les suivantes restent bloquées. Le reset réel et la persistance après reset ne sont pas encore testés.
- Refus immédiat de l'envoi MAC si l'allocation d'un MSDU non vide échoue, au lieu d'appeler le MAC avec un pointeur nul. La vraie fonction TI modifiée passe ses tests d'échec mémoire, de sécurité et de transmission.
- Protection structurelle à l'entrée radio avant les lectures de `MAC_CbackEvent` : longueurs NWK, adresses IEEE optionnelles, multicast, listes de relais, en-tête auxiliaire et taille du MIC selon le niveau de sécurité de la NIB. Les rejets libèrent une seule fois les messages DATA_IND. Deux millions de cas aléatoires natifs et les cas ARM de l'ELF lié vérifient les accès mémoire ; les appels ROM sont remplacés par des stubs dans ces derniers. Voir [radio-guard.md](radio-guard.md).
- UART fixe par défaut (115200). Autodétection expérimentale désactivée après échec physique ; voir [uart-autobaud.md](uart-autobaud.md).
- NVS interne seul ; suppression des dépendances SPI de la LaunchPad absentes du SONOFF. Le placement RAM est vérifié contre toute section allouée, le tas et la zone ROM MAC réservée.



| Élément demandé | Constat | Dépendance pour le terminer |
|---|---|---|
| Moteur de sécurité entièrement O(1) | Candidat 2026111701 : cache/index et ENC-MIC-32 NWK remplacés, réservation TX et NV natives raccordées ; copies/AES proportionnels à la trame, APS/Trust Center/Green Power TI | Essais radio, clés, coupures et reprise ; aucune garantie globale O(1) |
| Comptabilité des files indirectes entièrement O(1) | Candidat 2026111701 : quotas, poll/retry, index de handles, listes et confirmations raccordés ; comptages/expirations natifs et récupération source matching gardent des parcours | Validation des enfants endormis, saturation, charge et marge de tas réelle |
| Traitement broadcast entièrement O(1) | Candidat 2026111701 : index de doublons, ACKs uniques, roues de retry/expiration et payloads partagés raccordés ; instantanés de membres et routage restent partiellement TI | Vérification radio des acquittements, maillage, changements d'associations et charge |
| Autobaud différé | Échec du changement de débit sur le dongle ; désactivé par défaut à la demande de l'utilisateur | Investigation matérielle ultérieure, hors de la qualification à débit fixe |
| Fonctionnement final du coordinateur | Version installée 20261117 : Z2M/MQTT et ampoule validés, 75 associations conservées. Nouveau 2026111701 : compilation et tests logiciels seulement | Démarrage matériel/Z2M du nouveau candidat, appareils, terminaux à pile, charge et endurance |

Ces limites sont établies à partir du SDK, des références et du désassemblage du nouvel ELF. La bibliothèque SDK originale est intacte ; une copie intermédiaire reçoit des alias et des relocalisations pour raccorder ensemble les points d'entrée publics et leurs appels internes, sans modifier les octets exécutables des objets. Les appelants et la table native de confirmations sont audités après liaison. `SSP_NwkSecurityProc` est maintenant redirigé ; les fonctions de routage et certains parcours de buffers restent TI.

Un filtre à l'entrée AF/MAC ne remplace pas les quotas de la file indirecte NWK : les buffers peuvent déjà être occupés avant sa prise en charge. De même, supprimer des trames broadcast avant NWK peut changer les acquittements passifs ou la sécurité. Ces approximations ne sont pas intégrées sous l'étiquette d'une optimisation complète.

## Conditions pour appeler une version « finale »

Il faut terminer les modules manquants, vérifier les budgets RAM/Flash et les parcours de défaillance, puis réussir les jalons radio de [validation.md](validation.md), notamment sur un réseau représentatif de 74 appareils. Les résultats CPU, Flash et latence doivent être mesurés avec le protocole de [benchmarks.md](benchmarks.md). Les 24 timers et le tas ont une capacité bornée qui doit aussi être validée sous charge réelle.

Les premiers essais de laboratoire à 115200, leur retour à l'original et leurs résultats figurent dans le [rapport radio](radio-lab.md). Ils ont été suivis de l'installation autorisée à 1 Mbaud puis de la variante 20261117, comme indiqué plus haut. Aucune de ces validations matérielles ne couvre le nouveau candidat 2026111701.

L'autodétection a échoué sur le matériel ; à la demande de l'utilisateur, elle est reportée. Le build par défaut est à débit fixe et l'option `--experimental-autobaud` reste réservée à l'investigation. Les simulations ARM restent des tests ciblés, sans modèle temporel du DMA.
