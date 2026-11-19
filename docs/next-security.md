# Candidat suivant : persistance de sécurité

17 novembre 2026. Cette variante indépendante est construite avec `--experimental-next --baudrate 1000000`, révision **20261117**, dans `build/coordinator-optimized-1000000-next/`. Après sa préparation, l'utilisateur a autorisé son installation et confirmé l'arrêt de Z2M. Elle est désormais installée sur son réseau, dont la dernière sauvegarde contient **75 appareils**. Le programme 20261114 est sauvegardé pour un retour conservant la NV courante.

## Corrections intégrées


### En-tête et entrée persistés ensemble

La fonction source `ZDSecMgrUpdateNV` préparait un buffer de table sans y recopier l'en-tête, écrivait ce buffer, puis relisait et réécrivait l'en-tête séparément. Le test de l'ancienne fonction avec une mémoire d'allocation déterministe reproduit un nombre d'entrées incorrect lors d'une simple mise à jour.

La nouvelle fonction lit l'enregistrement complet une fois, préserve les autres entrées, modifie l'entrée choisie et ajuste le compte seulement lors d'une création ou suppression. Elle écrit **l'entrée et l'en-tête en une transaction NVOCMP**. En cas de mémoire insuffisante, de lecture échouée ou d'en-tête manifestement incohérent, elle ne programme pas un buffer incomplet. Son API reste `void` : cette correction ne donne pas aux appelants TI un nouvel acquittement de persistance.

Le format NV est conservé. Il ne s'agit pas d'un moteur de sécurité entièrement O(1) : la copie et l'écriture restent proportionnelles à la taille de l'enregistrement. Les mesures de tests portent sur les appels API NV, pas sur le nombre de lectures NOR physiques ni la latence radio.

### Réservation durable des compteurs avant utilisation

La restauration native exploitait des lectures et écritures de clé sans vérifier tous leurs statuts avant de publier les compteurs en RAM. La variante vérifie l'allocation, la lecture de chaque entrée, les bornes de `keyNvId`, la lecture de la clé et l'écriture de réservation du compteur.

Un partenaire n'est réactivé en RAM qu'après réussite de cette réservation durable. Une entrée qui échoue reste invalide en RAM, sans suppression automatique de sa sauvegarde NV. Les compteurs proches du débordement sont refusés : il doit rester de la place pour la réservation de démarrage et un intervalle de checkpoint supplémentaire. Le buffer temporaire de clé est effacé par des écritures volatiles avant libération. La taille et le schéma des clés et compteurs restent ceux de TI ; aucune écriture de compteur n'est différée.

## Configuration et validations

Le préprocesseur du profil réel confirme **trois partenaires APS**, **200 entrées Trust Center**, et `MAX_APS_FRAMECOUNTER_CHANGES = 1000`, soit une réservation de démarrage de **1001**. La table de partenaires APS ne doit pas être confondue avec la table Trust Center. Cette dernière bénéficie déjà de l'index NV du firmware installé ; aucun gros index RAM supplémentaire n'est ajouté pour une table de trois entrées.

Les tests compilent et exécutent les **fonctions réellement extraites de l'overlay**, ainsi que leur variante TI d'origine. Ils vérifient les transitions répétées, la conservation des autres entrées, les échecs d'allocation/lecture/écriture, les indices invalides, les en-têtes corrompus et les limites de compteurs. Sur les entrées valides, la restauration des clés et compteurs est comparée à celle de la fonction native. Ils tournent avec trois entrées, puis avec une configuration de test à 205 entrées ; cette seconde configuration ne change pas la capacité du firmware.

Pour une mise à jour du profil réel à trois entrées, le test compte **4 appels de lecture + 2 écritures** dans la fonction d'origine contre **1 lecture + 1 écriture** dans la nouvelle. Les tests à 205 entrées comptent 206 lectures contre une. Ces résultats ne sont pas un facteur de gain CPU ou radio mesuré.

La compilation ARM complète, les contrôles ELF/HEX/CCFG et le seuil de tas de 32 Kio passent. Les tests ARM existants couvrent les instructions des gardes radio et du transport/files ; ils ne constituent pas une exécution ARM ciblée de ces nouvelles fonctions de sécurité. Les 17 exécutables C et les 41 tests Python passent ; les neuf classes de défaillance de sécurité restent des injections sur PC, pas des coupures matérielles du réseau réel.

