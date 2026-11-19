# Récupération et retour au firmware d'origine

Plan préparé, aucun essai de récupération matériel exécuté.

L'utilisateur a précisé le 17 novembre qu'il ne dispose que du coordinateur de production. Le [plan pour l'unique dongle](single-dongle-rollback.md) détaille les sauvegardes et le retour arrière à vérifier avant d'envisager ces essais. Cette précision n'autorise pas à elle seule un arrêt, un reset ou un flash.

## Niveau 1 : bootloader ROM

Le bootloader est en ROM et n'est pas remplacé par une image applicative. Son accès dépend néanmoins des champs CCFG en Flash. Pour nos images, les deux champs d'activation valent 0xC5 et DIO15 bas déclenche la backdoor. L'auto-BSL SONOFF pilote DTR/RTS pour réaliser cette séquence ; sur une carte de laboratoire, la séquence manuelle équivalente utilise BOOT puis RESET selon la documentation du modèle P.

Si le diagnostic ne répond plus, essayer l'entrée ROM sur le **dongle de laboratoire**, indépendamment du logiciel applicatif et du débit MT. Le débit BSL et sa synchronisation sont gérés par cc2538-bsl ; ne pas confondre le protocole ROM avec NPI.

Référence : [TI SWRA466, Serial Bootloader Interface](https://www.ti.com/lit/an/swra466e/swra466e.pdf), [implémentation SONOFF cc2538-bsl](https://github.com/JelmerT/cc2538-bsl).

## Niveau 2 : programmateur externe

Si CCFG ne permet plus l'entrée ROM ou si le câblage auto-BSL échoue, utiliser un XDS110 compatible TI et les signaux cJTAG TCK/TMS, nRESET, GND et référence de tension de la carte. La position exacte des pads SONOFF n'est pas établie par un schéma public dans cet audit : aucune numérotation de pad n'est fournie au hasard.

Alimenter avec une seule source maîtrisée, référencer le programmateur au 3,3 V logique et utiliser les outils TI adaptés au CC2652P. Une opération de déverrouillage/effacement peut détruire toute la Flash et les secrets NV ; ne pas la présenter comme une restauration non destructive. Le contrôle fourni garde CPU_DAP et l'effacement permis, mais ne remplace pas une vérification réelle de la liaison debug. FCFG est une zone d'usine : ne pas y écrire.

## Retour au firmware d'origine du laboratoire

1. Identifier une image `CC1352P2_CC2652P_launchpad_coordinator` provenant d'une source connue, correspondant à la référence P et à la version voulue. Vérifier son empreinte et son CCFG avant de l'utiliser. Les versions `other`, P7, E et routeur ne sont pas interchangeables.
2. Rétablir cette image via BSL ou programmateur, avec vérification de l'écriture. Un effacement implique une NV vierge : l'image de firmware seule ne recrée pas un réseau.
3. Restaurer uniquement la sauvegarde du réseau de laboratoire via les mécanismes hôtes compatibles, si ce réseau doit être conservé. Ne pas injecter une NV brute d'une autre version sans compatibilité explicitement démontrée.
4. Vérifier identité du coordinateur, canal, PAN, compteur de sécurité, échanges avec le routeur et terminal de test, redémarrage à froid et réveil des équipements endormis.

La version de production déclarée 20240710 apparaît dans le changelog Koenkk et utilise le SDK 7.41.00.17. Le simple fait de télécharger une release de ce nom ne prouve pas qu'elle est strictement identique à votre binaire actuel. Une restauration de production future doit disposer de son propre dossier de preuve et d'une autorisation ; elle n'est pas exécutée dans ce projet.

Ne jamais faire fonctionner simultanément deux coordinateurs possédant l'identité et les clés du même réseau. Les essais de ce dépôt utilisent des identités et réseaux distincts.
