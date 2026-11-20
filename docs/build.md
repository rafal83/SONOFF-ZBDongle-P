# Construire le coordinateur FreeRTOS

Utiliser les commandes du [README principal](../README.md#construire-et-tester) et le workflow `.github/workflows/freertos.yml` sur `freeRTOS`.

Le build compile les sources du projet, le noyau FreeRTOS et les fichiers TI de support du CC2652P. L'extraction `--device-support-only` sélectionne les sources, les en-têtes et les licences nécessaires, sans les bibliothèques fermées du SDK.

Les builds n'ouvrent pas le port série. Les opérations matérielles suivent les procédures d'[installation](firmware/installation.md) et de [restauration](firmware/restauration.md). Les [licences](dependencies.md), manifestes et rapports accompagnent les paquets générés.
