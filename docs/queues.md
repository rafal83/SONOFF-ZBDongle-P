# Files de messages bornées et ajout constant

Les messages entre tâches utilisent maintenant une référence à la fin de chaque file. Son offset SRAM de 16 bits occupe du padding existant : taille et offsets des champs TI restent inchangés, soit 24 octets par tâche sur ARM. Le décodage utilise l'arène SRAM du CC2652P et conserve un repli sûr pour les buffers extérieurs ou non alignés. Les messages alloués par le tas normal suivent le chemin constant.

L'ajout ne parcourt plus les messages en attente. Dequeue, insertion en tête, extraction et recherche avec suppression maintiennent cette référence. Les files MAC indépendantes qui ne sont pas des files de tâches conservent leur contrat d'origine. Les recherches par événement restent linéaires. La réception et la mise à jour de son bit de réveil sont atomiques, empêchant de perdre un réveil si un producteur arrive au moment où la file devient vide. La saturation des quinze inscriptions de tâches renvoie `TASK_NO_TASK`, sans dépasser la table. `msgEnqueueMax` respecte exactement sa limite, y compris zéro ; la version d'origine pouvait admettre un message de trop.

La file publique de réception des broadcasts est remplacée par un FIFO statique de dix pointeurs, correspondant à `MAX_BROADCAST_QUEUED`. Enqueue/dequeue sont constants, sans allocation. Un refus laisse la propriété du message à l'appelant ; le consommateur NWK retrouve le message accepté. Les événements coalescés sont réarmés si nécessaire. Le linker redirige les références publiques et l'audit refuse une image conservant les anciennes fonctions de file. La table de transactions broadcast reste séparément dimensionnée à 40.


La sécurité, le routage, les retries, la détection des doublons et les acquittements passifs restent ceux de TI. Le FIFO n'est pas un remplacement du traitement complet des échos broadcast ni de la file indirecte NWK.
