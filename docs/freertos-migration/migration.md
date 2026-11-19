# Migrer le réseau de production vers le firmware FreeRTOS

Objectif : remplacer Z-Stack par le coordinateur FreeRTOS **sur le même dongle**, sans réappairer les appareils, avec retour arrière garanti.

## Principe

Zigbee2MQTT garde une sauvegarde du coordinateur (`coordinator_backup.json`) : identité, PAN, clé réseau, graine des clés de lien (TCLK), appareils, compteurs. Au premier démarrage sur le nouveau firmware, herdsman trouve un adaptateur non configuré mais une sauvegarde conforme à la configuration. Il applique alors sa stratégie `restoreBackup` : il forme un réseau provisoire, écrit la sauvegarde dans la NV (synthétisée et appliquée par le firmware), redémarre l'adaptateur, puis reprend le réseau.

Le firmware FreeRTOS ne touche jamais la NV Z-Stack (`0x50000–0x55FFF`). Il stocke son propre état dans `0x48000–0x4FFFF` (`nv_log`, journal sûr aux coupures). La NV de production reste donc intacte, et le retour à Z-Stack retrouve le réseau tel qu'il était.

## Répétition (sans matériel)

```powershell
node tools/znp_migration_rehearsal.cjs --backup private/<copie>/coordinator_backup.json
```

Le **vrai** herdsman 11.2.1 restaure la sauvegarde dans le firmware simulé (même code que le MCU). La sauvegarde que produit le nouveau firmware est ensuite comparée entrée par entrée. Le rapport, sans aucun secret, est écrit dans `private/znp-rehearsal/`.

Résultat du 18 novembre 2026 sur la sauvegarde de production (137 appareils, 129 clés de lien) :

| Vérification | Résultat |
|---|---|
| Stratégie au premier démarrage, puis au redémarrage | `restored`, puis `resumed` |
| IEEE du coordinateur, PAN, PAN étendu, canal, `nwkUpdateId` | identiques |
| Clé réseau et numéro de séquence, graine TCLK | identiques |
| Compteur de trames NWK | ≥ sauvegarde + 2 500 (réservé durablement **avant** le redémarrage) |
| Appareils transmis par herdsman | **130/130 identiques** (adresse, IEEE, enfant direct) |
| Clés de lien et compteurs APS | **129/129 identiques**, compteurs non décroissants |
| Appareils non transmis | **7** : sans clé de lien et non enfants directs. Le `restoreBackup` de herdsman leur donne le type « par défaut », que sa propre table considère comme libre : l'appareil suivant les écrase (Z-Stack → Z-Stack les perdrait aussi). Le firmware **réapprend** tout nœud authentifié par la clé réseau dès son premier trafic. |

Cette répétition a trouvé et fait corriger trois défauts : compteurs relevés non durables avant le reset, compteur APS entrant non persisté, rejet des nœuds non transmis.

## Garde-fous de persistance (depuis la séance 10)

- **Compteurs liés à la clé.** Les compteurs NWK et APS sortants sont stockés avec une empreinte de la clé réseau, et écrits **avant** l'enregistrement réseau à la formation. Une restauration est refusée si cet enregistrement de compteurs est absent, malformé, lié à une autre clé ou incohérent : rien n'est appliqué, rien n'est effacé, et la cause est lisible par `DIAG 0x19`. herdsman ne voit alors aucun réseau ; la récupération passe par `restoreBackup` depuis la sauvegarde Z2M, qui relève les compteurs de 2 500.
- **Store jamais formaté implicitement.** Un store corrompu n'est jamais formaté sans décision explicite : il est laissé intact et signalé. Seul `DIAG 0x1A` avec le mot de passe `WIPE` l'efface.

## Procédure réelle

**Ne s'exécute qu'avec l'accord explicite du propriétaire du réseau.**

1. Le dongle doit être sur le firmware Z-Stack de production (`restore` si nécessaire), et Z2M doit avoir tourné dessus.
2. Arrêter **proprement** Z2M : il écrit alors une `coordinator_backup.json` fraîche. Copier son dossier de données dans `private/`.
3. Lancer :

   ```powershell
   python tools/freertos_lab_session.py migrate --port COM8 --usb-serial <série> --expect-ieee 0x00124b00256c52ef `
       --z2m-stopped --z2m-data private/<copie-données-z2m> --take-backup private/<nouveau-dossier> `
       --confirm-production-migration
   ```

   L'outil :
   - vérifie la copie des données Z2M (sauvegarde unifiée de moins de 2 h, IEEE égale à celle du dongle, adaptateur `zstack`, débit) ;
   - rejoue la répétition sur **cette** sauvegarde et s'arrête si elle échoue ;
   - capture deux fois la Flash complète dans la même session ROM (image de retour arrière, NV Z-Stack comprise) ;
   - programme l'image FreeRTOS au **débit configuré dans Z2M** (1 000 000 bauds en production), store FreeRTOS vierge ;
   - vérifie que le firmware répond (produit 1).
4. Démarrer Z2M **sans changer sa configuration**. Le journal doit indiquer la stratégie `restoreBackup`. Les appareils reprennent sans réappairage : clés dérivées de la même graine, compteur NWK au-delà du dernier utilisé.

## Retour arrière

**Une fois le réseau exploité sous FreeRTOS, ne plus réécrire la capture octet pour octet** : la NV Z-Stack capturée ramènerait le compteur de trames NWK sous des valeurs déjà utilisées. L'action `restore` le refuse désormais. Le retour arrière passe par l'action `rollback` : sauvegarde fraîche lue sur le dongle, vérification du compteur, réécriture de l'application Z-Stack seule, effacement de l'état Z-Stack, puis `restoreBackup` par Z2M. Voir [rollback.md](rollback.md).

Restauration avant toute émission du coordinateur FreeRTOS (par exemple, si le firmware ne répond pas juste après la programmation) : l'action `restore` reste valable, son store étant encore vide.

## Marges de compteurs à la restauration

herdsman écrit les compteurs de sa sauvegarde majorés de 2 500. Quand le store FreeRTOS ne contient encore aucun plancher de compteurs (store vierge : première migration, ou formatage de récupération sans enregistrement récupérable), le firmware y ajoute 2^22 ; la répétition de migration le vérifie (`nwk_frame_counter_restore_margin`). Les appareils acceptent tout compteur supérieur.

## Limites connues au moment de la migration

Voir [znp-compat.md](znp-compat.md#non-pris-en-charge-erreur-rpc-ou-statut-dechec-explicite). Points d'attention pour ce réseau :

- **Couverture.** 133 appareils sur 137 sont derrière des routeurs. Le routage maillé (Link Status, many-to-one, Route Record, routage par la source, relais) est testé en simulation. Sur le matériel, le voisinage avec un routeur réel est constaté, mais le multi-sauts ne l'a pas été.
- **Puissance.** La configuration Z2M de production demande `transmit_power: 20`. herdsman l'envoie (`stackTune`) une fois le réseau démarré ; le firmware redémarre alors la radio sur le **PA 20 dBm**, sans perdre les bits *frame pending*. Ce chemin est **validé sur le matériel** depuis la séance 10. Sur le dongle, la configuration du PA 20 dBm échouait (`ERROR_PAR`), faute du code de région dans le lien d'overrides ; une fois corrigé, l'ampoule de test entend mieux le coordinateur à 20 dBm.
- **Fonctions absentes.** Pas de changement de canal, de rotation de clé, de Touchlink ni de Green Power.
