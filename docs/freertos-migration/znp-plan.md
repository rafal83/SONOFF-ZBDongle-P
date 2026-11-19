# Plan ZNP — rendre la cible FreeRTOS utilisable par Zigbee2MQTT (`adapter: zstack`)

Priorité fixée par l'utilisateur le 18 novembre : un firmware utilisable avec Z2M au plus vite. Le routage multi-sauts et *Update Device* viennent ensuite. Ce document est la feuille de route de travail ; [znp-compat.md](znp-compat.md) garde l'inventaire.

## Méthode

1. **Simulateur hôte** : la pile FreeRTOS portable (MAC, coordinateur, stockage, ZNP) compilée pour le PC, exposée sur un port TCP local, avec radio et Flash simulées.
2. **Oracle = le vrai herdsman 11.2.1** (déjà épinglé dans `.tooling/herdsman-lab`) : `ZStackAdapter` se connecte au simulateur par `tcp://127.0.0.1:port`. Aucune réponse n'est inventée : chaque commande est servie par l'état réel du coordinateur.
3. **Critère d'acceptation** : la séquence du banc `tools/herdsman_lab.cjs` (démarrage « reset », *permit join*, version, IEEE, paramètres réseau, descripteurs des extrémités 1 et 242, sauvegarde, reset, reprise « resumed », compteur de trames non décroissant, boucle AF locale), puis la restauration d'une sauvegarde (`restoreBackup`).
4. Seulement ensuite : même séquence sur le dongle, puis Z2M réel sur un réseau de laboratoire, puis migration.

## Ce que herdsman exige (lu dans `manager.ts`, `adapter-backup.ts`, `adapter-nv-memory.ts`)

| Étape | Commandes |
|---|---|
| Identification | `SYS ping`, `SYS version` (le produit choisit le chemin Z-Stack 3.x), `SYS getExtAddr` |
| NV | `SYS osalNvLength/osalNvReadExt/osalNvWriteExt/osalNvItemInit/osalNvDelete` (items 16 bits) et `SYS nvLength/nvRead/nvWrite/nvCreate` (tables étendues sys 1 : 1 gestionnaire d'adresses, 4 TCLK, 6 clés APS, 7 matériel de sécurité NWK) |
| Stratégie | items `ZNP_HAS_CONFIGURED_ZSTACK3` (96), `NIB` (33), `PRECFGKEY` (98), `NWK_ACTIVE_KEY_INFO` (58), `NWK_ALTERN_KEY_INFO` (59) |
| Formation | écritures `STARTUP_OPTION`, `LOGICAL_TYPE`, `ZDO_DIRECT_CB`, `CHANLIST`, `PANID`, `EXTENDED_PAN_ID`, `APS_USE_EXT_PANID`, `PRECFGKEYS_ENABLE`, `PRECFGKEY` ; `APP_CNF bdbSetChannel` ×2, `bdbStartCommissioning(4)` → `ZDO stateChangeInd(9)` |
| Reprise | `UTIL getDeviceInfo`, `ZDO startupFromApp` → `stateChangeInd(9)`, `ZDO extNwkInfo` |
| Extrémités | `AF register` (1, 242…), `ZDO extFindGroup/extAddGroup` (groupe Green Power) |
| Sauvegarde | NIB, clés, `TCLK_SEED` (257), tables étendues, `PRECFGKEYS_ENABLE` |
| Exploitation | `AF dataRequest/dataRequestExt` → `AF dataConfirm` ; `AF incomingMsg/incomingMsgExt` ; requêtes ZDO et leurs réponses AREQ ; `ZDO endDeviceAnnceInd`, `tcDeviceInd`, `leaveInd`, `concentratorIndCb` ; `SYS stackTune` |

## Décisions de conception

- **Clés de lien uniques à la manière de Z-Stack 3.x** : `clé = rotation(graine, décalage) XOR IEEE inversée répétée`. La graine (`TCLK_SEED`) et le décalage par appareil sont ce que herdsman sauvegarde et restaure. Conséquence : sauvegardes Z2M valides et **reprise des 75 appareils de production sans réappairage**.
- **NV de façade** : petits items de configuration stockés tels quels dans `nv_log` ; NIB, clés et tables étendues **synthétisés depuis l'état réel** du coordinateur en lecture et **appliqués à cet état** en écriture. Les dispositions binaires sont vérifiées par les classes `Structs` de herdsman dans les tests.
- **Produit annoncé** : la séquence d'acceptation est passée sur le simulateur puis sur le dongle ; le produit 1 (dialecte Z-Stack 3.x.0) est annoncé, et les écarts sont listés dans [znp-compat.md](znp-compat.md).

## Étapes

| # | Contenu | État |
|---|---|---|
| Z1 | Simulateur TCP hôte + harnais Node herdsman | **Fait** |
| Z2 | SYS/UTIL/NV de base, items simples dans `nv_log` | **Fait** |
| Z3 | NIB, clés, matériel de sécurité, tables étendues synthétisés | **Fait** |
| Z4 | APP_CNF formation, ZDO startupFromApp/stateChangeInd/extNwkInfo | **Fait** |
| Z5 | AF register/dataRequest/incomingMsg, ZDO requêtes ↔ réponses AREQ, groupes | **Fait** |
| Z6 | Séquence d'acceptation complète sur simulateur, puis sur le dongle | **Fait** |
| Z7 | `restoreBackup` depuis une sauvegarde, puis migration de laboratoire | **Fait** en simulateur et sur le dongle ; répétition sur la sauvegarde de production réussie ([migration.md](migration.md)) |
