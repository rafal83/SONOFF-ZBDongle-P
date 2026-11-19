# Validation — preuves et essais restant à réaliser

## Exécuté sur PC Windows, du 12 au 14 novembre 2026

La version optimisée `20261114` comprend **92 sources** et quinze exécutables C de contrôle, avec périphériques simulés. Les tests ARM exécutent les instructions liées de nos fonctions, avec dépendances ROM simulées. Les trois profils fixes sont compilés, contrôlés séparément et validés physiquement en série/NV le 17 novembre. Les profils 115200 et 1000000 passent également les échanges radio avec une HG06106C et une charge d'écritures NV concurrente. Le [comparatif](performance.md) donne les gains mesurés et leur portée. La [qualification finale](final-status.md) reste incomplète.

`python tools/setup_ti_toolchain.py` puis `python tools/build_coordinator.py --baseline` : installation des outils TI autonomes, compilation de 79 sources, liaison de la pile ZNP complète et export du HEX de référence. Comparaison de toutes les sections Flash ELF/HEX, 13 contrôles de récupération, trim SONOFF, identité ZNP et 14 symboles MT/réseau/NV vérifiés. Le préprocesseur confirme RF, UART et NV réellement compilés. Le rapport est dans `build/coordinator/manifest.json`. Aucun démarrage matériel ou Z2M n'est simulé par ces contrôles statiques.

`python tools/build.py host` : compilation C11 avec avertissements traités en erreurs et assertions actives, puis exécution du binaire de tests. Couverture fonctionnelle : vecteur SYS_PING, tous ses points de fragmentation, trames successives, bruit, checksum invalide, limite 250 octets, rejet 251, timeout et débordement de compteur, 200 000 octets pseudo-aléatoires, saturation et ordre de la file TX sur 30 cycles, commandes diagnostiques et erreurs MT. Ce sont des essais logiciels, pas des échanges UART physiques.

`node tests/herdsman_framing.mjs` : le véritable module `Frame` du snapshot herdsman encode quatre requêtes et décode les réponses du programme C via stdin/stdout. Ping, version, IEEE synthétique et erreur de commande sont vérifiés. Ce test d'interopérabilité de tramage n'instancie pas `ZStackAdapter` et ne valide pas son démarrage. Node 22.23.2 émet un avertissement attendu sur l'API expérimentale de transformation TypeScript.

`python tools/build.py diagnostic` : compilation et édition de liens ARM, génération du HEX à partir des segments ELF, contrôle CCFG positif. L'image actuelle, après modification du transport, contient 4 416 octets Flash adressés, dont 88 octets CCFG, hors taille du debug ELF. Aucun flash ou exécution MCU.

`python -m unittest discover -s tests -v` : tests Python des cas de récupération permis/interdits, CCFG incomplète, présence de données NV/IEEE secondaire, HEX corrompus, conversion sparse, sélection du patch et cohérence ELF/HEX réel. Les résultats finaux sont consignés dans [test-results.md](test-results.md).

## Ce qui ne constitue pas une preuve

La réussite du build ARM ne prouve pas que le quartz du PCB est correctement calibré. Le contrôle CCFG ne prouve pas que les pads debug sont accessibles. Le succès de SYS_PING ne prouve ni un commissioning Zigbee, ni la persistance, ni le démarrage complet de herdsman. La CI préparée n'est pas une exécution distante réussie.

## Intégration future isolée Zigbee2MQTT

Utiliser exclusivement une instance de laboratoire et un vrai ZNP. Réutiliser les commits hôtes verrouillés et installer leurs dépendances selon leurs fichiers de verrouillage, dans une VM dédiée ; aucun service HA/Proxmox de production n'est modifié. Un broker MQTT de laboratoire doit être isolé de celui de production et Home Assistant discovery désactivé.

Configuration d'exemple, **à créer dans `lab-data/` ignoré**, avec une adresse série spécifique au dongle test :

```yaml
version: 4
mqtt:
  server: mqtt://127.0.0.1:1883
  base_topic: zigbee2mqtt_lab
serial:
  adapter: zstack
  port: /dev/serial/by-id/IDENTIFIANT_EXCLUSIF_DU_DONGLE_LAB
  baudrate: 115200
  rtscts: false
homeassistant:
  enabled: false
advanced:
  network_key: GENERATE
  pan_id: GENERATE
  ext_pan_id: GENERATE
```

Cette configuration exprime l'intention d'isolation ; elle n'a pas été exécutée ni validée contre une instance installée. Choisir le canal de test après étude du plan radio, puis le garder fixe entre mesures. Ne pas copier la configuration, les clés, le PAN ou les sauvegardes de production.

Critères successifs :

1. Démarrage jusqu'à état coordinateur sans faux succès MT ; vérifier SYS/UTIL/NV étendue, endpoints et configuration APP_CNF.
2. Nouveau réseau, ouverture d'association limitée dans le temps ; associer un routeur et un terminal de laboratoire. Fermer ensuite l'association.
3. Lire les descripteurs ZDO, effectuer un échange ZCL réel et vérifier AF dataConfirm et réponse APS. Distinguer ACK MAC, confirmation APS et réponse applicative.
4. Débrancher un routeur test, observer reconstruction de routes et retour du service ; tester un appareil à pile avec son intervalle de poll réel. Ne pas le déclarer perdu uniquement parce qu'il dort.
5. Arrêts propres et coupures inopinées ; redémarrage à froid, maintien du réseau et des compteurs. Vérifier absence de réutilisation d'un compteur de trame sécurisé.
6. Sauvegarde/restauration puis retour arrière sur ce même laboratoire ; vérifier communication sans réassociation manuelle et cohérence des clés. Le schéma NV brut n'est pas supposé compatible entre piles.
7. Endurance 24 h puis 7 jours ; charge progressive et réseau multi-sauts, sans pertes cachées derrière les réessais. Exporter seulement des statistiques dépourvues de secrets.
8. Hors périmètre courant, différé à la demande de l'utilisateur : autobaud, démarrage/reconnexion aux trois débits et capture DMA sous charge radio. Ce jalon ne conditionne pas la qualification du profil fixe ; il sera nécessaire avant de réactiver l'autodétection.

## Contrôle statique supplémentaire du ZNP complet

Après compilation réelle, conserver : versions/empreintes du SDK, compilateur TI et SysConfig, patch effectif, options de préprocesseur, rapport Flash/SRAM/linker et empreinte du HEX. Contrôler le nombre de pages NV dans le linker et NVOCMP, la taille de pile/tas, les GPIO RF et le support MT requis par le snapshot herdsman. Le diagnostic `image.py` doit passer sur l'image finale, pas uniquement sur un fichier CCFG source.

**Phase F : série/NV, moteur herdsman réel et échanges avec une HG06106C exécutés, y compris reprise après reset et restauration de sauvegarde sans réappairage.** Voir [radio-lab.md](radio-lab.md). Restent le service Z2M/MQTT, les terminaux endormis, le maillage représentatif, la répétabilité et l'endurance. L'autodétection reste différée.