## Essai matériel autorisé sur Z2M

Le HEX `94a3717da4204523a601371a96b13b0a532c666c9a161f12adbd78e00d1bbf25` a été programmé à la demande de l'utilisateur. Deux lectures identiques de la Flash précèdent l'effacement de la seule application. La NV, les secteurs réservés et le CCFG ne sont ni effacés ni programmés. CRC ROM et relecture complète vérifient le résultat. Avant et après démarrage, les lectures réseau représentent les 75 appareils, avec paramètres et clés préservés et compteurs non décroissants.

Le véritable Z2M 2.14.3 / herdsman 11.2.0 démarre à 1 Mbaud avec la révision 20261117. Les contrôles de santé et du coordinateur passent, sans routeur manquant en mémoire. L'ampoule HG06106C passe 48 changements de luminosité avec relecture ZCL de chaque niveau, puis six allumages en couleurs alternés avec six extinctions. Sa couleur initiale est rétablie côté Z2M ; l'état final allumé et le niveau 254 sont relus par Zigbee.

Trois erreurs `NWK_NO_ROUTE` au démarrage concernent des appareils dont les dernières communications enregistrées avant migration sont déjà anciennes (septembre et début novembre). Ces appareils restent non validés. Les relevés après correction de l'export comptent **39 appareils ayant communiqué après installation**, sans assimiler la présence dans la base à une réponse radio.

La première demande d'export ZIP par MQTT expire après 45 secondes. Z2M signale `Unknown property` et `ECONNRESET`, puis se reconnecte au broker. Un essai de commande expire pendant cette phase ; l'état final de l'ampoule est cependant relu, puis le nouvel essai de couleurs réussit après reconnexion. La sauvegarde native créée sur le serveur reste valide et conserve clés, appareils et compteurs non décroissants.

L'enquête suivante établit un dépassement de capacité : le contenu de sauvegarde initial représente environ **11 037 344 octets Base64**, alors que le broker Mosquitto 2.1.2 annonce un maximum de **2 000 000 octets**. Z2M utilise MQTT 5 et annonce un plafond de réception de **1 048 576 octets** ; son abonnement à son propre sous-arbre le fait aussi recevoir ses réponses. Le [code de sauvegarde Z2M 2.14.3](https://github.com/Koenkk/zigbee2mqtt/blob/2.14.3/lib/extension/bridge.ts) inclut tous les fichiers hors `log/` et `ota/`. Le dépassement est mesuré ; le message précis `Unknown property` n'a pas été reproduit isolément.

Quatre anciennes copies de migration `configuration_backup_v1.yaml` à `v4` et dix-huit icônes sans référence dans les fichiers texte actifs sont déplacées vers le dossier voisin `\\192.168.1.25\config\zigbee2mqtt-history-20261117`. Chaque fichier correspond à la copie privée antérieure, et son empreinte est vérifiée après déplacement. La configuration active, la base, les états et le firmware restent en place ; Z2M n'est pas redémarré. L'opération peut être annulée en remettant les fichiers aux chemins consignés dans le plan privé, sans écraser d'éventuels fichiers nouveaux.

La nouvelle demande de sauvegarde réussit : **605 960 octets ZIP**, soit **807 948 octets Base64**. Le CRC de toutes les entrées passe, et le nouveau `coordinator_backup.json` conserve les appareils, les clés et les compteurs non décroissants. Le contrôle de santé et celui du coordinateur passent après cet export, avec une lecture réelle de l'ampoule et sans nouvelle déconnexion pendant la minute observée. Les quatre erreurs MQTT de l'incident initial restent conservées dans les preuves.

Le [rapport sans secrets](production-validation-20261117-next.json) conserve les réussites, les tentatives échouées et leurs empreintes. Les essais de restauration logique d'une sauvegarde dans ce candidat, les défauts matériels de persistance, les 75 appareils individuellement et l'endurance restent ouverts. Le candidat est laissé installé pour l'essai demandé ; aucun retour au 20261114 n'a été exécuté.

## Objectifs encore ouverts


Le build, les sources et les preuves du firmware installé ont été conservés dans `private/release-20261114-frozen/` avant modification des outils. L'archive du candidat suivant reste un paquet de laboratoire sans autorisation implicite de migration du réseau actuel.
