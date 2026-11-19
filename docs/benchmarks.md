# Phase E — optimisations et comparaison objective


Le [comparatif du 17 novembre](performance.md) mesure désormais le vrai firmware d'origine 20240710 face aux trois profils candidats sur le même dongle : 3 600 transactions SYS/NV, références avant/après, puis deux essais avec une HG06106C pour comparer les débits candidats sous charge NV. Il ne remplace pas la campagne complète ci-dessous : pas de mesure CPU, pas de référence RF du firmware d'origine et pas de maillage représentatif.

## Changements déjà réalisés

Le coordinateur complet intègre le transport borné, TLSF, les timers statiques, l'index NV et les files de tâches/broadcasts à ajout constant. L'autodétection UART reste désactivée après son échec matériel ; les profils élevés testés utilisent un débit fixe. Les tests logiciels et instructions ARM vérifient leurs invariants. Ils ne mesurent pas les performances RF ni les cycles physiques du MCU. Voir [les optimisations et leurs limites](optimizations.md).


## Chantiers de la base ZNP, par priorité

| Objectif | Action indépendante proposée | Mesure / limite |
|---|---|---|
| Mémoire déterministe | Pools pour enveloppes MT et métadonnées, bornes et erreurs explicites ; conserver les contrats TI | Tas libre minimal, plus gros bloc libre, temps d'allocation ; les bibliothèques fermées peuvent rester dynamiques |
| Moins d'accès Flash fréquents | Mesurer les recherches NV et profiler les accès sécurité avant d'ajouter un cache RAM borné | Nombre/temps de lectures, invalidation à chaque mise à jour ; ne jamais exposer les clés dans les traces |
| NV et compaction | Conserver NVOCMP et son mécanisme de récupération, tester les coupures à plusieurs étapes | Durée maximale, pages libres, nombre de compactions ; pas de changement de schéma sans migration |
| UART | Instrumenter le callback UART2 et ses files, conserver la correction TX_FINISHED du patch Koenkk | Pertes, high-water, délai RX→SRSP et durée des sections critiques |
| Priorités | Séparer SRSP/confirmations des rapports à fort volume, avec quota empêchant la famine des AREQ | Latence p95/p99 et confirmations livrées ; ne jamais réordonner des dépendances transactionnelles |
| Appareils endormis | Budget des files indirectes selon nombre réel d'enfants, poll et délai APS | Expirations, livraison après réveil ; les enfants directs ne sont pas tous les nœuds du réseau |
| Voisins / routes | Distinguer voisins directs, routes et source routes ; dimensionner sur SRAM disponible | Occupation, évictions et échecs route ; 31 routeurs globaux ne signifient pas 31 voisins directs |
| Broadcasts | Profiler caches de doublons, route discoveries et quotas ; respecter les temporisations Zigbee | Débit utile sous rafale et taux de doublons, sans casser découverte/réparation |
| Watchdog | Supervision du progrès de plusieurs tâches, journal minimal sans secrets, bootloop bornée | Faute injectée, reprise et NV cohérente ; ne pas nourrir le watchdog seulement depuis une ISR |
| Instrumentation | Compteur de cycles DWT, niveaux de pile/tas, histogrammes MT et compteurs radio | Distinguer temps CPU, attente UART, contention radio et temps applicatif |

Ne jamais supprimer les écritures durables nécessaires aux **compteurs de trames de sécurité** pour gagner du temps Flash. Une stratégie de réservation de plages doit être conçue et testée contre la réutilisation après coupure avant tout changement. Les bibliothèques TI en objet ne sont pas modifiées ni rétroconçues.

## Protocole de benchmark

Comparer d'abord le firmware communautaire de référence et la base personnalisée sur **le même SONOFF de laboratoire**, même débit, puissance, canal, antenne, alimentation, versions Z2M/herdsman et mêmes appareils. Alterner l'ordre A/B et faire au moins cinq répétitions ; noter les interférences et la disposition physique. Les réseaux restent séparés de la production et leurs secrets restent privés.

Scénarios : ping MT sans trafic RF ; lectures ZCL simples ; rafales concurrentes de 1/4/8/16 commandes ; rapports périodiques ; appareil endormi ; routeur coupé puis rétabli ; redémarrage hôte ; réseau multi-sauts ; reprise après coupure pendant compaction. Monter la charge progressivement jusqu'à l'apparition de pertes ou d'une dérive des files. Revenir à une charge faible pour mesurer la récupération. Faire ensuite une endurance 24 h puis 7 jours.

Pour chaque scénario : durée totale réellement observée, nombre d'opérations émises, succès applicatifs et erreurs/timeouts, latences médiane/p95/p99/max, débit utile confirmé, réessais MAC/APS, trous de rapports, occupations des files et tables, tas minimum, stack high-water et temps maximum de compaction. Une confirmation AF seule n'est pas nécessairement une réponse ZCL ; documenter exactement le point de début et de fin de chaque chronométrage. Réserver 5 min de chauffe puis une fenêtre stable d'au moins 10 min par charge ; ne pas retirer les timeouts des taux d'erreur.

Les critères proposés sont : absence de timeout SYS/AF à la charge de référence, aucune perte de persistance ou réutilisation de compteur, aucune dérive mémoire sur endurance, pas de régression de livraison des appareils endormis. Une amélioration de médiane avec une p99 ou des pertes aggravées n'est pas un succès global. Les seuils de gain restent à fixer à partir de la référence réellement mesurée.

## Outil statistique livré

`tools/benchmark.py` lit un CSV minimal `status,latency_ms` (statuts `ok`, `error`, `timeout`) et une durée explicitement fournie. Il calcule erreurs, succès/s et percentiles par rang le plus proche. Aucune acquisition automatique ni donnée de réseau n'est incluse.

```sh
python tools/benchmark.py private/mesures-lab.csv --duration-seconds 600 --label baseline-scenario-1
```

La durée doit inclure toute la fenêtre d'observation et les timeouts ; elle n'est pas déduite de la somme des latences concurrentes. Les latences sont calculées sur les succès uniquement et le taux d'erreur est publié séparément. Pour un test sans succès, les percentiles valent null. Aucun fichier de mesures RF n'est fourni : les seules données synthétiques sont dans les tests unitaires.

**Phase E : transport déterministe local et méthode de mesure réalisés.** Les optimisations de pile, l'instrumentation CPU/RF et les résultats comparatifs restent à intégrer et à mesurer sur laboratoire.
