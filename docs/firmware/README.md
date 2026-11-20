# Coordinateur FreeRTOS pour SONOFF ZBDongle-P

Firmware Zigbee natif pour le CC2652P, avec une interface ZNP compatible avec Zigbee2MQTT (`adapter: zstack`). La branche maintenue est `freeRTOS`.

- [Installation et mise à jour](installation.md).
- [Sauvegardes et restauration](restauration.md).
- [État et limites](status.md), [validation](validation.md), [compatibilité ZNP](znp-compat.md).
- [Architecture](architecture.md), [radio](radio.md), [pile Zigbee](zigbee-stack.md).
- [Essais matériels datés](hardware-session-20261118.md), [campagne matérielle](hardware-campaign.md), [stabilisation](stabilization-20261119.md).

Les essais matériels concernent les images et dates indiquées dans leurs rapports. Ils ne qualifient pas automatiquement chaque nouvelle prérelease.

## Flasher depuis le navigateur

Le [Web Flasher](https://rafal83.github.io/SONOFF-ZBDongle-P/) permet d'installer la dernière version ou de restaurer le firmware d'une sauvegarde. Il fonctionne dans Chrome ou Edge sur ordinateur, avec un ZBDongle-P branché en USB et Zigbee2MQTT arrêté.

Une sauvegarde complète est téléchargée avant chaque écriture. Seule l'application `0x00000–0x47FFF` est programmée : le stockage Zigbee, ses compteurs et la configuration `0x48000–0x57FFF` restent inchangés. La Flash complète est relue et son CRC vérifié avant le redémarrage. Voir les [conditions de restauration](restauration.md).

## Construire et tester

Après installation des [dépendances](../../README.md#construire-et-tester) :

```sh
python tools/build_freertos.py all
node tools/znp_sim_acceptance.cjs
node --test tests/web_flasher.test.mjs
python -m unittest discover -s tests -v
python tools/package_freertos_ci.py
```

Les sorties `build/freertos/diag-115200/` et `diag-1000000/` contiennent les images, ELF et manifestes. Le nom `diag` est historique ; le manifeste décrit la validation de chaque image.

## GitHub Actions et publication

Le [workflow unique](../../.github/workflows/freertos.yml) construit et teste sous Ubuntu 24.04, sans dongle. Chaque push validé sur `freeRTOS` publie une prérelease `freertos-<12 premiers caractères du commit>` avec l'archive vérifiée, les rapports et les licences. Les pull requests ne publient pas.

GitHub Pages publie ensuite le flasher depuis l'environnement `github-pages` lorsque `WEB_FLASHER_PAGES_ENABLED=true`. Passer cette variable à `false` suspend Pages ; les builds et releases continuent.

Pour préparer une prévisualisation locale après le packaging :

```sh
python tools/package_web_flasher.py --commit <SHA-complet-du-commit>
python -m http.server 8080 --directory build/web-flasher --bind 127.0.0.1
```

Ouvrir `http://127.0.0.1:8080`. Les tests ROM et navigateur utilisent un dongle simulé ; ils ne constituent pas une validation du flash USB sur le matériel.
