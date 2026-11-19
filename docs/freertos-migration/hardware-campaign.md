# Recette matérielle reproductible : deux routeurs et un appareil endormi

But : qualifier sur le matériel ce qui n'est validé qu'en simulation ([stabilization-20261119.md](stabilization-20261119.md), [status.md](status.md)). La recette se joue sur un **réseau de laboratoire**, puis l'image retenue est qualifiée sur la production dans la durée.

**Rien ne se programme sans l'accord explicite du propriétaire, donné pour cette séance.** Le dongle unique porte la production : pendant la recette, Zigbee2MQTT est arrêté et le réseau de production n'a plus de coordinateur. Les appareils gardent leur appartenance et reprennent au retour de l'image de production. Durée de coupure prévue : environ 2 h pour H1 à H8, hors endurance.

## Matériel et topologie

| Rôle | Exemple | Placement |
|---|---|---|
| C | ZBDongle-P, image à qualifier | Poste de test, port COM8 |
| R1 | Prise ou ampoule routeur, **hors du réseau de production** | À 1 saut de C |
| R2 | Second routeur | Hors de portée directe de C, à portée de R1 (autre étage, ou C et R2 séparés par un mur porteur) |
| S | Capteur sur pile (porte, température) | À portée de R2 seulement |

Chemin attendu : S → R2 → R1 → C, soit deux relais. La topologie est vérifiée à l'étape H2 par `--map`, avant toute mesure. Si R2 entend C directement, il faut l'éloigner : sans cela, le multi-sauts n'est pas éprouvé.

## Images

| Étape | Image | Contrôle |
|---|---|---|
| H1 à H8 | `build/freertos/diag-1000000/sonoff-freertos-diag.hex` | SHA-256 du manifeste noté dans le rapport |
| H9 | `build/freertos/diag-1000000-hwaes/sonoff-freertos-diag.hex` | `hw_aes_state = 1` après l'autotest |

## Déroulé

Chaque étape écrit ses résultats sous `private/hw-campaign/` (non versionné). Les commandes `DIAG` passent par l'UART ZNP : arrêter tout client herdsman avant `freertos_perf.py`.

