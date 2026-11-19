# Migration au SDK TI 8.33.00.16

18 novembre 2026. Le dernier [SDK SimpleLink Low Power F2 publié par TI](https://www.ti.com/tool/download/SIMPLELINK-LOWPOWER-F2-SDK) est le **8.33.00.16**, daté du 22 juin 2026, compatible CC2652P. La migration utilise une copie séparée du SDK et le candidat **2026111802 à 1 Mbaud**, avec les trois moteurs intégrés. Il est maintenant installé sur le dongle, après autorisation et sauvegardes vérifiées. Les candidats précédents sont conservés.

La validation logicielle de ce candidat et sa comparaison des bibliothèques sont publiées séparément : [rapport ARM](core-integration-validation-20261118-sdk833-r2.json), [comparaison des objets TI](sdk-migration-comparison-20261118.json). Le [rapport matériel](production-validation-20261118-sdk833-r2.json) atteste l'installation, le démarrage MCU, 300 échanges série, la relecture complète, les paramètres/clés/compteurs et les 75 associations conservés. Z2M 2.14.3/herdsman 11.2.0 et MQTT démarrent ; les contrôles de santé et une sauvegarde native récente passent. Deux GET d'état sur un luminaire Philips et une prise Tuya reçoivent une télémétrie récente, sans changer leurs sorties. Les rapports ARM conservent leur portée d'émulation.

## Échec initial et correctif de démarrage

Le premier candidat **2026111801** démarrait et répondait aux lectures série/NV, mais rejetait les demandes AF avec **MEM_ERROR**. Il est rejeté pour l'usage radio ; son [rapport d'échec](production-validation-20261118-sdk833.json), ses binaires et les sources antérieures au correctif sont conservés. Les tests logiciels initiaux n'exécutaient qu'une initialisation manuelle du tas.

Le vrai démarrage TI appelle `osalHeapInitFxn` depuis `Startup_exec`, puis depuis `BIOS_init`. Le second appel remettait `port_core_state` à zéro ; la première réservation de 13 520 octets restait occupée et le contrôle de marge empêchait d'en allouer une seconde. La perte du pointeur, reproduite avec l'ELF exact 2026111801, bloquait ensuite les buffers et la sécurité NWK.

**2026111802** rend cette réservation idempotente. Le test ARM appelle maintenant le véritable callback deux fois au démarrage, puis après création de buffers actifs : pointeur, index, listes et budget restent inchangés. Ce test rejette l'ancien ELF et passe sur le correctif. Les 47 tests Python, dont les scénarios de repli vers 20261117 avec compteurs actualisés, passent. Le correctif est installé et les erreurs MEM_ERROR ne réapparaissent pas dans la fenêtre observée.

Le dernier relevé compte **41 appareils avec échanges récents** (28 routeurs et 13 terminaux), sur 75 associations. La fenêtre capturée conserve huit erreurs NWK_NO_ROUTE et quatre autres erreurs de commandes, notamment des délais d'attente et un attribut non pris en charge. L'utilisateur a depuis confirmé que la **HG06106C est débranchée** : son absence de réponse est expliquée et n'est plus un incident à investiguer. Il confirme aussi les remontées d'un **capteur de porte à pile**. Ces informations sont distinguées des 41 observations corrélées et ne gonflent pas ce compteur. La couverture de tout le réseau et l'endurance restent ouvertes.

## Portée

