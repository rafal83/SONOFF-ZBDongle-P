# Attributions et périmètre des licences

Ce dépôt est un projet indépendant pour le SONOFF ZBDongle-P / TI CC2652P. Les noms SONOFF, Texas Instruments, FreeRTOS, Zigbee et zigbee-herdsman identifient les matériels, protocoles et dépendances concernés ; aucune affiliation, approbation ou certification de produit n'est revendiquée.

La licence MIT à la racine couvre les contributions originales de ce projet. Elle ne remplace aucune licence tierce et ne s'applique pas globalement au SDK TI ni aux firmwares de tiers. La notice de copyright historique est conservée.

- **FreeRTOS-Kernel V11.1.0** : licence MIT livrée par le projet FreeRTOS. Le texte exact est inclus dans chaque paquet firmware.
- **TI CC13x2/CC26x2 device support** : sources de démarrage, driverlib et patch radio fournies dans le SDK TI 8.33.00.16. Leurs notices BSD sont conservées dans les sources et reproduites dans les paquets. Voir `LICENSES/TI-BSD-3-Clause.txt`, les notices de chaque fichier lié et le manifeste TI inclus.
- **Configuration radio** : données des outils TI SysConfig/SmartRF et adaptations de tables de puissance provenant du patch Koenkk. Les contributions du projet ne remplacent pas les droits et notices de ces sources.
- **Koenkk / Z-Stack-firmware** : Copyright (c) 2018-2021 Koen Kanters, licence MIT reproduite dans `LICENSES/Koenkk-MIT.txt`. Cette licence concerne les contributions du dépôt Koenkk, pas toutes les bibliothèques TI.
- **zigbee-herdsman** : Copyright (c) 2019 Jack Wu, Simen Li, Hedy Wang et Koen Kanters. Les constantes de disposition NV générées dans `znp_structs.h` proviennent des structures de zigbee-herdsman 11.2.1. Licence MIT reproduite dans `LICENSES/zigbee-herdsman-MIT.txt`.

Les compilateurs, émulateurs et bibliothèques Python/Node de test sont des outils hôtes séparés ; ils ne sont pas incorporés dans le firmware et leurs installateurs ne sont pas joints aux releases.

La chaîne FreeRTOS ne lie pas les bibliothèques fermées Z-Stack/MAC de TI.
