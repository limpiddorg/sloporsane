# Stockage des sessions et historique paginé

## Architecture retenue

Chaque session possède deux fichiers principaux :

```text
<session-id>/
  session.sqlite
  info.json
```

SQLite est la source de vérité, pas un index supprimable. `info.json` est un
résumé lisible et recréable des métadonnées (dont mode, modèle et réflexion).
Les nouvelles sessions ne créent ni `messages.jsonl` ni `messages.index.jsonl`.
SQLite 3.50.4 est compilé statiquement dans le daemon depuis l'amalgamation
publique officielle, vérifiée par SHA-256. Son code est dans le domaine public :
https://www.sqlite.org/copyright.html. Aucun exécutable `sqlite3` n'est requis.

## État livré

`SessionDatabase` possède la connexion, les requêtes préparées et les transactions.
`SessionJournal` conserve son nom et son API de messages, mais utilise uniquement
SQLite. Schéma courant 6, versionné par `user_version` et identifié par
`application_id`. Les tables actuelles contiennent les messages par position et
identifiant unique, leurs résumés séparés des corps complets et les métadonnées
nécessaires à la reprise. Un index rôle/position permet l'accès direct au dernier
message utilisateur. Les données de replay provider restent conservées.

`SessionRecoveryIndex` conserve les appels en attente, identifiants
d'appels résolus, actions refusées, audit des approbations et dernier état par
action exacte. L'index est mis à jour dans la transaction du message source.
Le registry ne parcourt plus les résumés pour réparer une exécution interrompue.
Les permissions lisent directement les actions enregistrées, sans décoder les
sorties d'outils. Les refus équivalents partagent le dernier état d'approbation ;
le rapport du prochain tour conserve toutes les bascules chronologiques.
Les lectures restent proportionnelles aux appels en attente ou aux états de
permission retournés, pas à tous les messages. La réponse d'états d'approbation
retourne encore l'ensemble des refus concernés, sans pagination.

Les messages d'un événement et ses métadonnées sont enregistrés dans une même
transaction. Le checkpoint de compaction et sa frontière de contexte le sont
également. Un échec annule toute la transaction, sans tronquer de fichier.
Le cache SQLite est configuré à environ 4 Mio par connexion ; cela ne borne pas
encore la mémoire des objets d'affichage de l'application.

`info.json` est publié par remplacement atomique à la création, au début d'un tour,
à sa fin, lors des changements de réglages et des sauvegardes explicites. Les
événements intermédiaires ne le réécrivent pas systématiquement. Une erreur de
publication est signalée mais ne transforme pas un commit réussi en échec.
La reprise lit toujours les métadonnées SQLite. La liste utilise le cache et le
recrée depuis SQLite s'il est absent ou invalide ; un cache valide mais ancien peut
rester périmé jusqu'à la prochaine ouverture de cette session.

## Formats et durabilité

Seule une base neuve (version 0) ou le schéma courant 6 est accepté. Les anciens
schémas internes sont refusés sans migration ni remplacement de leurs messages.
Les fichiers JSONL et anciennes métadonnées restent intacts et ne sont jamais
importés ; `info.json` ne peut pas initialiser une base. Une base corrompue est
refusée, jamais remplacée depuis un autre format.

Les index de recovery et d'affichage sont créés avec la base neuve et mis à jour
dans les transactions des messages. À l'ouverture, aucun ancien agrégat n'est
reconstruit et aucun corps n'est chargé pour compléter un nom de skill absent.
Un audit d'approbation sans refus IA qualifié antérieur est rejeté, jamais
transformé en autorisation.

Le mode WAL et `synchronous=FULL` demandent à SQLite de synchroniser chaque commit
sur le stockage. La garantie dépend aussi du système de fichiers et du matériel.
`session.sqlite-wal` et `session.sqlite-shm` peuvent accompagner la base ouverte.
Ne pas les supprimer ni copier seulement la base pendant une exécution : utiliser
l'API de sauvegarde SQLite ou fermer proprement le daemon avant de copier le dossier.
Un seul daemon par dossier de sessions reste requis.

## Pagination restante

`SessionHistoryService` consulte maintenant `SessionHistoryIndex` dans SQLite.
Il n'appelle plus `SessionHistoryView::update()` et ne charge plus tous les
résumés. Les pages utilisent des ancrages indexés et des plages de positions,
sans OFFSET parcourant les anciennes lignes. Une Activity repliée lit son résumé
préagrégé, sans charger ses éléments. Le rendu réutilise les fonctions de
`SessionHistoryView`, dont l'ancien projecteur sert aussi d'oracle aux tests.

La projection est mise à jour dans la transaction du message, y compris lorsqu'un
résultat tardif complète une ancienne Activity. Les durées, compteurs modèle et
métadonnées finales ne nécessitent pas de reconstruire les groupes à l'ouverture.
Les sélections de pages restent des positions stables, pas des snapshots : les
groupes peuvent être actualisés entre sélection et lecture durant l'exécution.

Les contenus individuels sont encore lus en entier. Les Activities ouvertes,
dont la dernière préchargée comme auparavant, chargent encore tous leurs éléments.
Les lectures de détails récupèrent les résumés des messages sources par position ;
un message comportant énormément de parties reste donc coûteux. Les pages sont
bornées en messages visibles, pas encore en octets ni en éléments auxiliaires.

Le contrat cible reste une ouverture et des pages bornées en éléments et en octets :

1. Remplacer le scan des checkpoints lors de la préparation du contexte par des
   requêtes dédiées. Les scans d'approbations et d'appels interrompus sont retirés.
2. Paginer les éléments d'Activity et séparer les gros contenus en portions, avec
   budgets explicites dans le protocole et la TUI.
3. Étendre les mesures de budgets au chemin des gros contenus et Activities
   ouvertes. Les tests comparent déjà la même dernière page après 30 et 3 000
   anciens messages et vérifient une Activity repliée de 100 000 éléments.

`bytesRead()` mesure les octets des corps retournés par le lecteur, pas les entrées/sorties physiques
totales de SQLite. `summaryRowsRead()` compte les résumés décodés. Les tests de
reprise auxiliaire vérifient que ces deux compteurs restent à zéro, même avec
plusieurs milliers de messages et de grosses sorties refusées.
Les tests d'équivalence comparent pages, résumés et détails à l'ancien projecteur
après chaque ajout et réouverture. Ils vérifient aussi rollback, annulation de
création et rejet non destructif des anciens formats. Ces tests ne sont pas une mesure des lectures physiques ni une garantie
de mémoire bornée pour les contenus et Activities ouverts, restant à paginer.