| Étape | Commande | Mesures | Critère de réussite |
|---|---|---|---|
| **H0** Préparation | Arrêter Z2M proprement ; `node tools/production_nv_snapshot.cjs --port COM8 --baudrate 1000000 --expected-revision <rév.> --expect-ieee 0x00124b00256c52ef --database <copie database.db> --old-backup <copie coordinator_backup.json> --output private/<dossier>/fresh_backup.json` | Sauvegarde fraîche (lecture seule) | Compteurs non décroissants ; fichier écrit |
| **H1** Programmation | `python tools/freertos_lab_session.py flash --port COM8 --usb-serial <série> --expect-ieee 0x00124b00256c52ef --baud 1000000 --z2m-stopped --take-backup private/<dossier>/capture` | Capture double, relecture | Réponse produit 1, révision attendue |
| **H1b** Base | `python tools/freertos_perf.py --port COM8 --baud 1000000 --out private/hw-campaign/base.jsonl` | AES/CCM\* logiciels (cycles DWT), piles de toutes les tâches, store | Aucun crash ; marge de chaque pile ≥ 25 % ; store `mounted` |
| **H2** Arrivée | `node tools/znp_campaign.cjs --serial COM8 --baud 1000000 --join 3 --permit 300` puis `--map` | Interview de R1, R2, S ; tables LQI | 3 interviews réussies ; R2 absent des voisins de C ; S enfant de R2 |
| **H3** Latences | `node tools/znp_campaign.cjs --serial COM8 --baud 1000000 --latency 50` puis `freertos_perf.py` | Aller-retour ZCL (médiane, p95, max) par appareil ; `rx_latency_max_us`, `znp_max_cycles`, `rx_max_cycles` | R1 : 0 échec, p95 < 150 ms ; R2 : 0 échec, p95 < 300 ms ; S : ≥ 98 % livrés au *poll* |
| **H4** Congestion | `--burst 40` | Réussites, statuts, durée ; `mac_queue_full`, `mac_tx_busy`, `route_failures` (`DIAG 0x18`) | Aucun blocage ; chaque échec remonté avec un statut ; `route_failures` inchangé hors `NO_ACK` ; latences revenues à H3 après la rafale |
| **H5** Redémarrage d'un relais | Couper R1 30 s, le rebrancher ; `--latency 10` toutes les 10 s | `neighbor_restarts`, `early_mtorr`, `early_mtorr_deferred` ; délai de retour de R2 et S | R1 détecté redémarré ; R2 et S rejoints en moins de 30 s après le retour de R1 |
| **H6** Perte d'un relais | Couper R1 définitivement (s'il existe un autre chemin), sinon le couper 5 min | `route_failures`, `neighbors_dropped`, *Network Status* reçus | Échecs remontés ; routes rétablies au retour |
| **H7** Persistance | `SYS_RESET_REQ`, puis débrancher le dongle 10 s ; `--latency 5` | `restore_status`, `counter_floor_from_store` (`DIAG 0x19`) | `restored`, plancher présent ; 0 échec d'authentification chez les appareils (aucun réappairage) |
| **H8** Endurance courte | `--soak 120 --interval 30` (1 h) ; `freertos_perf.py --interval 60 --samples 60` dans une fenêtre où herdsman est arrêté | Taux de livraison, charge CPU, piles, compteurs | ≥ 99 % pour R1 et R2, ≥ 97 % pour S ; aucune dérive des piles ; aucun reset |
| **H9** AES matériel | `freertos_lab_session.py flash ... --baud 1000000 --hw-aes --z2m-stopped` (store conservé), puis répéter H1b, H3, H4 et H8 | `ccm_hw_frame_cycles` contre `ccm_sw_frame_cycles` ; `ccm_hw_ops`, `ccm_hw_faults`, `ccm_hw_mismatches`, `ccm_hw_audits`, `aes_timeouts`, `aes_resets` | `hw_aes_state = 1` ; 0 désaccord ; 0 délai dépassé ; audits croissants ; gain mesuré et consigné (aucune estimation) |
| **H10** Zigbee2MQTT | Z2M de laboratoire (dossier séparé) sur le même réseau : interview, carte réseau, *permit join*, suppression et ré-ajout de S, arrêt propre | Journal Z2M ; `coordinator_backup.json` réécrit à l'arrêt | Aucune erreur ; sauvegarde écrite à l'arrêt (correctif des files ZNP, commit `0d5987e`) |
| **H11** Retour | Programmer l'image de production retenue (ou revenir à Z-Stack avec `rollback`, voir [rollback.md](rollback.md)) ; redémarrer Z2M de production | Journal Z2M | `resumed` ; appareils de production entendus |

Remettre ensuite R1, R2 et S à zéro avant de les rendre au réseau de production.

## Qualification prolongée sur la production

Après la recette, avec l'accord du propriétaire, l'image retenue remplace celle de production : Z2M arrêté, programmation, Z2M redémarré, `resumed` attendu. Suivi :

- **chaque jour** : erreurs et avertissements du journal Z2M (hors analyse DIAG), appareils entendus, disponibilité ;
- **chaque semaine**, Z2M arrêté 5 minutes : `freertos_perf.py` (perf, maillage, store) et sauvegarde fraîche (`production_nv_snapshot.cjs`) ;
- **critères d'arrêt** : réinitialisation inattendue, compteur `ccm_hw_mismatches` ou `aes_timeouts` non nul, marge de pile sous 10 %, store `CORRUPT`, appareils perdus. En cas d'arrêt : revenir à l'image précédente, ou à Z-Stack par `rollback`.

## Rapport

Pour chaque étape : date, image (SHA-256), commande exacte, sortie JSON, verdict. Une étape n'est **validée** qu'avec son critère atteint sur le matériel. [status.md](status.md) n'est mis à jour qu'à partir de ces rapports.
