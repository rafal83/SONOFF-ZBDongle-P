# Essais avec l'unique coordinateur : retour arrière préalable

17 novembre 2026. L'utilisateur dispose uniquement du coordinateur de ses 74 appareils et demande de pouvoir remettre son firmware d'origine après les essais. Ce document prépare ce scénario. Aucun arrêt, accès série, reset, lecture NV ou flash de ce dongle n'est effectué ni autorisé par la seule préparation de ce plan.

## Ce qui doit être sauvegardé

Le firmware et le réseau sont deux éléments distincts. Remettre un HEX d'origine ne restaure pas les clés, compteurs, tables ou données de Zigbee2MQTT. Une sauvegarde Home Assistant/Proxmox ne contient pas automatiquement la Flash du microcontrôleur USB.

Le dossier de retour doit contenir :

- Une sauvegarde récente du coordinateur via le mécanisme Z2M/zstack, avec identité IEEE et paramètres réseau cohérents. Ne pas afficher les clés dans les rapports.
- Une copie cohérente du dossier de données Z2M, dont `coordinator_backup.json`, `database.db`, configuration et fichiers de secrets référencés ; conserver la version exacte Z2M/herdsman.
- Une lecture complète de la Flash principale du même CC2652P : `0x00000` à `0x57fff`, soit 360 448 octets, incluant application, NV et CCFG. La possibilité de cette lecture dépend de l'accès ROM et des protections du dongle réel.
- Les empreintes SHA-256, le port/numéro USB confirmé, la révision installée et la méthode de restauration. Le `20240710` précédemment déclaré reste à confirmer. Le firmware de référence `20250321` déjà téléchargé dans ce projet n'est pas une sauvegarde du dongle actuel.

Ces données contiennent des secrets. Les conserver en espace privé à accès restreint, avec une deuxième copie protégée hors du dossier de travail, jamais dans Git ni dans les archives publiques du firmware.

## Séquence à préparer avant autorisation d'intervenir

1. Préparer hors ligne les outils BSL épinglés, pilotes, emplacements privés, image expérimentale et procédure de récupération. Identifier le numéro USB sans ouvrir son port. Vérifier l'accès exclusif envisagé entre Home Assistant/Proxmox et le poste d'essai.
2. Prévoir une fenêtre d'indisponibilité : le coordinateur unique ne peut servir simultanément la production et les essais. L'utilisateur autorisera explicitement l'arrêt de Z2M et la sauvegarde nécessitant un reset avant leur exécution. Désactiver temporairement les redémarrages automatiques qui reprendraient le port pendant cette fenêtre.
3. Produire la sauvegarde logique récente, arrêter proprement Z2M et obtenir une copie cohérente de ses données. Ne pas présumer qu'un ancien `coordinator_backup.json` correspond encore à la Flash actuelle.
4. Lire la Flash principale sans effacement ni programmation. Vérifier le modèle et la taille réellement annoncés ; comparer deux lectures complètes et leurs SHA-256. Contrôler CCFG et la méthode de retour ROM/debug. Si une protection empêche la lecture ou si les copies diffèrent, ne pas flasher l'image expérimentale. Ne pas déverrouiller par effacement pour obtenir une sauvegarde.
5. Présenter le résultat concret : sauvegardes présentes et vérifiées, empreintes, procédure de retour et limites constatées. La preuve de lisibilité et les contrôles logiciels ne constituent pas encore un essai de restauration réussi. Le premier flash expérimental nécessite une autorisation explicite portant sur ce dongle de production.

L'option de lecture du cc2538-bsl épinglé est `--read --len 360448 --address 0`, avec port explicite et entrée ROM SONOFF. C'est une référence pour la procédure future, pas une commande exécutée. Le parseur attend une longueur décimale ; son défaut de 512 Kio dépasserait ce modèle de 352 Kio. La lecture fait entrer le dongle en ROM et interrompt donc son activité Zigbee.

Le vérificateur `tools/image.py check` interdit intentionnellement les données NV dans une image applicative neuve. Il ne faut pas l'appliquer tel quel à une sauvegarde Flash complète contenant précisément la NV à préserver.

## Limiter les premiers essais

Commencer avec une instance Z2M distincte et un réseau de test vierge, canal/PAN/clés distincts de la production. Ne pas laisser le firmware expérimental reprendre automatiquement les paramètres de production présents en NV. Leur sauvegarde doit être terminée avant toute remise à zéro autorisée destinée au laboratoire.

Ne pas réinitialiser ni réassocier un appareil de production pour le seul essai. Sans appareil Zigbee de réserve, les vérifications restent limitées au démarrage, au dialogue Z2M et à la formation du réseau : les associations et échanges ne seront pas entièrement validés.

Éviter de faire émettre le firmware expérimental avec les clés et l'identité du réseau de production avant de restaurer un ancien état : les compteurs de sécurité auraient alors avancé chez les appareils et un simple retour d'octets pourrait les faire reculer. Des essais sur le vrai réseau demanderaient une procédure distincte de compteurs et de restauration, pas une promesse de retour par simple copie.

## Retour au service

Restaurer le firmware exact et l'état de réseau du même dongle avec une méthode validée pour son format ; vérifier l'écriture et la lecture de retour. Ne pas écrire dans FCFG ni modifier son identité d'usine. Une image générique du même nom de version n'est pas forcément identique au binaire sauvegardé.

Remettre l'accès USB et les données Z2M d'origine, conserver la version hôte, puis vérifier IEEE, canal, PAN, démarrage et échanges avec plusieurs routeurs et appareils endormis à leur réveil. Le retour sans réassociation est l'objectif, pas une garantie établie pour notre firmware expérimental. En cas d'incohérence, conserver les sauvegardes et analyser l'échec avant de réinitialiser des appareils.

Sources : [sauvegarde/restauration et stockage partagé du réseau dans la FAQ Z2M](https://www.zigbee2mqtt.io/guide/faq/), [outils de flash et sauvegarde zstack](https://www.zigbee2mqtt.io/guide/adapters/zstack.html), [cc2538-bsl épinglé](https://github.com/JelmerT/cc2538-bsl/blob/28cce9749bfb73c50363ded01bd8001b1639d9fb/cc2538_bsl/cc2538_bsl.py).

## Séance exécutée le 17 novembre

Après autorisation explicite et confirmation de l’arrêt de Z2M, le dongle a été testé sur le PC. Deux captures complètes identiques, vérifiées par CRC ROM, ont précédé chaque effacement dans la même connexion ROM. L’original 20240710 et sa NV ont été restaurés et relus intégralement ; son redémarrage a été vérifié. Les sauvegardes sont dans `private/dongle-20261117/`. Aucune copie des fichiers Z2M ni accès HA/Proxmox n’a été réalisé. Voir le [rapport des essais](hardware-validation-20261117.md).
