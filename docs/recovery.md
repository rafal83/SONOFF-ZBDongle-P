# Récupérer un dongle après une écriture interrompue

1. Garder la sauvegarde initiale et arrêter Zigbee2MQTT.
2. Reconnecter le dongle au [Web Flasher](https://rafal83.github.io/SONOFF-ZBDongle-P/).
3. Relancer l'[installation](firmware/installation.md) ou la [restauration d'un firmware sauvegardé](firmware/restauration.md).
4. Attendre la relecture complète et le CRC final, puis vérifier la reprise dans Zigbee2MQTT.

Le flasher conserve le stockage Zigbee et la configuration CCFG. Après une erreur d'écriture ou de vérification, il n'envoie pas de commande de redémarrage.

Le bootloader ROM reste accessible si sa configuration CCFG et le câblage le permettent. Si l'entrée automatique échoue, vérifier le port USB, l'arrêt des autres applications et la procédure BOOT/RESET du modèle P. Le [protocole TI SWRA466](https://www.ti.com/lit/pdf/swra466) décrit les conditions d'accès au bootloader.

Un stockage réseau endommagé exige une [récupération séparée avec une sauvegarde Zigbee2MQTT](firmware/restauration.md#sauvegarde-du-réseau-zigbee2mqtt). Une ancienne Flash complète ne doit pas remplacer les compteurs actuels. Aucun essai de récupération sur un dongle physique n'est établi par les tests du flasher.
