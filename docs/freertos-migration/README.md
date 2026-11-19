# Migration FreeRTOS — état

Branche `freeRTOS`. Coordinateur Zigbee natif sous FreeRTOS pour le CC2652P, indépendant de TI-RTOS7 et de Z-Stack. Il expose l'interface ZNP attendue par Zigbee2MQTT (`adapter: zstack`) et a été construit à côté du firmware Z-Stack installé, sans le modifier.

| Phase | Document | État |
|---|---|---|
| A — faisabilité | [feasibility.md](feasibility.md) | Terminée |
| B — noyau et périphériques | [architecture.md](architecture.md) | **Validé sur le dongle** (démarrage, horloges, UART MT, FreeRTOS, watchdog, diagnostic) : [essais du 18 novembre](hardware-session-20261118.md) |
| C — radio 802.15.4 | [radio.md](radio.md) | **Réception et émission validées sur le dongle** (CSMA, ACK, *source matching*) ; **PA 20 dBm validé** (séance 10, après correction du lien d'overrides TX20) |
| D/E — pile Zigbee | [zigbee-stack.md](zigbee-stack.md) | **Coordinateur complet côté logiciel** : arrivée, Trust Center, clés de lien uniques à la Z-Stack (graine + décalage), persistance, routage maillé (concentrateur), devoirs de routeur. Arrivée, clé unique, persistance et pilotage **validés sur un appareil réel** ; maillage multi-sauts validé en simulation |
| F — ZNP | [znp-compat.md](znp-compat.md) | **herdsman 11.2.1 : recette de 17 étapes réussie sur simulateur et sur le dongle** ; appairage et pilotage d'un appareil réel via le `Controller` herdsman |
| G — validation | [validation.md](validation.md) | 17 programmes hôtes, 68 vérifications ARM avec profondeurs de pile, analyse statique des piles à chaque build, recette herdsman, séances matérielles |
| Migration | [migration.md](migration.md) | **Production migrée le 18 novembre** ; image `969fb13` en service ([status.md](status.md#production)) |
| Retour arrière | [rollback.md](rollback.md) | Action `rollback` sans régression de compteur (écrite, testée hors matériel, jamais exécutée) ; `restore` octet pour octet refusé après exploitation |
| Stabilisation | [stabilization-20261119.md](stabilization-20261119.md) | Audit AES matériel, mémoire et piles, maillage sous congestion et redémarrages, sécurité du stockage : **simulation seulement** |
| Recette matérielle | [hardware-campaign.md](hardware-campaign.md) | Deux routeurs et un endormi, mesures DWT, latences, CPU, mémoire, Z2M : **à exécuter** |
| **État détaillé** | [status.md](status.md) | Chaque fonction : implémentée / testée en simulation / validée sur matériel / observée en production ; mesures |

**Artefacts** : `build/freertos/diag-115200/` et `build/freertos/diag-1000000/` contiennent `sonoff-freertos-diag.hex` (le nom est historique). Leur manifeste décrit honnêtement le niveau de validation et les conditions d'usage en production.

Commandes :

```powershell
python tools/build_freertos.py all                      # tests hôte + ARM, simulateur, images 115200 et 1000000
node tools/znp_sim_acceptance.cjs                       # recette herdsman sur le simulateur
node tools/znp_sim_acceptance.cjs --serial COM8         # même recette sur le dongle (image 115200)
node tools/znp_pair_test.cjs --serial COM8              # appairage réel via le Controller herdsman
node tools/znp_migration_rehearsal.cjs --backup <...>   # répétition de migration (sans matériel)
```

## GitHub Actions

Le workflow [`build-freertos`](../../.github/workflows/freertos.yml) compile cette branche sur **Ubuntu 24.04** à chaque push et pour les pull requests vers `freeRTOS`. Il exécute les tests hôtes et ARM, construit le simulateur, puis génère les images **115200** et **1000000 bauds**. La CI utilise Zig et les sources du SDK TI, sans exécuter les outils TI Windows et sans utiliser de dongle.

Ce workflow unique vérifie aussi les algorithmes partagés et le cadrage NPI. Il remplace les anciens workflows de diagnostic UART et de compilation Z-Stack Windows sur cette branche.

Après validation d'un push sur `freeRTOS` ou d'un lancement manuel sur cette branche, il publie une **prérelease GitHub** sous le tag `freertos-<12 premiers caractères du commit>`, avec `sonoff-freertos-experimental.zip` et son fichier `.sha256`. Les pull requests exécutent les tests sans publier de release. La publication reprend un brouillon interrompu et conserve une prérelease déjà publiée lors d'une relance du même commit. Les manifestes et la documentation inclus dans l'archive décrivent le niveau réel de validation matérielle et les conditions de migration en production.

## Flasher depuis le navigateur

Le [Web Flasher](https://rafal83.github.io/SONOFF-ZBDongle-P/) est publié sur GitHub Pages après les tests et la release. Le workflow conserve aussi l'artefact téléchargeable `web-flasher-<commit>`, avec les deux débits et leurs licences. La page vérifie le SHA-256 du firmware, identifie le CC2652P et sauvegarde toute sa Flash localement avant de permettre l'installation. Chrome ou Edge sur ordinateur et un dongle connecté en USB à cet ordinateur sont nécessaires ; Zigbee2MQTT doit être arrêté pour libérer le port.

L'écriture est limitée à `0x00000–0x47FFF`. Le stockage FreeRTOS, la NV Z-Stack et la CCFG (`0x48000–0x57FFF`) sont conservés et relus pour vérification. Une CCFG différente de celle des images empêche l'installation. La page n'effectue ni effacement global, ni migration automatique du réseau Z-Stack, ni restauration des compteurs : pour changer de pile, suivre la [procédure de migration](migration.md). La sauvegarde BIN peut contenir des clés Zigbee : elle reste locale et doit être conservée dans un endroit privé.

Les tests du flasher utilisent un simulateur du protocole ROM et ne qualifient pas le flash Web Serial sur le matériel. Le résultat affiché après installation confirme la relecture Flash et le redémarrage demandé, pas le fonctionnement radio ni le démarrage de Zigbee2MQTT.

### Prévisualisation locale

Après un build et `python tools/package_freertos_ci.py`, préparer le site avec l'identité complète du commit ayant servi au build :

```powershell
python tools/package_web_flasher.py --commit <SHA-complet-du-commit>
python -m http.server 8080 --directory build/web-flasher --bind 127.0.0.1
```

Ouvrir `http://127.0.0.1:8080` dans Chrome ou Edge. Un fichier HTML ouvert directement avec `file://` ne permet pas le chargement du catalogue. L'artefact CI `web-flasher-<commit>` contient aussi le site complet prêt à servir, sans installation de dépendances JavaScript.

### Déploiement GitHub Pages

Pages utilise **GitHub Actions** comme source et la branche `freeRTOS` est autorisée dans l'environnement `github-pages`. La variable Actions `WEB_FLASHER_PAGES_ENABLED=true` active le job `pages` après réussite du build et de la release. Pour suspendre la publication du site, supprimer cette variable ou la passer à `false` ; les builds et releases continuent alors indépendamment de Pages.
