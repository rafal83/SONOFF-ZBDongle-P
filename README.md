# SONOFF ZBDongle-P — coordinateur FreeRTOS

Projet indépendant pour le **SONOFF ZBDongle-P / TI CC2652P**, avec une pile Zigbee en sources et une interface ZNP pour Zigbee2MQTT. Aucune affiliation ni certification de produit n'est revendiquée.

Les objectifs d'optimisation s'inspirent de l'idée présentée par SMLIGHT ZeroStack ; cette inspiration ne constitue pas un portage de son firmware.

## État et utilisation

La branche `freeRTOS` est la cible maintenue. Consulter [son état détaillé](docs/firmware/status.md), [les validations](docs/firmware/validation.md) et [les limites de compatibilité](docs/firmware/znp-compat.md) avant l'installation. Un build et des tests sur simulateur ne qualifient pas une image sur le matériel.

Le [flasher USB](https://rafal83.github.io/SONOFF-ZBDongle-P/) est disponible sur GitHub Pages, en sources et en artefact CI. Voir [son utilisation](docs/firmware/README.md#flasher-depuis-le-navigateur).

## Construire et tester

Python 3.13, Git, Node 22 et Zig 0.13.0 sont utilisés par la CI Ubuntu 24.04. Les commandes suivantes construisent et testent sans ouvrir de port série :

```sh
python -m pip install --target .tooling ziglang==0.13.0
python -m pip install --require-hashes --only-binary=:all: --target .tooling/arm-tests -r tools/arm-tests-requirements.txt
python -m pip install -r tools/host-tests-requirements.txt -r tools/core-tests-requirements.txt
python tools/fetch_sources.py FreeRTOS-Kernel Z-Stack-firmware zigbee-herdsman
python tools/download_ti_toolchain.py sdk833 --extract --device-support-only
python tools/audit_distribution.py --check-sdk
python tools/build_freertos.py all
python -m unittest discover -s tests -v
python tools/setup_herdsman_lab.py
node tools/znp_sim_acceptance.cjs
python tools/package_freertos_ci.py
```

Le paquet contient les images **115200** et **1000000 bauds**, les manifestes, les rapports et les licences. Les bibliothèques fermées, outils hôtes, SDK complet et sauvegardes ne font pas partie de ce paquet.

## Licences

Les contributions originales sont sous [MIT](LICENSE). Les dépendances conservent leurs propres droits et conditions : [attributions](NOTICE.md), [inventaire](docs/dependencies.md), [textes des licences](LICENSES/).

## Organisation

- `firmware/freertos/` : pile, pilotes et tests de la cible courante.
- `firmware/src/` : algorithmes partagés et transport MT.
- `tests/` : contrôles Python, JavaScript et modèles de test.
- `tools/` : [outils de build, tests et sauvegarde](tools/README.md).
- `web-flasher/` : interface locale Web Serial.
- `LICENSES/` : licences tierces et attributions.
- `private/` : sauvegardes et données personnelles, hors Git et hors distribution.
- `upstream/`, `vendor-sdk/`, `.tooling/`, `build/` : dépendances et sorties locales ignorées.

Consulter les procédures d'[installation et mise à jour](docs/firmware/installation.md) et de [restauration depuis une sauvegarde](docs/firmware/restauration.md). Aucun build ni test logiciel ne déclenche un flash ou une action sur le réseau Zigbee.
