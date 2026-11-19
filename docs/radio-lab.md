# Validation radio avec la HG06106C

17 novembre 2026. L'utilisateur a réservé une ampoule Lidl/Livarno HG06106C hors de son réseau de production et autorisé son association au réseau temporaire. Le coordinateur unique a été sauvegardé puis restauré après chaque séance. Aucun Z2M ni broker MQTT n'a été lancé sur le PC.

Complément : le profil fixe **1 Mbaud** a depuis passé les mêmes échanges en reprenant le réseau de laboratoire, avec en plus **40 lectures pendant 80 écritures NV modifiées**. Un contrôle avec le candidat 115200 a aussi passé cette charge. Le [comparatif de performances](performance.md) donne les latences et distingue ces essais de reprise de l'association initiale ci-dessous.

## Résultat acquis

Le HEX `bd33bf77ff3c1e4a713ee4b35783f087a6dfc94b9945ef19c8b16fa501ef6d57`, à 115200 bauds fixes, a réussi sur le CC2652P réel :

- Association de l'ampoule, puis fermeture de l'association.
- Descripteur de nœud routeur, endpoints 1 et 242, descripteurs simples.
- Lecture ZCL des identifiants : `TS0505A`, fabricant `_TZ3000_dbou1ap4`, conformes à cette ampoule.
- Lecture de son état, changement marche/arrêt avec lecture de confirmation, puis remise à l'état initial.
- 30 lectures d'attributs supplémentaires avec leurs réponses applicatives. Les confirmations AF sont attendues par le véritable `ZStackAdapter`.
- Sauvegarde des tables incluant l'appareil, reset logiciel du coordinateur et nouvel échange radio sans réappairage.
- Effacement de la configuration expérimentale, restauration par le gestionnaire herdsman depuis la sauvegarde, puis nouvel échange avec l'ampoule sans réappairage. Le compteur réseau de la sauvegarde restaurée n'a pas reculé.
- Aucun rejet dans le garde radio, échec d'allocation RX, abandon TX, réessai du pilote TX ou erreur UART dans les 27 instantanés de diagnostic de cette séance.
- Retour final à l'original `20240710` : CRC ROM et relecture complète des 360 448 octets identiques au dernier instantané original, puis version/IEEE et dix pings vérifiés.

La sécurité NWK configurée lue dans la sauvegarde est de niveau 5. Les octets chiffrés sur l'air n'ont pas été capturés par un sniffer indépendant. Cette séance valide des échanges réels avec un appareil ; elle ne mesure ni les gains CPU, ni les tempêtes de broadcasts, ni les files d'appareils endormis.

## Essais précédents et limites

Les premiers essais avaient expiré sur un descripteur simple ou une lecture Basic. Le harnais attend maintenant l'annonce après l'indication provisoire du Trust Center, suit la lecture Basic anticipée de herdsman pour les fabricants Tuya 0/4098 et borne les reprises ZDO à trois tentatives. Deux autres fenêtres n'ont vu aucune nouvelle association. La séance réussie a suivi le rapprochement et la remise en appairage de l'ampoule. La cause exacte des premiers échecs n'a pas été isolée ; un essai réussi ne prouve pas leur disparition dans toutes les conditions.


## Outils et protection du retour arrière

`tools/herdsman_radio.cjs` utilise le vrai moteur herdsman 11.2.1 verrouillé dans `tools/herdsman-lab-lock.json`. Son précontrôle `--check-runtime` ouvre zéro port et vérifie aussi le codec du diagnostic avec le parseur MT réel. La commande SYS `0xF0` est une extension de laboratoire en lecture seule : huit compteurs/métadonnées, aucun accès mémoire arbitraire, clé, adresse ou payload.

`tools/run_dongle_session.py --radio-peer` ouvre l'association pendant au plus 180 secondes. Les états et logs restent privés. Avant de restaurer l'original, le superviseur peut conserver la Flash expérimentale complète et sa CRC ROM. Une reprise avec `--radio-resume-session` vérifie son empreinte, son origine expérimentale, son code/CCFG et la distinction de sa NV avec la sauvegarde de production. Elle copie seulement la plage NV `0x50000..0x55fff` dans le candidat audité. Le firmware original et sa NV ne sont jamais la source d'une reprise radio expérimentale.

La preuve publique combine les résultats série/NV de la séance 009 et les résultats radio de la séance 011, **strictement sur le même SHA-256 de HEX**. La séance 009 avait échoué dans son volet association : seuls ses résultats série/NV réussis sont repris. Chaque retour arrière complet est vérifié indépendamment par l'exportateur. Les essais locaux précédents sur le HEX `eebe2d…` restent historiques et ne sont pas transférés au nouveau HEX.

Sources pour l'appareil et le déroulement d'interview : [HG06106C Zigbee2MQTT](https://www.zigbee2mqtt.io/devices/HG06106C.html), [guide constructeur, page 10](https://doc.eedomus.com/files/334403_QSG_FR.pdf), [interview du moteur herdsman épinglé](https://github.com/Koenkk/zigbee-herdsman/blob/99d664696dfb36dad175a7a0205d2481a42a1902/src/controller/model/device.ts).
