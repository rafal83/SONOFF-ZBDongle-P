# Dépendances et licences

| Composant | Source et version | Licence et périmètre |
|---|---|---|
| Contributions originales | Pile FreeRTOS, algorithmes et outils du projet | MIT à la racine ; droits tiers exclus |
| Noyau | FreeRTOS-Kernel V11.1.0, commit verrouillé | MIT ; texte upstream joint au paquet |
| Support CC2652P | SDK TI 8.33.00.16 : startup, driverlib, patch radio, en-têtes | Notices BSD par fichier ; empreinte du SDK vérifiée ; extraction limitée au support matériel |
| Configuration radio | SysConfig/SmartRF TI et adaptations Koenkk | Notices TI et MIT Koenkk ; voir `NOTICE.md` et `LICENSES/` |
| Dispositions NV / compatibilité ZNP | zigbee-herdsman 11.2.1 | MIT ; notice dans le code généré et le paquet |
| Référence de configuration radio | Koenkk/Z-Stack-firmware, commit verrouillé | MIT pour les contributions Koenkk ; ne remplace pas les droits TI |
| Flash / sauvegarde locale | cc2538-bsl, commit verrouillé | Licence upstream ; outil hôte séparé |
| Compilation | Zig / Clang / LLD 0.13.0 | Licences Zig/LLVM ; outils non incorporés au firmware |
| Tests ARM | Unicorn, Capstone, pyelftools | Licences respectives ; outils hôtes non liés au firmware |
| Tests cryptographiques | cryptography, PyCryptodome | Licences des paquets ; outils hôtes uniquement |
| Compatibilité hôte | zigbee-herdsman et dépendances npm verrouillées | Licences upstream ; aucun `node_modules` dans les images |

La chaîne de build ne lie pas les bibliothèques fermées Z-Stack/MAC de TI. Le manifeste TI et les avis exacts des sources TI compilées sont joints aux images. La licence MIT de ce projet ne remplace pas les conditions tierces.

Références : [manifeste TI SDK 8.33](https://software-dl.ti.com/simplelink/esd/simplelink_cc13xx_cc26xx_sdk/8.33.00.16/exports/manifest_simplelink_cc13xx_cc26xx_sdk_8_33_00_16.html), [FreeRTOS](https://github.com/FreeRTOS/FreeRTOS-Kernel/blob/V11.1.0/LICENSE.md), [zigbee-herdsman](https://github.com/Koenkk/zigbee-herdsman/blob/master/LICENSE), [Koenkk](https://github.com/Koenkk/Z-Stack-firmware/blob/master/LICENSE).
