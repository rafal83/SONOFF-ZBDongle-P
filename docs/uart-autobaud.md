# UART fixe et autodétection différée

Depuis les essais physiques du 17 novembre 2026, **le build par défaut utilise 115200 bauds fixes**. Le passage automatique à 460800 a échoué sur le dongle réel. L'utilisateur a demandé de reporter cette fonction. Les profils `--baudrate 460800` et `--baudrate 1000000` sont également fixes et doivent correspondre au débit Z2M.

Les deux profils fixes élevés ont maintenant passé les essais série/NV sur le dongle. Le profil 1 Mbaud a aussi passé les échanges réels avec la HG06106C, y compris sous écritures NV concurrentes. Cela valide le débit fixe ; le changement automatique reste désactivé. Voir [performance.md](performance.md).

Le code expérimental reste accessible avec `--experimental-autobaud`. Cette option produit un dossier et une archive portant le suffixe `-autobaud`, avec `auto_baud: true`. Ce paquet n'a pas réussi les essais matériels de changement de débit.

L'acquisition expérimentale surveille DIO12 avec GPT0 et DMA14. Huit descripteurs alternent quatre horodatages et l'effacement W1C du drapeau IOC. Quatre transitions de `FE 00` mesurent deux bits bas, dix bits jusqu'au start suivant et neuf bits bas. Le quatrième front rejette l'alias du réveil `EF`. Le mux UART reste actif et les événements passent par AON_PROG2. Le routage initial DMA14 peut être NONE ou AON_PROG0 (valeur au reset), puis est restauré exactement. Les conflits de ressources font refuser l'acquisition.

Le contrôleur expérimental réarme la capture après chaque trame valide, mesure ou expiration de 100 ms. La tâche NPI vérifie le transport toutes les 10 ms, traite d'abord les trames déjà valides et attend la fin du TX avant une réouverture UART. L'annulation synchrone de lecture est protégée et un refus d'ouverture restaure le débit précédent. En mode fixe, aucune acquisition n'est démarrée et aucun événement UART ne déclenche un changement de débit.

Les tests natifs et ARM vérifient le code et les descripteurs avec des périphériques simulés. Leur réussite n'a pas prédit le résultat matériel : le routage, le déclenchement et la latence réelle de capture restent à investiguer. Aucun succès matériel d'autodétection n'est revendiqué. Voir le [rapport physique](hardware-validation-20261117.md).

Référence : [manuel CC13x2/CC26x2](https://www.ti.com/lit/ug/swcu185/swcu185.pdf).
