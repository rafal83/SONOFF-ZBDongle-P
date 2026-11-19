# Essais physiques du 17 novembre 2026

Avec Z2M arrêté et le dongle connecté au PC, l'utilisateur a autorisé les essais suivis du retour au firmware d'origine. Aucun accès à Home Assistant/Proxmox ni lancement de Z2M sur ce PC n'a été effectué.

Le candidat à **115200 bauds fixes** a réussi sur le véritable CC2652P :

- Démarrage ZNP `20261114`, lecture version/capacités et identité IEEE conservée.
- Dernier candidat, avec conservation du cache NV indépendant : 300 SYS_PING, moyenne 4,131 ms, minimum 3,760 ms, maximum 4,516 ms. Ces temps incluent USB et hôte ; ils ne mesurent pas le CPU Zigbee.
- 20 cycles d'écriture/lecture d'un objet NV de 64 octets, puis lecture identique après reset logiciel. Les cycles réutilisent la même valeur ; ce n'est pas une preuve de vingt programmations Flash physiques.
- Reprise après 300 octets `FE`, préfixe tronqué et FCS invalide.
- Restauration complète des 360 448 octets d'origine, NV et CCFG comprises : CRC ROM et relecture intégrale identique à la sauvegarde immédiatement antérieure à l'essai. L'original `20240710` répond ensuite à dix pings à 115200 bauds.

L'image expérimentale utilisait une NV vierge, sans charger le réseau de production. Les premiers essais étaient limités à SYS/NV ; les séances suivantes ont formé un réseau temporaire sans ouvrir les associations. Les sauvegardes et données sensibles restent sous `private/dongle-20261117/`, exclu de Git et des archives. Chaque essai a son propre instantané : l'ouverture/fermeture du pont USB peut redémarrer l'application d'origine et faire évoluer sa NV, d'où deux captures dans la même connexion ROM que l'effacement.

Le changement automatique 115200 → 460800 a échoué lors des essais précédents, malgré la réussite des modèles logiciels. Les ajustements de routage DMA et du préfixe à quatre fronts n'ont pas suffi. **À la demande de l'utilisateur, cette fonction est reportée et désactivée par défaut.** Son code et ses tests restent disponibles via `--experimental-autobaud`, dans un paquet distinct non qualifié. Les profils fixes 460800 et 1000000 ont depuis passé les essais série/NV ; le profil 1000000 passe aussi les échanges radio et la charge NV concurrente. Voir [performance.md](performance.md).


Après le signal de restitution, rebrancher le dongle à son emplacement habituel et démarrer Z2M avec ses données et réglages existants. Aucun effacement, réappairage ou changement de débit n'est requis pour revenir à l'original.

## Démarrage et restauration par le vrai moteur herdsman

`zigbee-herdsman` 11.2.1 est installé séparément sous `.tooling/herdsman-lab`, avec les vingt dépendances verrouillées dans `tools/herdsman-lab-lock.json` et leurs intégrités npm. Aucun Z2M ni broker MQTT n'est installé ou lancé. Le rapport conserve les empreintes du harnais, du verrou et des vrais modules `ZStackAdapter`, `manager` et `Znp` exécutés.

Lors de la première qualification locale, sur le candidat HEX `eebe2d2f36a6e798cd69367e5220a83f7e22bcd226cdadcf933b6d1f218a9f70`, le moteur a réellement réussi :

- `start()` retourne `reset` et forme un réseau temporaire, clé/PAN aléatoires, canal 25, puissance demandée 0 dBm.
- Enregistrement de 15 endpoints et du groupe Green Power ; fermeture explicite des associations sur le coordinateur.
- Descripteur de nœud coordinateur, endpoints actifs et descripteurs simples des endpoints 1 et 242 via ZDO.
- Sauvegarde réelle des tables NV, conversion au format de sauvegarde unifié, validation de la clé du réseau de test.
- Reset logiciel puis second `start()` retournant `resumed` : identité, paramètres et clé conservés, compteur de sécurité non décroissant.
- Envoi ZCL vers le coordinateur lui-même : succès AF dataConfirm et réception du payload attendu par l'hôte. Cet échange local valide AF/APS et le transport, sans prouver une émission/réception radio entre deux appareils.
- Effacement de la configuration expérimentale par le gestionnaire herdsman, puis troisième `start()` retournant `restored` depuis la sauvegarde privée. Clé et paramètres identiques, compteur de sécurité non décroissant après restauration.
- Restauration finale de l'original `20240710` et de sa NV, relecture intégrale identique, puis dix pings et lecture de version réussis.

Pour préparer le même runtime sans accès matériel : `python tools/setup_herdsman_lab.py`, puis `node tools/herdsman_lab.cjs --check-runtime`. Une séance matérielle explicitement autorisée utilise `tools/run_dongle_session.py --herdsman-lab` avec les identifiants et sauvegardes du dongle. Ce n'est pas une commande à lancer sur un coordinateur actif. Les paramètres réseau, clés, logs bruts et sauvegardes restent privés ; `tools/export_hardware_evidence.py` n'exporte que les mesures autorisées et vérifie les fichiers de retour arrière.

## Extension radio de la qualification

Le candidat courant `bd33bf77ff3c1e4a713ee4b35783f087a6dfc94b9945ef19c8b16fa501ef6d57` comprend les compteurs de diagnostic et a passé les échanges avec une HG06106C hors production. La preuve publique utilise ses propres essais série/NV (séance 009) et radio (011), sans hériter de résultats du HEX précédent. Le [rapport radio](radio-lab.md) détaille les commandes, les tentatives, la reprise NV expérimentale et les limites.
