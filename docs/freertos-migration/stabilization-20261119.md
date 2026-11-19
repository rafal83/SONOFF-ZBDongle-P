# Stabilisation et performances — 19 novembre 2026

Travail fait **en simulation et en analyse statique uniquement**. Aucune image n'a été programmée ce jour : la production tourne toujours sur l'image du commit `969fb13` (voir [status.md](status.md#production)). Tout ce qui suit attend une séance matérielle autorisée ([hardware-campaign.md](hardware-campaign.md)).

Commits : `935eb29` (AES), `d9297f9` (piles, SRAM), `0d72557` (maillage), `677a0d8` (compteurs), `f69636a` (retour arrière).

## 1. Accélérateur AES : audit et corrections

Le backend matériel n'est lié que dans la variante `--hw-aes`, et il n'y est activé qu'après l'autotest de démarrage. Défauts trouvés dans la version précédente, tous corrigés :

| Défaut | Conséquence possible | Correction |
|---|---|---|
| Fonctions TI `AESWriteToKeyStore`, `AESReadFromKeyStore`, `AESStartDMAOperation`, `AESDMAReset` : attentes **non bornées** | Tâche radio bloquée si le moteur ne répond plus | Séquence TI réécrite au niveau des registres ; chaque attente est bornée à 1 ms |
| Aucune remise à zéro après un délai dépassé ou une erreur DMA | Erreurs en cascade, moteur dans un état inconnu | Réinitialisation logicielle du DMA (attente bornée) et du moteur, zone de clé invalidée, cache de clé oublié, puis rechargement |
| Indicateur d'échec `hw_failed` global, sans verrou | Deux appels concurrents partagent l'état | État par opération ; verrou non bloquant (un second appelant passe en logiciel) dans le pilote et dans le backend |
| **Chiffrement en place avec défaut** : la reprise logicielle repartait de la sortie déjà modifiée | Trame émise corrompue | L'entrée est copiée (128 o au plus) avant toute opération en place ; la reprise part de la copie intacte |
| Déchiffrement en place avec défaut : trame rejetée (le texte chiffré était effacé) | Perte d'une trame authentique | Reprise logicielle depuis la copie |
| **Aucune détection d'un bloc faux renvoyé sans erreur** (clé périmée, sortie figée ou nulle) | Avec une sortie nulle, un MIC forgé à 0 serait **accepté** | Le premier bloc de chaque opération est recalculé en logiciel : tout écart abandonne définitivement l'accélérateur. Chaque trame **rejetée** par l'accélérateur est revérifiée en logiciel. Une opération sur 32 à 95 (tirage pseudo-aléatoire, pour ne pas s'aligner sur le trafic) est entièrement recalculée et comparée |
| Défauts persistants | Accélérateur inutilisable mais toujours sollicité | Abandon après 8 opérations fautives consécutives ; logiciel seul jusqu'au redémarrage |
| Domaine crypto éteint (veille) | Cache de clé invalide | Pas de veille dans ce firmware (`configUSE_TICKLESS_IDLE 0`) ; `hal_aes_invalidate()` est prévu pour une veille future |

Règle de sûreté : **le matériel ne décide jamais seul**. Une trame n'est acceptée que si le chemin logiciel l'accepterait aussi, et aucune sortie d'un bloc fautif n'est émise. Compteurs exposés par `DIAG 0x1B` (indices 19 à 31).

### Tests

| Test | Contenu | Résultat |
|---|---|---|
| `test_crypto` | Vecteurs pyca (AES, CCM\*, MMO, HMAC) ; backend matériel simulé : parité bit à bit sur toutes les longueurs de 0 à 127, en place ou non ; erreur à un bloc quelconque (300 tirages) ; défaut persistant ; sortie nulle silencieuse face à un MIC forgé à 0 ; clé périmée silencieuse face à une trame scellée avec l'ancienne clé ; bits inversés silencieux au déchiffrement puis au chiffrement ; réentrée ; entrée de plus de 128 o | **29 684 vérifications** ; aucune trame forgée acceptée ; défaut silencieux du chiffrement détecté par l'audit après 176 opérations (seuil du test : 192) |
| `test_hal_aes` (nouveau) | Le **vrai `hal_aes.c`** exécuté sur l'hôte contre un modèle des registres crypto, avec 9 pannes injectées : erreur de bus DMA, moteur muet, erreur d'écriture et de lecture du *key store*, lecture bloquée, zone non marquée écrite, indicateur résiduel, DMA de sortie tardif, DMA bloqué. Cache de clé, changement de clé, invalidation, verrou ; puis CCM\* complet par-dessus le pilote, avec 120 pannes injectées | **3 186 vérifications** : chaque panne est signalée dans le temps imparti, le moteur est réinitialisé et la clé rechargée ; 0 écart sur 1 052 opérations matérielles |
| Banc ARM | Pile du backend matériel en place : 968 o | Émulation |

Le modèle suit la description TI des registres (`hw_crypto.h`) et ses séquences ; **ce n'est pas le silicium**. Le gain réel, le temps par bloc et le comportement du DMA restent à mesurer : [hardware-campaign.md](hardware-campaign.md), étape H3.

## 2. Mémoire

### SRAM statique (image 1 Mbaud, logiciel)

**66 800 o sur 81 920** (pile MSP de 2 Kio comprise), soit 15 120 o libres. La variante `--hw-aes` utilise 67 128 o. Aucune allocation dynamique : `configSUPPORT_DYNAMIC_ALLOCATION 0`, et le build refuse `malloc`.

| Bloc | Octets | Détail |
|---|---|---|
| Coordinateur `zb_coord_t` | 37 456 | appareils 15 712 (200 × 48 o + index), routes 6 636, sécurité NWK 5 720 (200 pairs), MAC 5 720 (24 tampons de 127 o), extrémités 1 152, broadcasts en attente 744, clés 512, divers |
| Piles des tâches | 8 704 + 2 048 MSP | radio 5 120, hôte 2 048, superviseur 1 024, idle 512 |
| Files ZNP (8 + 8 × 253 o) | 4 048 | |
| Store `nv_log` (index + tampons) | 2 640 | |
| UART (tx 2 096, tampons rx/tx, trames) | ≈ 5 300 | |

### Piles

| Tâche | Allouée | Pire cas statique | Marge | Mesures |
|---|---|---|---|---|
| radio (logiciel) | 5 120 o | 4 264 o | 856 o | Émulation ARM : AF_DATA_REQUEST 2 240 o, réception jusqu'à AF_INCOMING_MSG 1 680 o, Link Status 1 576 o |
| radio (`--hw-aes`) | 5 120 o | 4 592 o | 528 o | Backend matériel : copie et audit (+ 328 o) |
| hôte | 2 048 o | 1 392 o | 656 o | |
| superviseur | 1 024 o | 384 o | 640 o | Matériel (18 novembre, ancienne image) : 25 mots utilisés |
| idle | 512 o | 200 o | 312 o | |
| interruptions (MSP), toutes imbriquées | 2 048 o | 824 o | 1 224 o | |

Le pire cas statique vient de `tools/stack_analysis.py`, appelé à chaque build. L'outil lit le graphe d'appels de l'ELF (capstone) et ajoute la trame d'exception avec FPU (200 o). Il résout les appels par pointeur avec des règles explicites, structure de *callbacks* par structure. Il compte aussi chaque fonction d'une composante récursive une fois par chaîne. **Le build échoue si une marge passe sous 256 o.** Les relevés de pile de toutes les tâches sont dans `DIAG 0x1B`, indices 14 à 18.

**Défaut trouvé par l'analyse.** La chaîne complétion MAC → *callback* de confirmation → envoi (Network Status, réponse de *rejoin*, *Transport Key*) → `kick` → échec immédiat de la radio → complétion… était **récursive**. Chaque niveau coûte environ 1,4 Ko de pile radio : deux niveaux auraient suffi à la déborder. Désormais `kick()` est différé pendant les *callbacks* : leur appelant reprend la file. Test : 500 renvois en chaîne pendant que la radio refuse tout, profondeur 1, 501 confirmations.

### Optimisations faites (aucune capacité réduite)

| Changement | Gain |
|---|---|
| Aucune minuterie logicielle utilisée : `configUSE_TIMERS 0` (tâche timer, sa pile et sa file supprimées) | ≈ 1 300 o SRAM, 1,2 Ko Flash |
| Routes : horloge LRU `stamp` supprimée (toujours mise à jour avec `ms`, l'âge de `ms` sert d'ordre, sans souci de débordement), champs réordonnés sans remplissage | 1 732 o (routes source 36 → 28 o × 200, routes de table 16 → 12 o × 32) |
| **Total** | **SRAM statique 69 832 → 66 800 o** |

### Optimisations proposées, non faites

| Proposition | Gain estimé | Pourquoi pas maintenant |
|---|---|---|
| Index d'appareils : référencer `dev[].ext`/`short_addr` au lieu de les recopier dans les clés de l'arbre (`core_index`) | ≈ 3 200 o | Change `core_index`, partagé avec le code d'origine ; à faire avec ses tests |
| Clusters d'extrémité en réserve commune plutôt que 16 × 16 fixes | ≈ 900 o | herdsman enregistre 15 extrémités avec peu de clusters, mais une application tierce pourrait en déclarer beaucoup : à mesurer avant |
| Relais des routes source en réserve à longueur variable (la plupart ont 0 à 3 relais) | jusqu'à 3 Ko | Réduit la capacité dans le pire cas (200 routes de 10 relais) : à décider sur mesure du réseau réel (`Mgmt_Rtg`) |
| Pile superviseur 256 → 160 mots | 384 o | Mesure matérielle sur l'image actuelle d'abord |

## 3. Maillage à 80 routeurs : nouveaux scénarios

`test_mesh_scale` (le vrai `zb_coord` au centre d'une grille de 9 × 9 routeurs simulés) comptait six phases. Quatre s'y ajoutent :

| Phase | Résultat |
|---|---|
| 7. **Canal occupé** : 40 % des trames du coordinateur échouent au CSMA | 72/72 joints, 43 échecs CSMA remontés à l'hôte (`0xE1`) ; aucune route oubliée, aucun voisin abandonné, aucun MTORR ajouté |
| 8. **File d'émission saturée** : l'hôte écrit à tous les routeurs d'un coup pendant que des rapports arrivent | 9 acceptées (file MAC de 8 + 1 en vol), 63 refusées **immédiatement** (`MAC_REFUSED`) ; chaque trame acceptée confirmée une fois et livrée ; aucun tampon perdu ; les refusées passent au second essai |
| 9. **Tempête de broadcasts** : 24 broadcasts × 2 copies entrelacées, puis 80 d'un coup | 24 remontées exactement, 24 doublons écartés ; 72 relais refusés (file de 6), chaque broadcast **remonté localement quand même** ; trafic unicast intact ensuite |
| 10. **10 routeurs redémarrés** (2 voisins, 4 coins) après 30 s d'arrêt, tables perdues, compteurs gardés | Redémarrage repéré au premier *Link Status* des voisins ; arbre reconstruit **1 s** après leur retour ; 72/72 joints ; 0 échec d'authentification |

Trois défauts trouvés par ces scénarios et corrigés :

1. **La congestion était prise pour des liens cassés.** Tout échec MAC (CSMA, erreur radio, purge) comptait comme une panne de route. Le coordinateur abandonnait alors ses voisins, oubliait ses routes et lançait des MTORR, c'est-à-dire des broadcasts sur un canal déjà saturé. Seule l'absence d'acquittement (`NO_ACK`) compte désormais.
2. **Le redémarrage d'un routeur n'était pas vu.** La reprise attendait le MTORR périodique, soit 119 s en simulation, pendant lesquelles les rapports de tout le sous-arbre se perdaient. Désormais, un voisin qui envoie un *Link Status* complet (premier et dernier fragment) sans nous, alors qu'il nous listait, est compté comme redémarré, et un MTORR part aussitôt.
3. **Un MTORR anticipé limité en débit était abandonné.** Il est désormais **différé** à la fin de l'intervalle de 10 s. Sur la phase 4, les MTORR anticipés passent de 27 à 12. Par ailleurs, un *Link Status* fractionné ne remet plus à zéro le coût sortant d'un voisin absent de ce fragment.

Nouveaux compteurs dans `DIAG 0x18` (indices 23 à 36).

## 4. Stockage : hypothèses de sécurité

Les corrections du 18 novembre (compteurs v2 liés à la clé, montage strict) n'ont pas été réécrites. Leur revue :

| Hypothèse | Constat | Suite |
|---|---|---|
| Le compteur NWK sortant ne repasse jamais sous une valeur utilisée **sous la même clé** | **Faux dans un cas.** Z2M garde sa `network_key` dans sa configuration. Après un CLEAR_STATE de l'adaptateur et une re-formation sans sauvegarde, la **même clé** revient. Or `zb_coord_form` réécrivait l'enregistrement de compteurs avec 0, écrasant le plancher que `forget` avait conservé | **Corrigé** : l'enregistrement de compteurs (v1 ou v2, quelle que soit la clé) est un **plancher** chargé au montage (`zb_coord_attach_store`) ; test D1 |
| Le formatage de récupération (`DIAG 0x1A WIPE`) d'un store corrompu peut tout effacer | **Faux** : les compteurs repartaient alors de la sauvegarde herdsman, qui n'ajoute que 2 500, alors que la sauvegarde de production avait 12 h | **Corrigé** : `nv_log_salvage` relit en lecture seule les versions valides (CRC) de l'enregistrement dans les deux bancs, même dans un store corrompu ; le plancher est réécrit après le formatage ; test D2 (bit inversé dans l'enregistrement le plus haut : ignoré) |
| Une restauration herdsman sur un store vierge reste au-dessus des valeurs utilisées | Seulement si la sauvegarde date d'avant les 2 500 dernières trames | **Marge** : sans plancher au montage, les compteurs écrits par l'hôte reçoivent 2^22 de plus (environ 80 jours du trafic moyen de production, 0,1 % de l'espace sur 32 bits). La répétition de migration le vérifie |
| Format legacy v1 (sans lien à la clé) | Accepté s'il est non nul ; aucun moyen de vérifier la clé | Les compteurs étant monotones tous réseaux confondus, une valeur v1 d'un ancien réseau reste un plancher sûr. Réécrite en v2 dès la restauration |
| Empreinte de clé sur 32 bits (MMO) | Détecte une incohérence ou une corruption, pas un attaquant qui peut écrire la Flash (il peut alors tout faire) | Accepté |
| Fenêtre de changement de clé (`set_network_key` : compteurs puis réseau) | Une coupure entre les deux écritures laisse des compteurs liés à la nouvelle clé et un réseau sous l'ancienne : **restauration refusée** (`COUNTERS_OTHER_KEY`), donc indisponibilité mais aucune réutilisation | Accepté et documenté : récupération par `restoreBackup`. Z2M ne change pas la clé de lui-même |
| **Retour en arrière d'une image Flash entière** (capture ancienne réécrite) | Ramène tout le store, plancher compris, sous des valeurs utilisées : rien de local ne peut le détecter | **Interdit dans l'outillage** : l'action `restore` refuse si le store FreeRTOS porte un réseau ; le retour arrière passe par l'action `rollback` ([rollback.md](rollback.md)) |
| Store corrompu au démarrage | Rien n'est formaté ni effacé ; sans store, la réservation des compteurs échoue, donc **aucune trame sécurisée n'est émise** : le coordinateur se tait plutôt que de réutiliser un compteur | `DIAG 0x19` (format 2) indique l'état, la présence du plancher et les planchers récupérés |

## 5. Non-régression

| Niveau | Résultat |
|---|---|
| Tests hôtes (C11, UBSan) | **17 programmes**, dont `hal_aes` (nouveau) ; `mac_core` 603 937, `crypto` 29 684, `mesh` 253, `store_integrity` 62 729, `mesh_scale` 6 776 vérifications |
| Émulation ARM | 68 vérifications, profondeurs de pile mesurées |
| herdsman 11.2.1 sur simulateur | 17 étapes réussies après chaque lot |
| Répétition de migration (sauvegarde de production du 18 novembre) | Réussie, marge de restauration vérifiée |
| Tests Python du dépôt | 54 réussis (3 ignorés), dont l'analyseur du store et la garde de `restore` |
| Images | 115 200, 1 000 000 et 1 000 000 `--hw-aes` : contrôles d'image, CCFG identique, marges de pile au-dessus de 256 o |

## 6. Ce qui n'est pas validé sur matériel

Rien de ce document. En particulier : accélérateur AES (jamais exécuté sur le MCU), relevés de pile de l'image actuelle, détection de redémarrage par *Link Status* face à de vrais routeurs, comportement sous congestion réelle, et outil `rollback` (jamais exécuté).
