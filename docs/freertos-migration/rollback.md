# Retour arrière et protection de la production

## Règles

- Aucun outil de ce dépôt ne programme le firmware FreeRTOS automatiquement : chaque programmation est une commande explicite, et la migration de production exige `--confirm-production-migration`.
- Son image ne contient aucun octet dans la NV de production `0x50000–0x55FFF`. Le seul code d'écriture Flash, `hal/hal_flash.c`, est borné à son propre store `0x48000–0x4FFFF` ; le build vérifie qu'aucun autre fichier n'appelle les fonctions d'écriture.
- Son CCFG est identique à celui de 2026111802 : bootloader ROM actif, backdoor DIO15 active basse, accès debug conservé.

## Pourquoi une simple restauration ne suffit plus après une migration

Le firmware FreeRTOS ne touche pas la NV Z-Stack : après la migration, elle contient toujours l'état du réseau **au moment de la capture**, compteur de trames NWK compris. Pendant ce temps, le coordinateur FreeRTOS a émis sous la même clé avec des compteurs plus élevés.

Réécrire la capture octet pour octet ferait repartir Z-Stack sous ces valeurs déjà utilisées. Les appareils, qui mémorisent le dernier compteur reçu, rejetteraient alors ses trames comme des rejeus, jusqu'à ce qu'il rattrape plusieurs heures de trafic. Et des nonces CCM\* seraient réutilisés sous la même clé. Z-Stack n'ajoute au démarrage qu'une petite marge, très insuffisante.

L'action `restore` de `tools/freertos_lab_session.py` **refuse** donc désormais de réécrire la capture si le store FreeRTOS porte un réseau. L'option `--accept-counter-regression` la débloque, pour un réseau de laboratoire seulement.

## Retour arrière de la production : action `rollback`

**Ne s'exécute qu'avec l'accord explicite du propriétaire du réseau** (`--confirm-production-rollback`).

1. Arrêter Zigbee2MQTT **proprement**.
2. Lire une sauvegarde fraîche **depuis le dongle FreeRTOS**, en lecture seule (aucune commande d'écriture ZNP n'est autorisée par l'outil) :

   ```powershell
   node tools/production_nv_snapshot.cjs --port COM8 --baudrate 1000000 --expected-revision <révision FreeRTOS> `
       --expect-ieee 0x00124b00256c52ef --database <copie de database.db> `
       --old-backup <copie de coordinator_backup.json> --output private/<dossier>/fresh_backup.json
   ```

   L'outil vérifie que l'identité, le PAN, la clé et la graine sont inchangés et que les compteurs ne décroissent pas.
3. Lancer le retour arrière dans la foulée (la sauvegarde doit avoir moins de 2 h ; le coordinateur continue d'émettre tant qu'il tourne) :

   ```powershell
   python tools/freertos_lab_session.py rollback --port COM8 --usb-serial <série> --expect-ieee 0x00124b00256c52ef `
       --session-dir private/migration-20261118/zstack-before --fresh-backup private/<dossier>/fresh_backup.json `
       --z2m-stopped --confirm-production-rollback
   ```

   L'outil :
   - capture deux fois la Flash ; il lit dans le store FreeRTOS la borne réservée du compteur NWK et **refuse** si le compteur de la sauvegarde + 2 500 (incrément de herdsman à la restauration) ne la dépasse pas ;
   - vérifie que la NV Z-Stack et le CCFG sont identiques à la capture, puis réécrit **l'application Z-Stack seule**, avec relecture ;
   - vérifie que Z-Stack 2026111802 répond avec la bonne IEEE ;
   - par des commandes ZNP standard, celles qu'utilise herdsman, fixe l'option de démarrage « effacer configuration et état », supprime le marqueur « configuré » puis redémarre ; il vérifie ensuite que l'adaptateur n'est plus configuré.
4. Remplacer `coordinator_backup.json` de Z2M par `fresh_backup.json`, puis démarrer Z2M **sans changer sa configuration**. Attendu : stratégie `restoreBackup`, puis réseau `restored`, avec un compteur au-delà du dernier utilisé par FreeRTOS. Aucun appareil à réappairer.

État : outil écrit et testé hors matériel (analyse du store sur un store écrit par le code C ; refus de `restore`). **Jamais exécuté sur le dongle.**

## Revenir au firmware Z-Stack sur un dongle de laboratoire

1. Entrer dans le bootloader ROM (auto-BSL SONOFF par DTR/RTS, ou BOOT + RESET).
2. Programmer l'image Z-Stack voulue, avec vérification :
   - optimisée installée : `build/coordinator-optimized-1000000-cores-sdk833-r2/sonoff-znp-coordinator.hex` ;
   - repli : `build/coordinator-optimized-1000000-next/sonoff-znp-coordinator.hex`.
3. Si la Flash complète avait été sauvegardée avant l'essai, et si le réseau de cette sauvegarde n'a **pas** tourné sur FreeRTOS entre-temps, la restaurer et vérifier son SHA-256.

## Références

Voir aussi [sdk-migration.md](../sdk-migration.md), [single-dongle-rollback.md](../single-dongle-rollback.md) et [migration.md](migration.md).
