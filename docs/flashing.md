# Phase F — flash réservé au laboratoire

**Procédure préparée, non exécutée.** Le dongle de production est exclu. Les outils de build et de tests de ce dépôt n'ouvrent aucun port série. Un éventuel arrêt de Zigbee2MQTT de production, flash ou accès à sa NVRAM exigera une autorisation explicite distincte ; elle n'a pas été donnée.

Pour la contrainte de l'unique dongle précisée le 17 novembre, suivre d'abord le [plan de sauvegarde et retour arrière](single-dongle-rollback.md). Les commandes de laboratoire ci-dessous ne constituent pas une autorisation de les appliquer au coordinateur actif.

## Préparer un essai indépendant

Utiliser un ZBDongle-P séparé, identifié par son numéro USB et sa référence, avec uniquement ce dongle exposé à la VM de laboratoire. Aucun passthrough du coordinateur actif dans cette VM. Éviter les ports génériques `/dev/ttyUSB0` qui peuvent changer ; employer le chemin `by-id` propre au dongle test. Sous Windows, vérifier la correspondance du port COM et du numéro USB avant toute ouverture.

Conserver localement le firmware original **de ce dongle test**, son origine, sa version, son SHA-256 et, si disponible, une sauvegarde Flash complète. Une lecture BSL déclenche un reset et n'est donc pas une opération à effectuer sur la production sans autorisation. Une sauvegarde Flash peut contenir des secrets.

Le retour de `tools/image.py check` doit être positif avant un flash de nos images CC2652P. Il vérifie bootloader/backdoor/debug/erase, vecteurs et absence du secteur NV dans l'image. Il ne prouve ni la correspondance du PCB ni la conservation des données par le logiciel de flash. Un effacement global efface la NV même si l'HEX ne contient aucune donnée NV.

## Commandes d'exemple, à exécuter seulement dans ce laboratoire

Le [cc2538-bsl public épinglé](https://github.com/JelmerT/cc2538-bsl/tree/28cce9749bfb73c50363ded01bd8001b1639d9fb) fournit désormais la commande `cc2538-bsl` via son package Python. Ses options sont vérifiées dans `cc2538_bsl/cc2538_bsl.py`. L'installation de l'outil et de pyserial/intelhex est distincte des dépendances des tests PC.

```sh
python tools/image.py check build/coordinator-optimized/sonoff-znp-coordinator.hex
# EXEMPLE DESTRUCTIF POUR LE SEUL DONGLE TEST, non exécuté :
cc2538-bsl --port PORT_EXPLICITE_DU_DONGLE_LAB --bootloader-sonoff-usb --erase --write --verify build/coordinator-optimized/sonoff-znp-coordinator.hex
```

`--erase` effectue un effacement global. Ce choix est réservé à un dongle de laboratoire sauvegardé et sans réseau utile. Ne jamais employer `--disable-bootloader`/`-D`, ni forcer le passage d'un contrôle CCFG échoué, ni écrire l'IEEE de production sur le dongle test.


L'image complète optimisée `build/coordinator-optimized/sonoff-znp-coordinator.hex` est la cible des essais Z2M de laboratoire décrits dans [validation.md](validation.md). La base sans nos optimisations reste dans `build/coordinator/`. L'image séparée `build/sonoff-uart-diagnostic.hex` reste un diagnostic sans Zigbee et ne sert pas aux essais réseau.

## Sauvegardes d'une installation existante : plan pour une intervention future

Aucune copie de votre installation n'a été demandée ou effectuée. Avant une migration expressément autorisée, prévoir un arrêt propre pour obtenir un ensemble cohérent de `coordinator_backup.json`, `database.db` et `configuration.yaml`, plus fichiers de secrets/références associés. Ne pas présumer qu'une copie prise pendant des écritures est cohérente. Inclure les clés Trust Center, clés de liaison et compteurs requis par le mécanisme de restauration utilisé.

Conserver ces fichiers chiffrés et à accès restreint hors Git ; ils sont ignorés par `.gitignore`, mais cela ne protège pas un fichier déjà suivi. Ne pas les joindre à un rapport public ni aux artefacts CI. Un firmware d'origine retrouvé en ligne ne contient pas vos paramètres réseau et ne remplace pas une sauvegarde du coordinateur.
