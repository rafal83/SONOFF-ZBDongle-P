# Essai autorisé sur le réseau réel — 17 novembre 2026

Le candidat **20261114 à 1 000 000 bauds** est désormais installé sur le coordinateur de production à la demande de l'utilisateur. Z2M a été arrêté pendant la migration ; l'utilisateur a ensuite replacé le dongle sur le serveur et redémarré Z2M. Le firmware d'origine n'est donc plus celui actuellement installé.

## Préservation du réseau

Une copie vérifiée des **68 fichiers** du dossier Z2M a été prise avant intervention. La base contient 74 appareils (31 routeurs, 40 terminaux et trois entrées de type inconnu), en plus du coordinateur et de trois groupes. Une sauvegarde logique fraîche, lue sur le dongle d'origine avec le vrai moteur herdsman et une liste restrictive de commandes de lecture SYS/NV, représentait les 74 appareils. Elle contient aussi des entrées historiques, soit 137 enregistrements d'appareils au total.

Deux captures ROM complètes identiques ont précédé le flash dans la même connexion. L'installation utilise uniquement les effacements sectoriels et l'écriture de **0x00000 à 0x4ffff**. Les pages NV **0x50000–0x55fff** et le secteur contenant le CCFG/bootloader **n'ont été ni effacés ni programmés**. La CRC ROM et une relecture des 360 448 octets vérifient le résultat.

Après démarrage du candidat à 1 Mbaud, une nouvelle lecture logique vérifie l'identité du coordinateur, les paramètres réseau, la clé réseau, la graine TCLK, les appareils et leurs clés de liaison. Les compteurs réseau et par appareil ne reculent pas. Les 74 appareils sont toujours représentés. Aucun appareil n'a été réappairé et aucune commande d'association n'a été envoyée.

La configuration Z2M a été modifiée par remplacement/insertion du seul paramètre `serial.baudrate: 1000000`, avec conservation du reste du fichier et vérification après écriture. Les secrets, images Flash et sauvegardes demeurent dans `private/production-20261117/`, hors Git et archives publiques.

## Contrôles sur le serveur

Le véritable **Zigbee2MQTT 2.14.3**, avec **herdsman 11.2.0**, démarre et reconnaît la révision 20261114. Il retrouve les 74 appareils. Les requêtes MQTT de santé et de contrôle du coordinateur réussissent ; aucun routeur n'est signalé comme manquant dans la mémoire du coordinateur.

L'observation MQTT dure 150 secondes. Elle inclut treize requêtes `/get` d'état sur des routeurs annoncés compatibles avec cette lecture. Dix sont suivies de télémétrie fraîche. Trois lectures échouent avec `NWK_NO_ROUTE` : les dernières communications de ces appareils dans la base **avant migration** datent déjà de septembre ou début novembre. Cela suggère une indisponibilité antérieure ; sans comparaison radio immédiate avec l'original, ces trois appareils restent non validés.

Aucun `/set`, changement de sortie, reset, interview, scan de carte réseau ou ouverture d'association n'est effectué par le contrôleur. Les remontées des capteurs sont observées passivement. Un état ancien republié au démarrage n'est pas compté comme une réponse : son `last_seen` doit être postérieur à l'installation. Le [rapport sans secrets](production-validation-20261117.json) donne le dernier décompte et les limites.


## Retour éventuel à l'original

Le retour se prépare avec **Z2M arrêté** et le dongle reconnecté au PC. Le programme d'origine est sauvegardé. `tools/migrate_production.py rollback` vérifie le réseau courant, capture sa Flash deux fois, puis réécrit uniquement la zone applicative d'origine. La **NV courante reste en place**, avec les compteurs avancés pendant l'essai. Le bootloader reste également intact. La relecture complète et une vérification logique du réseau suivent le redémarrage à 115200.

Ne pas restaurer directement l'ancien dump Flash complet après utilisation sur le vrai réseau : ses compteurs de sécurité pourraient être plus anciens que ceux vus par les appareils. La procédure de laboratoire `run_dongle_session.py` ne convient pas à ce retour de production.

La configuration Z2M doit alors repasser à 115200 avec une sauvegarde de sa version courante et une modification limitée au débit. Le retour n'a pas été exécuté pendant cet essai : le candidat reste installé pour l'utilisation demandée.
