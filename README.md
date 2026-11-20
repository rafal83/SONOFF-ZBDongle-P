# SONOFF ZBDongle-P — coordinateur FreeRTOS

Projet indépendant pour le **SONOFF ZBDongle-P / TI CC2652P**, avec une pile Zigbee en sources et une interface ZNP pour Zigbee2MQTT. Aucune affiliation ni certification de produit n'est revendiquée.

Les objectifs d'optimisation s'inspirent de l'idée présentée par SMLIGHT ZeroStack ; cette inspiration ne constitue pas un portage de son firmware.

## État et utilisation

La branche `freeRTOS` est la cible maintenue. Consulter [son état détaillé](docs/firmware/status.md), [les validations](docs/firmware/validation.md) et [les limites de compatibilité](docs/firmware/znp-compat.md) avant l'installation. Un build et des tests sur simulateur ne qualifient pas une image sur le matériel.

Le [flasher USB](https://rafal83.github.io/SONOFF-ZBDongle-P/) est disponible sur GitHub Pages, en sources et en artefact CI. Voir [son utilisation](docs/firmware/README.md#flasher-depuis-le-navigateur).

## Comparatif

Valeurs au **20 novembre 2026**. Ce résumé est tenu à jour avec [le détail, les conditions et la méthode](docs/firmware/benchmarks.md).

**Réseau réel :** dongle de production, 130 appareils, lecture d'un attribut sur chaque routeur.

| | Notre firmware | Z-Stack Koenkk 20250321 | ZeroStack (SMLIGHT) |
|---|---|---|---|
| Rafale sur tous les routeurs : réponses des routeurs joignables | **100 %** (8 rafales sur 8 à 28 / 28) | **100 %** (28 / 28) | non mesuré (binaire fermé) |
| Lecture isolée, médiane | **34,4 ms** (p95 à 60,4 ms) | 51 ms | non mesuré |

**CPU :** mesuré sur le dongle ou dans l'émulateur. Les chiffres de ZeroStack et du « Z-Stack stock » sont ceux publiés par SMLIGHT ; ils n'ont pas été reproduits.

| | Notre firmware | ZeroStack (publié) | Z-Stack « stock » (selon SMLIGHT) |
|---|---|---|---|
| Sécurité d'une trame (CCM\*), sur le dongle | ≈ 1 650 cycles en trafic réel, contrôles logiciels compris (1 950 au banc) | ≈ 3 900 cycles | 532 182 cycles |
| Écho de broadcast (1 000 échos, 50 routeurs) | **≈ 53 ms** au total | 151 ms | 3 s |
| Trame rejouée | 2 474 cycles | **1 550 cycles** | 143 802 cycles |
| Écritures avant compaction, 200 appareils | 405 | **1 013** (store vide) | 69 |
| Coupures de courant testées, 0 perte | **27 980** | 22 959 | — |
| Bruit UART : reprise | 40 ms | 3 à 55 ms | 8,84 s |
| Tas dynamique | **aucun** | oui | oui |

**Débit soutenu : SIMULATION, pas une mesure physique.** La vraie pile tourne face à un canal 802.15.4 temporisé, 28 routeurs simulés et un modèle de herdsman. Voir [la méthode et les conditions](docs/firmware/benchmarks.md#5-débit-soutenu-et-appareils-endormis--simulation).

| | Firmware FreeRTOS (simulation) | ZeroStack (publié) |
|---|---|---|
| Messages entrants (rapports), 28 routeurs à 1 saut | **190 /s soutenus** (≥ 99,8 % livrés, p99 ≈ 60 ms, CPU ≈ 3 %), ≈ 195 /s au plafond ; ≈ 110 /s dans un arbre à 3 niveaux | « 180 messages/s », conditions non publiées |
| Transactions complètes (lecture ZCL aller-retour), débit stable | 53 /s (arbre à 3 niveaux) à 103 /s (1 saut) : limite de l'air et de la fenêtre de 16 de herdsman, pas du CPU | non publié |
| 20 appareils endormis, parent perdu | 98,5 % des lectures servies, réassociation par le coordinateur | non publié |

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
