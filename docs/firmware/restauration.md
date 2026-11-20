# Sauvegardes et restauration

## Restaurer un firmware depuis le navigateur

Cette opération remet le **firmware FreeRTOS sauvegardé** sur le même dongle, en conservant les données Zigbee actuelles, notamment les clés et les compteurs de sécurité.

1. Arrêter proprement Zigbee2MQTT, puis brancher le dongle à l'ordinateur.
2. Ouvrir le [Web Flasher](https://rafal83.github.io/SONOFF-ZBDongle-P/) et choisir **Restaurer un firmware sauvegardé**.
3. Sélectionner la sauvegarde JSON complète téléchargée par le flasher. Elle contient la Flash, son SHA-256 et l'IEEE du dongle.
4. Cocher l'arrêt de Zigbee2MQTT, puis **Connecter et sauvegarder**. Conserver cette nouvelle capture de l'état actuel également.
5. Vérifier le dongle, la révision et le débit affichés, puis cocher la confirmation de restauration et lancer **Restaurer le firmware sauvegardé**.
6. Attendre la relecture complète et le CRC final. Reporter le débit de la sauvegarde dans Zigbee2MQTT, puis vérifier la reprise du réseau.

La sauvegarde doit contenir une application portant l'identifiant FreeRTOS et la version de stockage compatibles, introduits avec cette fonction. Une ancienne image sans cet identifiant est refusée. Les images d'une autre pile sont refusées : conserver leurs anciennes données NV ne garantit pas des compteurs actuels.

Les anciens BIN complets sont acceptés s'ils portent cet identifiant et conservent leur nom d'origine `zbdongle-p-<IEEE>-<date>.bin`. Leur intégrité ne dispose pas d'un SHA-256 indépendant : utiliser le fichier original, conservé dans un emplacement privé. Les fichiers HEX et les sauvegardes réseau Zigbee2MQTT ne sont pas des sauvegardes Flash du flasher.

Le modèle, l'IEEE, la taille, les vecteurs de démarrage, l'identifiant de stockage et la CCFG sont vérifiés avant l'effacement. L'application seule `0x00000–0x47FFF` est restaurée. Toute la zone `0x48000–0x57FFF` doit être identique à son état avant l'opération lors de la vérification finale. Aucun effacement global ou du secteur CCFG n'est effectué ; le protocole suit la [documentation du bootloader TI](https://www.ti.com/lit/pdf/swra466).

## Pourquoi conserver les données réseau actuelles

Réécrire une ancienne Flash complète ramènerait aussi les compteurs NWK et APS sous des valeurs déjà utilisées. Les appareils pourraient rejeter les trames comme des rejeux et des nonces de chiffrement pourraient être réutilisés. Le flasher préserve donc le stockage et ses planchers de compteurs, y compris pendant une restauration.

Si le stockage réseau est lui-même endommagé, cette restauration d'application ne le répare pas. Conserver la capture et utiliser une sauvegarde **native Zigbee2MQTT récente**, avec les vérifications ci-dessous.

## Sauvegarde du réseau Zigbee2MQTT

`coordinator_backup.json` est une sauvegarde logique du réseau ; il ne contient pas le firmware. Conserver aussi `database.db` et `configuration.yaml`, avec les mêmes clés et identifiants réseau.

La reprise par herdsman doit conserver IEEE, PAN, PAN étendu, canal, clé et séquence, graine TCLK, appareils et clés de lien. Les compteurs sortants doivent dépasser les derniers réellement utilisés. Une sauvegarde ancienne ne garantit pas cette condition. Ne pas effacer le store, fabriquer un compteur ou réécrire une ancienne capture complète pour contourner un refus.

Le firmware réserve les compteurs durablement avant émission et vérifie leur liaison à la clé. Un store corrompu n'est pas formaté implicitement. La restauration native `restoreBackup` est testée avec le vrai herdsman sur simulateur : identité et clés inchangées, 130 appareils transmis identiques, 129 clés de lien, compteurs non décroissants. Les appareils absents de la sauvegarde peuvent nécessiter un réapprentissage ; ces essais ne garantissent pas la récupération de chaque réseau réel.

Le flasher ne restaure pas automatiquement `coordinator_backup.json`. Une récupération du stockage ou d'un réseau doit être vérifiée séparément avant de relancer le trafic.
