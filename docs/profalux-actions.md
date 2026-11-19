# Actions Profalux MAI-ZTM20C : diagnostic du 18 novembre 2026

Les événements de boutons restent absents pour la télécommande testée. L'utilisateur indique le même comportement avec le firmware d'origine ; cette comparaison n'a pas été observée directement pendant cette séance. Aucun correctif n'est validé.

## Observations

Lors de la première séquence Montée, Stop, Descente confirmée par l'utilisateur, la capture Z2M reçoit quatre messages AF de la télécommande, tous sur `genPowerCfg` : rapport de batterie et réponses de lecture. Aucune commande de bouton n'est livrée sur `genLevelCtrl` ou `closuresWindowCovering`. Les changements du volet sont reçus pendant les appuis.

La lecture complète de la table réelle de la télécommande confirme cinq liaisons :

| Endpoint source | Cluster | Destination |
|---|---|---|
| 1 | `genLevelCtrl` (`0x0008`) | Coordinateur, endpoint 1 |
| 2 | `genPowerCfg` (`0x0001`) | Coordinateur, endpoint 1 |
| 2 | `closuresWindowCovering` (`0x0102`) | Moteur, endpoint 2 |
| 2 | Cluster propriétaire `0xFC20` | Moteur, endpoint 2 |
| 2 | `closuresWindowCovering` (`0x0102`) | Coordinateur, endpoint 1 |

Les liaisons standard vers le coordinateur ne manquent donc pas dans cette table. La présence d'une liaison propriétaire ne prouve pas que les boutons utilisent ce cluster.

## Essai de liaison supplémentaire

Une liaison `0xFC20`, endpoint 2 de la télécommande vers endpoint 1 du coordinateur, est ajoutée par requête ZDO. La télécommande répond SUCCESS ; une relecture complète confirme six entrées, dont les cinq initiales intactes.

Après une nouvelle séquence Montée, Stop, Descente confirmée par l'utilisateur, aucun message AF de bouton de cette télécommande n'est reçu. Les rapports du moteur restent visibles. Cette liaison n'apporte donc pas de solution dans l'essai effectué.

La seule liaison ajoutée est ensuite retirée. Une relecture complète vérifie le retour exact aux cinq entrées initiales. Les niveaux de journaux temporaires sont également restaurés et vérifiés dans la configuration effective. Aucun flash ni réappairage n'est réalisé pendant ces diagnostics.

## Limites

La capture porte sur les messages livrés par le coordinateur à Z2M, sans capture radio indépendante. Elle ne permet pas d'identifier le cluster et la destination réellement émis lors des appuis, ni de localiser un éventuel filtrage avant AF. La commande directe du moteur reste une hypothèse ; l'absence de messages ne suffit pas à attribuer la cause à la télécommande, au convertisseur ou au coordinateur.

Les scripts ponctuels, captures, transactions et tables détaillées sont conservés sous `private/`, hors Git. Les observations de ce document ne qualifient pas tous les exemplaires de ce modèle.
