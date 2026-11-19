# Gains mesurés sur le dongle — 17 novembre 2026

Le profil fixe **1 000 000 bauds** apporte un gain mesuré sur ce PC et ce ZBDongle-P. Le firmware contient les mêmes optimisations indépendantes que le profil 115200 ; son débit UART est fixé à la compilation. L'autodétection reste désactivée.

## Comparaison avec le firmware d'origine 20240710

Le vrai code et le CCFG du firmware sauvegardé ont été réutilisés sans modification pour la référence. Chaque profil démarre avec une NV de laboratoire vierge, reçoit la même charge synthétique, puis laisse place à une restauration intégrale de l'original et de sa NV de production. Aucune image d'origine ni donnée réseau privée n'est distribuée.

| Profil | Lecture NV 240 octets, médiane | Écriture identique 240 octets, médiane | Écriture identique, p99 |
|---|---:|---:|---:|
| Original 20240710, 115200 | 26,38 ms | 30,13 ms | 133,14 ms |
| Candidat, 115200 | 26,59 ms | 27,29 ms | 27,80 ms |
| Candidat, 460800 | 9,26 ms | 10,19 ms | 10,43 ms |
| Candidat, 1000000 | **5,65 ms** | **6,55 ms** | **7,19 ms** |

À 1 Mbaud, les deux médianes s'améliorent respectivement de **4,67×** et **4,60×** par rapport à la référence la plus rapide des deux mesures de l'original. À débit inchangé, la lecture n'accélère pas ; le p99 des écritures identiques passe de **133,14 à 27,80 ms**, soit **4,79×**. Le nombre d'effacements Flash n'est pas instrumenté : ces résultats portent sur la latence des transactions, pas sur une durée de vie Flash mesurée.

Le protocole comprend cinq profils : original avant, candidat 115200, 460800, 1000000, original après. Pour chaque profil : trois tours, six cas (ping, lectures de 16/64/128/240 octets, écriture identique de 240 octets), 40 observations par cas et par tour, cinq échauffements exclus et ordre des cas mélangé avec la même graine. Les **3 600 transactions chronométrées réussissent**, avec contrôle du statut et des données. La dérive de la médiane de lecture 240 octets entre les deux références est de **0,33 %**.

Ces temps comprennent Python, USB, UART et réponse MT. Ils ne mesurent ni les cycles CPU, ni le débit radio, ni une accélération uniforme de toutes les commandes Zigbee. Les écritures testées ici sont identiques ; les écritures qui modifient les données sont couvertes séparément par l'essai radio sous charge.

## Échanges réels avec la HG06106C

Deux essais successifs comparent le **candidat 115200 au candidat 1000000**, avec la même ampoule et reprise de la NV privée du réseau de laboratoire. Le firmware d'origine n'a pas été soumis à ce comparatif radio.

| Lecture d'attribut distant | Candidat 115200 | Candidat 1000000 |
|---|---:|---:|
| 30 lectures successives, médiane | 67,76 ms | **16,15 ms** |
| 40 lectures pendant 80 écritures NV modifiées, médiane | 89,17 ms | **22,23 ms** |
| Maximum observé pendant les écritures | 349,84 ms | 171,44 ms |

Les deux essais passent ZDO, descripteurs, attributs Basic, commande marche/arrêt avec relecture, 30 lectures, puis **40 lectures pendant 80 écritures NV de 240 octets dont les données changent**. La dernière valeur NV est relue et vérifiée. La radio répond encore après reset du coordinateur et après restauration de la sauvegarde du réseau de test ; les compteurs de sécurité ne reculent pas. Les compteurs diagnostiques de rejet radio, d'allocation, d'échec TX et d'erreur UART restent nuls.

Une première reprise à 115200 depuis un instantané plus ancien a échoué sur le descripteur distant. L'essai a ensuite réussi avec l'instantané le plus récent, sans réappairage. Cette tentative n'entre pas dans les mesures réussies. Les deux essais successifs avec une seule ampoule établissent un fonctionnement et des latences observées ; ils ne constituent pas une campagne de fiabilité, de maillage ou d'endurance.

## Paquet et utilisation

Le paquet de performances est `build/sonoff-znp-optimized-20261114-1000000-lab.zip`. Le HEX mesuré est identifié par SHA-256 :

```text
cb2b5e4f5cae02a5601c6c1c8d3cc9d7be7a009cfa2fb7398d323f39eb31dee5
```

Ce profil exige cette configuration **lorsqu'il est effectivement installé** :

```yaml
serial:
  port: /dev/serial/by-id/TON_DONGLE
  adapter: zstack
  baudrate: 1000000
  rtscts: false
```

À l'issue de ce comparatif, l'original 20240710 avait été restauré avec ses données réseau à 115200. Une étape ultérieure, autorisée par l'utilisateur, a installé le candidat à 1 Mbaud sur le vrai réseau et adapté le débit Z2M : voir [production-trial-20261117.md](production-trial-20261117.md) pour l'état actuel. La configuration du débit est documentée par [Zigbee2MQTT](https://www.zigbee2mqtt.io/guide/configuration/adapter-settings.html).


## Preuves et reproduction

- [Mesures et statistiques recalculées](performance-results-20261117.json).
- [Qualification exacte du HEX 1 Mbaud](hardware-validation-20261117-1000000.json).
- [Contrôle radio sous charge à 115200](radio-load-control-20261117.json).
- [Qualification série du profil 460800](hardware-validation-20261117-460800.json).

`tools/run_performance_matrix.py` orchestre les sessions explicitement autorisées, avec sauvegarde et restauration par profil. `tools/transport_benchmark.py` vérifie chaque transaction. `tools/summarize_performance.py` refuse les observations manquantes, dupliquées ou en échec, recalcule les statistiques depuis les CSV privés et ne publie que mesures, booléens et empreintes. Les preuves sont liées au SHA du HEX et au débit ; une modification du firmware exige une nouvelle mesure.
