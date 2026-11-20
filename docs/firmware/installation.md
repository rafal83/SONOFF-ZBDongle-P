# Installer ou mettre à jour FreeRTOS

## Préparer

Utiliser un **SONOFF ZBDongle-P / CC2652P** ; le modèle E n'est pas compatible. Consulter [l'état du firmware](status.md) et ses [limites](znp-compat.md).

1. Sauvegarder le répertoire de données de Zigbee2MQTT, notamment `configuration.yaml`, `database.db` et `coordinator_backup.json`, dans un emplacement privé.
2. Arrêter proprement Zigbee2MQTT et toute application qui utilise le port série.
3. Brancher le dongle à l'ordinateur qui exécute Chrome ou Edge.

## Installer

1. Ouvrir le [Web Flasher](https://rafal83.github.io/SONOFF-ZBDongle-P/).
2. Choisir **Installer la dernière version** et le débit série voulu.
3. Cocher la confirmation d'arrêt de Zigbee2MQTT, puis **Connecter et sauvegarder** et sélectionner le dongle.
4. Conserver le fichier JSON téléchargé. Il contient toute la Flash, l'IEEE du dongle et un SHA-256 ; il peut contenir des clés privées du réseau.
5. Cliquer sur **Installer le firmware**. Garder le dongle branché et la page ouverte jusqu'à la vérification finale.
6. Rebrancher le dongle sur son hôte, puis démarrer Zigbee2MQTT avec `serial.adapter: zstack` et le `serial.baudrate` choisi.

La mise à jour remplace uniquement l'application. Les données réseau et la configuration du dongle sont conservées, puis relues pour vérification. Une configuration CCFG incompatible bloque l'installation avant l'effacement.

La conservation de la Flash ne convertit pas le format d'un réseau créé par un autre firmware. Pour reprendre un réseau, utiliser la sauvegarde native Zigbee2MQTT et vérifier son identité et ses compteurs ; voir [sauvegardes et restauration](restauration.md).

## En cas d'interruption

Le flasher ne demande aucun redémarrage après une écriture incomplète ou une vérification en échec. Reconnecter le dongle au flasher et relancer l'installation, ou [restaurer un firmware sauvegardé](restauration.md). Conserver la première sauvegarde : la capture d'une application interrompue ne la remplace pas.

La réussite indique une relecture Flash et une commande de redémarrage acquittée. Vérifier ensuite dans Zigbee2MQTT la reprise du réseau et les échanges avec les appareils.