TI indique que les nouveautés propres au SDK 8.33 concernent Wi-SUN et que les autres composants restent ceux du 8.31.00.11. Les [notes du composant Zigbee livré](https://software-dl.ti.com/simplelink/esd/simplelink_cc13xx_cc26xx_sdk/8.33.00.16/exports/docs/zigbee/release_notes_zigbee_8_30_00.html) restent intitulées Z-Stack 8.30.00. Passer au dernier SDK n'établit donc pas un gain de performance Zigbee.

Le profil SONOFF conserve DIO13/12 pour UART, DIO29/28 pour RF, le trim oscillateur `0xc1`, la récupération ROM sur DIO15 et la NV `0x50000–0x55fff`. Les réglages électriques et LED RF-STAR du dépôt Egony ne sont pas appliqués. Les chemins indirects, BTT et NWK ENC-MIC-32 restent raccordés ; APS, Trust Center et Green Power restent TI.

Le paquet officiel Windows est vérifié avec son SHA-256 publié par TI : `f61ccd1727bc54819506b4acc3b3a2891d0dd802df1c421700e93c1b8f5d3688`. La bibliothèque NWK est également épinglée par version ; une bibliothèque inconnue ou provenant de l'autre SDK est refusée avant préparation des alias. L'audit compare les octets, tailles et alignements des sections allouées des objets, ainsi que leurs cibles de relocalisation ; les données de debug sont exclues de cette comparaison. Les tests exécutent ensuite les véritables instructions liées et vérifient les appelants natifs.

La comparaison des **20 objets NWK** trouve leurs sections allouées et cibles de relocalisation identiques malgré les empreintes différentes des archives. Les différences NPI inspectées portent uniquement sur le copyright, conservé en 2025. Le patch Koenkk est adapté sur cette unique ligne de contexte, sans assouplir le contrôle des autres modifications. Le SDK ajoute un événement de temporisation broadcast dans `ZMacEventLoop` : le corps natif de cette fonction reste conservé. Les originaux de `nwk.h`, `zd_sec_mgr.c`, `nvocmp.c`, `ClockP.h` et du pilote UART2 CC26X2 comparés sont identiques.

## Résultats du candidat

Les **101 sources TI/ZNP** compilent. Les tests natifs des optimisations et des moteurs passent, ainsi que les tests ARM de garde radio, UART et intégration complète. Cette dernière campagne exécute **229 appels et 147 590 instructions**, dont les répartiteurs natifs, les confirmations et l'initialisation du routage, avec huit confirmations applicatives et aucun doublon. Les frontières Flash/RF/RTOS et accélérateur AES restent modélisées. Le pic de pile observé dans ces parcours est de 776 octets.

| Budget SRAM du candidat | Octets |
|---|---:|
| Tas avant allocations OS et réservation des moteurs | 32 908 |
| Réservation moteurs et adaptateurs | 13 520 |
| Métadonnées TLSF de cette réservation | 8 |
| Libre immédiatement après réservation | **19 380** |

Le nouveau MAC augmente notamment ses tableaux de source matching. Le premier build échouait au contrôle de 32 Kio. La variante retire maintenant **320 octets de BTT native devenue inutilisée** : seules les deux relocalisations de la boucle d'initialisation BTT dans le `RTG_Init` conservé pointent vers une constante zéro en Flash. Le compte de la boucle est nul ; les initialisations du routage, des recherches de routes et du timer de rétention restent exécutées. Le linker peut ainsi supprimer la table native. L'audit exige son absence et une constante en lecture seule, tandis que les instructions natives restent intactes. La BTT indépendante garde ses **40 transactions**. Les tableaux MAC agrandis, les index NV et les seuils de 32 Kio puis 16 Kio sont conservés.

Le budget indiqué précède les allocations système suivantes. Il ne constitue pas une mesure du tas libre après démarrage sur un réseau réel. Le HEX vérifié a pour SHA-256 `cb400551e38ca84491dc920b647cac1ed839c5b20fb34aed5290c243870b093b`.

Les outils autonomes existants TI Arm Clang 4.0.2.LTS et SysConfig 1.23.0+4000 sont conservés. Ce sont les versions utilisées par notre build ; les notes TI recommandent une autre combinaison. Les tests logiciels locaux ne constituent pas une validation TI de cette combinaison ni une validation matérielle.

## Reproduction

Depuis un environnement déjà préparé selon [build.md](build.md) :

```powershell
python tools/setup_ti_toolchain.py --sdk-version 8.33.00.16
python tools/build_coordinator.py --experimental-cores --baudrate 1000000 --sdk-version 8.33.00.16
```

Le rapport comparatif exige également la copie vérifiée du SDK 8.30.01.01. Si elle n'est pas installée, lancer `python tools/setup_ti_toolchain.py` avant cette étape facultative pour la compilation, obligatoire pour notre paquet comparatif :

```powershell
python tools/audit_sdk_migration.py
python tools/finalize_core_candidate.py --sdk-version 8.33.00.16
```

Le HEX est `build/coordinator-optimized-1000000-cores-sdk833-r2/sonoff-znp-coordinator.hex` et le paquet `build/sonoff-znp-optimized-2026111802-1000000-cores-sdk833-r2-lab.zip`. Sources SDK, overlays, préparations de bibliothèques, journaux et artefacts de cette variante utilisent des chemins séparés. Les tests natifs réutilisent leurs noms d'exécutables temporaires ; exécuter les builds successivement. Les commandes sans sélection de SDK conservent le profil 8.30 pour reproduire les anciens candidats.

Pour actualiser le paquet après des essais physiques, utiliser `python tools/attach_sdk833_evidence.py --session-dir private/production-sdk833-r2-20261118`. Le script vérifie les captures et les rapports avant de modifier le manifeste et le ZIP. Il ne flashe pas. La finalisation logicielle précédente refuse volontairement de remplacer un manifeste déjà marqué comme testé sur matériel.

La continuité des associations est vérifiée par lecture des sauvegardes avant/après le flash, sans réappairage ni effacement NV/CCFG. Le retour préparé utilise `tools/upgrade_sdk833.py rollback --candidate-revision 2026111802` avec les paramètres du dongle et de la séance : restauration de l'application 20261117 avec la NV actuelle et ses compteurs, sans ancien dump NV. Ce retour n'a pas été exécuté physiquement pendant cette séance. La couverture complète et l'endurance restent ouvertes. Le manifeste conserve `final_qualified: false`.
