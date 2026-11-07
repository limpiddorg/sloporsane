# Coût d'exécution Sane et Slop

## Snake : tous les correctifs

Essai réel `snake-all-fixes-sane-1`, même prompt, `gpt-6.1-sol`, medium,
contexte 272 000 et Auto/Jev 60. Daemon de mesure recompilé avec le provider
corrigé, pas de pilotage de la TUI ni d'attente humaine. Un essai terminé,
aucun résultat écarté. Les autres colonnes reprennent les essais conservés.

| Section (temps provider) | Slop référence | Sane validation ciblée | Sane transitions auto | Sane tous correctifs |
| --- | ---: | ---: | ---: | ---: |
| Objective Plan | — | 24,1 s | 22,7 s | 19,7 s |
| Implementation Plan | — | 31,1 s | 31,5 s | 22,3 s |
| Lectures de construction | 2,7 s | 13,0 s | 15,6 s | 6,5 s |
| Écriture/édition | 110,0 s | 139,8 s | 150,6 s | 142,9 s |
| Annonces de progression | — | 63,7 s | 32,0 s | 12,8 s |
| Contrôles et vérifications | 2,7 s | 66,7 s | 39,9 s | 11,8 s |
| Clôtures de steps | — | 22,6 s | 18,8 s | 13,9 s |
| Réponse finale séparée | 5,1 s | Incluse | Incluse | Incluse |
| Total provider | 120,4 s | 361,2 s | 310,9 s | 229,8 s |
| Hors provider, toutes phases | 9,0 s | 26,9 s | 20,9 s | 18,6 s |
| Total mural | 129,4 s | 388,1 s | 331,9 s | 248,4 s |
| Construction seule, murale | 129,4 s | 328,6 s | 273,6 s | 202,5 s |
| Appels modèle, toutes phases | 10 | 42 | 32 | 22 |
| Appels modèle, construction | 10 | 37 | 27 | 17 |
| Cache global (tokens cachés / entrée) | 91,49 % | 88,73 % | 88,23 % | 90,05 % |

Résultat actuel : 285 568 tokens cachés / 317 120 tokens d'entrée, 9 237 de
sortie. Construction seule : 267 904 / 286 234, soit 93,60 % de cache. Les
premiers appels des trois agents successifs (Discussion/planification/exécution)
sont à zéro ; les suivants réutilisent le cache. Transport confirmé : trois
connexions successives, 22 requêtes, 19 continuations ; le même agent de
construction garde sa connexion et ses 16 continuations sur 17 requêtes,
y compris la transition entre steps.

Le plan contient deux steps et quatre substeps, contre trois steps et six
substeps précédemment. Quatre comptes rendus completed, aucun started/validating
séparé. La validation finale groupe bash/read/grep dans une réponse, sans
simulation DOM/timers ad hoc ni répétition des tests moteur inchangés.

Gain mural observé : 83,4 s (25,1 %) sur le Sane précédent, 139,7 s (36,0 %)
sur celui d'avant les transitions automatiques. Construction seule : 71,1 s
(26,0 %) et 126,1 s (38,4 %) respectivement. Le ratio de construction sur le
Slop conservé passe à 1,56 ; celui du total avec plans à 1,92. Les gains
cumulent taille du plan, automatisation, consignes et format Codex ; une seule
génération par état ne démontre pas leur causalité séparée ni la reproductibilité.
Le taux de cache global ne progresse que modestement : ne pas attribuer la baisse
du temps au seul cache.

Hors chrono : les 14 tests générés passent et la syntaxe des trois modules est
valide. Aucun fichier du jeu modifié, pas de navigateur réel ni de nouvelle
batterie d'acceptation indépendante. Artefacts sous
`build-ai/benchmarks/snake-modes/snake-all-fixes-sane-1/` : result, modèles,
événements et checkout d'exécution dans `data/sos/worktrees/`.

## Format Codex corrigé et vérifié dans les deux modes

Le diagnostic réel du 400 est `System messages are not allowed` lorsque
`role: system` est envoyé avec `type: message`. La même entrée sans `type`
(EasyInputMessage) est acceptée, avec contenu chaîne ou tableau `input_text`.
Une consigne système tardive dans cette forme est effectivement suivie.
Ce comportement a été testé sur l'endpoint Codex OAuth, pas déduit du seul
schéma de l'API Responses publique.

Le provider commun à Sane/Slop garde donc `instructions` constant à
`Follow the conversation instructions.` et retire uniquement `type` des messages
`system` de la copie réseau Codex. Ordre, rôle et contenu sont conservés ;
l'historique d'origine et le format OpenAI API classique restent inchangés.

Contrôle direct HTTP avec identité stable et historique synthétique croissant :
6 appels, tokens cachés successifs 0, 0, 6400, 6528, 6528, 6528 ; les appels 3
et 5 ajoutent une consigne système tardive sans modifier le préfixe. Taux de cache
96,59 % et 97,13 % sur ces deux appels, sans garantie de hit ni gain de latence.
Artefact : `build-ai/benchmarks/snake-modes/codex-history-cache-1.jsonl`.

Après correction du provider, contrôle réel via Integration/Agent : création du
titre/session puis trois tours Slop → Sane → Slop réussis, sans outils, avec les
trois réponses attendues. Artefact : `codex-fixed-format-modes-1/` dans le même
dossier. 44 tests locaux provider, titres, transformer et WebSocket réussis ;
build `sosd` réussi. Pas de nouveau benchmark Snake pour cette seule correction.

## Optimisations suivantes : sans nouvelle mesure de temps

Le démarrage initial du premier substep est aussi activé par le harness avant
la première génération. Une reprise réutilise la frontière existante ; les
révisions conservent leurs cibles explicites. Les consignes de planification
limitent le périmètre des tests aux comportements demandés et risques concrets.
Validation et clôture privilégient contrôles courts, résultats réutilisés et
explications sans récapitulatif redondant, sans supprimer les exigences explicites.

`prepareCodexInstructions` est supprimé. Le champ Codex requis `instructions`
reste constant ; les éléments `system`/`developer` restent dans `input`, à leur
position d'origine. Les annonces tardives n'altèrent plus le préfixe de la
requête précédente. Les historiques stockés ne sont pas réécrits et les
frontières de compaction restent explicites.

Validation locale : 189 tests runtime et 54 tests provider/Implement réussis,
build `sosd` réussi. Tests de stabilité du préfixe HTTP et de continuation WS
après ajout d'annonces, activation initiale sans appel modèle, reprise et flow
Implement sur le protocole. Aucun gain de temps ou de cache mesuré à ce stade.
Le contrôle réel `cache-preserved-instructions-1` est bloqué par HTTP 400 avant
création de session et avant tout appel modèle instrumenté (pas de `models.jsonl`).
Il ne valide donc pas l'acceptation du nouveau format par le serveur réel.

## Snake : transitions automatiques

Après finalisation/abandon d'une substep en construction normale, le harness
persiste atomiquement l'activation de la première restante, ou la validation.
Le démarrage initial par step et les réparations/révisions restent explicites.
Les frontières utilisent le même snapshot et n'ajoutent ni diff ni succès fictif.

Nouvel essai Sane réel, même prompt et paramètres que la paire précédente.
Les deux premières colonnes sont les essais précédents conservés, pas de nouvelles
exécutions. Temps par section = temps provider, pas durée des outils.

| Section | Slop précédent | Sane précédent | Sane transitions auto |
| --- | ---: | ---: | ---: |
| Objective Plan | — | 24,1 s | 22,7 s |
| Implementation Plan | — | 31,1 s | 31,5 s |
| Lectures de construction | 2,7 s | 13,0 s | 15,6 s |
| Écriture/édition | 110,0 s | 139,8 s | 150,6 s |
| Annonces de progression | — | 63,7 s | 32,0 s |
| Contrôles et vérifications | 2,7 s | 66,7 s | 39,9 s |
| Clôtures de steps | — | 22,6 s | 18,8 s |
| Réponse finale séparée | 5,1 s | Incluse | Incluse |
| Total provider | 120,4 s | 361,2 s | 310,9 s |
| Hors provider | 9,0 s | 26,9 s | 20,9 s |
| Temps mural | 129,4 s | 388,1 s | 331,9 s |
| Appels modèle | 10 | 42 | 32 |

Gain observé Sane : 56,2 s (14,5 %), ratio sur le Slop précédent 2,56 contre
3,00. Neuf annonces au lieu de 17 : trois démarrages initiaux et six résultats,
aucun started/validating séparé pour les transitions automatiques. Le plan
comporte six substeps au lieu de sept et réutilise explicitement les résultats
de tests/syntaxe après la documentation. Les nouvelles consignes conditionnelles
de validation sont également présentes : ce n'est pas une mesure isolée des
transitions, ni un gain reproductible établi sur une seule génération.

Un premier essai conservé (`snake-auto-sane-1`, 370,40 s, deux steps) a utilisé
la sélection locale entre-temps passée à `gpt-6-astra`, contexte automatique.
Il ne figure pas dans la comparaison homogène. Le second essai
`snake-auto-sane-sol-1` fixe les paramètres publics depuis le résultat précédent
avec `SOS_BENCH_REFERENCE` sans modifier la configuration utilisateur.

Vérification hors chrono de l'essai comparable : 15 tests générés réussis et
syntaxe des trois modules valide. Aucun livrable modifié par le vérificateur,
pas de navigateur réel. Les tests Implement couvrent aussi activation/reprise,
appartenance des diffs, réparation ciblée et abandon d'une substep sans clôture
automatique du step. La validation humaine reste distincte.

## Snake : consignes de validation ciblée

Nouvelle paire, même prompt/configuration et même harness recompilé, Slop puis
Sane, après ajout des consignes de validation proportionnée et de réutilisation
des résultats encore valides. Aucun essai terminé exclu.

| Mesure | Slop | Sane |
| --- | ---: | ---: |
| Temps mural | 129,41 s | 388,10 s |
| Appels modèle | 10 | 42 |
| Temps provider | 120,43 s | 361,18 s |
| Construction hors planification | 129,41 s | 328,62 s |
| Tokens d'entrée | 69 256 | 819 534 |
| Tokens de sortie | 5 247 | 13 331 |
| Tokens cachés | 63 360 | 727 168 |

Sane est 3,00 fois plus lent que Slop et prend 38,47 s de plus que la paire WS
précédente (+11,0 %). Deux appels modèle de moins ne compensent pas le contenu
supplémentaire généré : 17 rapports de progression et trois clôtures restent.
Les contrôles prennent 66,72 s provider, contre 44,95 s précédemment, lectures
de validation comprises. Le plan généré exige explicitement une nouvelle passe
de tests/syntaxe et une inspection finale des six fichiers/diffs. Il maintient
donc des répétitions malgré les consignes d'exécution ciblée ; le modèle réutilise
en revanche les résultats du stub DOM au dernier step. Le contrôle de wiring
du deuxième step génère un script Node conséquent, puis le dernier step génère
encore un script de vérification de périmètre et de comparaison avec HEAD.
Pas de gain établi sur cette paire non déterministe, ni de causalité démontrée.

Vérification hors chrono : 9 tests générés Slop et 16 Sane passent, ainsi que
les contrôles de syntaxe de leurs trois modules. Aucun fichier généré modifié,
pas de vérification navigateur réelle ni de nouvelle batterie indépendante.
Artefacts : `build-ai/benchmarks/snake-modes/snake-focused-{slop,sane}-1/`.

## Snake : reprise avec identité et WebSocket

Le même benchmark Snake a été relancé après les correctifs d'identité et de
transport, sur les sources courantes recompilées sous `build-ai/`. Ce n'est pas
un pilotage de la TUI installée : même daemon de mesure, vraies classes runtime,
pas d'attente humaine, même prompt et même configuration OpenAI `gpt-6.1-sol`,
`medium`, contexte 272 000, Auto/Jev au seuil 60 %. Ordre : Slop puis Sane.
Le traitement des instructions système n'a pas été corrigé.

| Mesure | Slop précédent | Slop identité + WS | Sane précédent | Sane identité + WS |
| --- | ---: | ---: | ---: | ---: |
| Temps mural | 119,51 s | 122,69 s | 357,44 s | 349,63 s |
| Appels modèle | 9 | 9 | 43 | 44 |
| Temps cumulé provider | 110,89 s | 113,51 s | 327,87 s | 323,14 s |
| Tokens d'entrée | 58 767 | 59 109 | 838 417 | 871 239 |
| Tokens de sortie | 4 885 | 4 906 | 12 234 | 11 590 |
| Tokens cachés | 3 968 | 47 872 | 53 120 | 768 768 |
| Taux de cache | 6,75 % | 80,99 % | 6,34 % | 88,24 % |

Le transport est confirmé : Slop utilise une connexion, neuf requêtes et huit
continuations. Sane utilise trois connexions successives (Discussion,
planification Implement puis agent de construction), 44 requêtes et 39
continuations. Aucun appel supplémentaire de récupération de continuation
n'est compté. Le bloc d'instructions change toujours aux transitions Implement :
cinq états successifs au total, comme lors du benchmark précédent.

Sane gagne **7,80 s, soit 2,18 %**, sur cette paire. Sa phase de construction
passe de 308,82 s à 292,80 s (**−5,19 %**), tandis que ses planifications prennent
plus longtemps. Il reste **2,85 fois plus lent que Slop**, contre 2,99 auparavant.
Environ 92 % de l'écart actuel demeure dans les appels provider. Le cache
augmente fortement, mais il ne supprime ni les réponses à générer ni les
nombreux allers-retours : 17 rapports de progression et trois clôtures de step
restent présents. On ne peut pas attribuer causalement ces quelques secondes au
seul transport : une paire par configuration, générations non déterministes,
plan et contenu différents, charges serveur non contrôlées.

Vérification hors temps mesuré : dix tests générés passent en Slop, quinze en
Sane ; syntaxe des deux modules et sept mêmes contrôles indépendants passent
dans chaque cas. `acceptance-ws.mjs` adapte uniquement les noms de fonctions et
vérifie le mouvement résultant pour les modèles de direction différents. Aucun
fichier généré n'a été corrigé par le vérificateur. Pas de contrôle navigateur réel.

Artefacts locaux : `build-ai/benchmarks/snake-modes/snake-ws-slop-1/` et
`snake-ws-sane-1/`, avec `result.json`, `models.jsonl`, `events.jsonl`, le prompt
et les jeux produits. L'essai Sane a son code dans son checkout d'exécution
sous `data/sos/worktrees/`, pas dans le dépôt utilisateur initial vide.

## WebSocket et continuation : contrôle Slop

Le transport Codex des conversations identifiées utilise un WebSocket TLS
persistant (IXWebSocket 11.4.6). Quand les paramètres sont identiques et que
l'entrée suivante conserve exactement l'entrée précédente plus sa réponse
normalisée, il envoie `previous_response_id` avec seulement les nouveaux items.
Un changement de contexte, outils, instructions ou modèle repart avec l'entrée
complète. Aucune donnée de connexion/continuation n'est persistée dans l'historique.

Le handshake indisponible provoque un repli HTTP avant soumission de génération.
Les environnements configurés avec un proxy HTTP restent sur curl pour ne pas
contourner le proxy. Une rupture après soumission remonte au mécanisme normal de
retry, sans rejeu silencieux de sortie partielle ni des outils. Une référence
`previous_response_not_found` est retentée une fois avec le contexte complet.
L'annulation ferme la connexion ; les connexions inactives depuis cinq minutes
ou âgées de 55 minutes sont remplacées au prochain appel. Une nouvelle session
ou une modification des headers d'authentification repart sur une autre connexion.
Le header `x-codex-turn-state` et le traitement des instructions système restent
inchangés.

Même petite conversation de six échanges/sept appels que le contrôle ci-dessous,
avec l'identité active dans les trois essais. Ordre : WS 1, HTTP, WS 2. Réponses
correctes, une seule lecture ; instructions, outils et préfixes vérifiés stables.

| Essai | Tokens cachés / entrée | Taux | Temps cumulé provider | Temps mural du client |
| --- | ---: | ---: | ---: | ---: |
| WebSocket 1 | 34 688 / 50 927 | 68,11 % | 17,09 s | 23,93 s |
| HTTP avec identité | 42 368 / 50 879 | 83,27 % | 17,33 s | 24,99 s |
| WebSocket 2 | 42 368 / 50 922 | 83,20 % | 16,86 s | 22,88 s |

Chaque essai WebSocket confirme **une connexion, sept requêtes et six
continuations**, sans tentative supplémentaire : sept items d'entrée transmis
au total, contre 49 en rejouant les fenêtres complètes de ces sept appels.
Les outils et instructions restent dans chaque requête ; ce n'est pas une
suppression de tous les tokens du contexte. L'usage provider compte toujours le
contexte logique complet. La télémétrie enregistre uniquement des compteurs,
empreintes et usages, jamais les credentials ou les corps de requêtes bruts.

**Ce contrôle ne montre pas de hausse du taux de cache par rapport à HTTP avec
identité.** Les temps sont proches et l'échantillon trop petit pour établir un
gain de latence reproductible. La continuation fonctionne et réduit l'entrée
transmise, mais ne justifie pas une promesse de cache systématique.

Artefacts locaux sous `build-ai/benchmarks/snake-modes/` :
`cache-websocket-slop-{1,2}/`, `cache-websocket-http-1/`. Le wrapper de mesure
ajoute `transport.statistics` et permet le témoin via `SOS_BENCH_HTTP_ONLY=1`.
Validation finale : 188 tests runtime réussis, 66 tests provider/session réussis
et trois ignorés car non applicables. Le bundle installé n'a pas été reconstruit.

## Identité de session Codex : contrôle Slop

Le provider Codex transmet désormais l'identifiant persisté de la conversation
dans le header `session-id` et dans `prompt_cache_key`. L'identité reste stable
après reprise et remplacement du provider ; deux conversations gardent des
identités distinctes. Ces champs ne sont pas ajoutés aux requêtes OpenAI API.
La transformation des instructions système n'est pas modifiée ici. Ce correctif
n'ajoute ni WebSocket ni reprise du header de routage `x-codex-turn-state`.

Vérification réelle avec les mêmes réglages que Snake : six échanges Slop,
lecture d'un fichier synthétique de 120 règles puis cinq questions courtes sur
son contenu. Chaque essai utilise les vrais `Integration`, `Session`, `Agent`
et provider, avec sept appels modèle. Les réponses ont été vérifiées : valeurs
correctes, aucune relecture après la première question, aucun fichier modifié.

| Essai | Entrée cumulée | Tokens cachés | Taux |
| --- | ---: | ---: | ---: |
| Avec identité, essai 1 | 50 894 | 34 560 | 67,91 % |
| Témoin sans identité | 50 866 | 0 | 0 % |
| Avec identité, essai 2 | 50 894 | 26 880 | 52,82 % |

Ordre : identité 1, témoin, identité 2. Le témoin désactive uniquement l'envoi
de l'identité dans le wrapper de mesure. Même fixture et mêmes questions, mais
répertoires et sessions distincts et réponses modèle non déterministes.
Les hits après lecture atteignent 97–99 % ; certains appels restent à zéro.
Ce petit contrôle montre la réutilisation effective du cache avec l'identité,
pas un taux garanti, une mesure isolée de chacun des deux champs, ni un gain
de latence établi : le témoin dure environ 25,5 s contre 29,3 s et 24,1 s avec
identité. Le cache déclaré et la latence ne sont pas interchangeables.

Artefacts locaux : `build-ai/benchmarks/snake-modes/cache_conversation.py`,
`cache-session-slop-{1,2}/` et `cache-session-control-1/`, contenant prompts,
événements, usages et `cache-result.json`. Le wrapper de contrôle utilise
`SOS_BENCH_OMIT_SESSION_ID=1`, uniquement dans le daemon de mesure.

## Snake : comparaison avec le vrai modèle

Un même cahier des charges demande un Snake navigateur sans dépendances : six
fichiers, grille 20×20, flèches/WASD, nourriture, croissance/score, collisions,
pause/reprise, restart et tests déterministes avec Node. Deux dépôts Git neufs
séparés ont été utilisés sous `build-ai/benchmarks/snake-modes/`.

Les essais passent par les vraies classes `Integration`, `Session`, `Agent`, les
outils et les providers de SOS. Un exécutable de mesure ajoute uniquement un
wrapper de télémétrie au provider et isole le stockage. L'authentification reste
dans le fichier officiel existant, sans copie des credentials dans le dépôt.
La configuration sélectionnée est identique : OpenAI `gpt-6.1-sol`, raisonnement
`medium`, contexte 272 000, politique Auto/Jev au seuil 60 %. Jev reste activé.

Slop construit directement. Sane suit le parcours Objective Plan, ouverture de
branche, Auto plan puis Everything. Le chrono Sane inclut ces deux planifications,
la préparation du checkout et la construction jusqu'à la clôture technique.
Aucune attente humaine ni validation/import humain n'est incluse. La TUI n'est pas
exécutée : ce test isole le moteur et ne mesure pas sa réactivité graphique.

### Résultats

| Mesure | Slop | Sane |
| --- | ---: | ---: |
| Temps mural | 119,51 s | 357,44 s |
| Nombre d'appels modèle | 9 | 43 |
| Durée cumulée des appels modèle | 110,89 s | 327,87 s |
| Reste du temps mural | 8,62 s | 29,57 s |
| Tokens d'entrée cumulés | 58 767 | 838 417 |
| Tokens de sortie cumulés | 4 885 | 12 234 |
| Tokens d'entrée déclarés cachés | 3 968 (6,75 %) | 53 120 (6,34 %) |

Sane prend **2,99 fois** le temps de Slop sur cette paire. Parmi les **237,92 s**
supplémentaires, **216,97 s**, soit environ **91 %**, sont passées dans les appels
au modèle, pas dans les snapshots ou l'affichage. Les durées provider comprennent
le transport et le traitement serveur ; elles ne mesurent pas séparément le
raisonnement, le préremplissage et la génération.

Les phases Sane observées sont : Objective Plan 22,39 s, ouverture 0,02 s,
Implementation Plan 26,20 s, construction/préparation 308,82 s. Même sans la
planification, la construction Sane prend environ 2,58 fois le tour Slop entier.

### Origine des appels supplémentaires

| Type de réponse modèle | Slop : appels / durée | Sane : appels / durée |
| --- | ---: | ---: |
| Rapports de progression | 0 | 17 / 53,12 s |
| Clôtures techniques de steps | 0 | 3 / 23,90 s |
| Publication Objective Plan + Implementation Plan | 0 | 2 / 37,57 s |
| Lectures | 1 / 2,37 s | 7 / 27,01 s |
| Écriture / édition | 6 / 100,80 s | 7 / 137,75 s |
| Commandes de contrôle | 1 / 2,42 s | 5 / 43,89 s |
| Texte final sans outil | 1 / 5,31 s | 2 / 4,61 s |

Les comptes de ce tableau désignent les **réponses modèle**, pas les appels
individuels d'outils. Une réponse Sane groupe trois lectures, d'où neuf appels
`read` pour sept réponses de lecture.

Le modèle a proposé trois steps et sept substeps : moteur, interface, puis
documentation/vérification finale. Cette décomposition provoque plusieurs effets :

- Les 17 rapports de progression sont tous émis dans des réponses séparées. Le
  moteur permet déjà les appels ordonnés groupés et le prompt le demande, mais
  ce groupement n'a pas été utilisé pour ces annonces pendant cet essai.
- Les frontières obligatoires ajoutent des allers-retours modèle ; leur coût ne
  se limite donc pas à la capture locale effectuée par l'outil de progression.
- Chaque nouveau step relit une partie du travail précédent et relance des
  contrôles. La vérification finale devient même une substep planifiée, puis la
  phase de validation du step déclenche encore des contrôles. Des inspections
  supplémentaires sont faites pour rassurer le modèle alors que la construction
  est déjà préservée.
- Sane génère aussi davantage de contenu : tests plus développés, explications,
  plans et petits scripts de vérification supplémentaires. Son moteur est créé
  puis complété dans un second edit ; Slop écrit son moteur en une fois.
- Les multiples appels transportent une conversation grandissante : le dernier
  appel Slop a 9 220 tokens d'entrée, contre 35 332 pour le dernier appel Sane.
  Les 17 réponses de progression représentent à elles seules 342 790 tokens
  d'entrée cumulés.

Le principal problème reproduit ici est donc une **orchestration trop fractionnée
et répétitive**, pas un outil d'édition quatre fois plus lent.

### Cache et intégrité du préfixe

La télémétrie conserve taille et empreintes, pas les requêtes brutes ni les
credentials. Pour les boucles observées, chaque nouvel input conserve tous les
anciens items du précédent input comme préfixe exact. Les descriptions d'outils
restent stables pendant la construction. Le groupe de messages système change
aux frontières de steps ; l'adaptateur Codex les regroupe dans `instructions`.

Le cache déclaré est faible dans **les deux modes**, environ 6 % du volume
cumulé. Ce test ne permet donc pas d'attribuer spécifiquement l'écart à une
réécriture générale des messages Sane ou à une défaillance de cache propre à
Sane. Le routage/cache fournisseur et l'effet exact des changements de
`instructions` restent à examiner séparément.

### Vérification et limites

Les deux implémentations passent leurs tests générés : neuf tests Slop et quatorze
Sane. Après les chronos, les deux passent aussi les sept mêmes contrôles de
réception du moteur, indépendants des tests générés, ainsi que les contrôles de
syntaxe de `snake.mjs` et `main.mjs`. Aucun vrai navigateur n'a été utilisé ;
le rendu et les interactions dans un navigateur réel restent non vérifiés.

Une seule paire terminée est mesurée. Les sorties ne sont pas identiques et les
performances du fournisseur varient : ce n'est ni une médiane représentative ni
une preuve d'un facteur constant ×3/×4. Deux essais de mise au point du client,
interrompus sur un champ `session.created` puis une réponse de transfert de
branche, sont exclus ; aucun essai arrivé à terme n'a été écarté. Le build utilisé
est celui de `build-ai`, sans optimisation explicite dans la commande inspectée.

Artefacts locaux : `slop-2/result.json`, `sane-2/result.json`, `models.jsonl`
par essai (durées, usage/cache, empreintes et outils), `events.jsonl`,
`verification.json`, prompt et sources générées. Le client reproductible est
`run.py`, le daemon instrumenté est `daemon.cpp`, les contrôles communs sont dans
`acceptance.mjs`, sous `build-ai/benchmarks/snake-modes/`.

## Snapshots : mesure locale sans modèle

Le microbenchmark indépendant sous `build-ai/benchmarks/snapshot-cost/` appelle
le vrai code de snapshot/édition, sans modèle, Jev ou TUI. Chaque cellule
récurrente est la médiane de cinq mesures après une première capture neuve.
Les fichiers synthétiques sont uniques et suivis par Git ; les caches système
ne sont pas vidés.

| Dépôt | Volume source | Capture sans changement | Petit edit Slop | Petit edit Implement |
| --- | ---: | ---: | ---: | ---: |
| 1 000 × 4 Kio | 3,9 Mio | 0,287 s | 0,407 s | 0,524 s |
| 10 000 × 4 Kio | 39,1 Mio | 1,315 s | 1,276 s | 1,717 s |
| 30 000 × 4 Kio | 117,2 Mio | 3,751 s | 3,303 s | 4,016 s |
| 10 000 × 64 Kio | 625 Mio | 2,531 s | 2,842 s | 3,088 s |

Sur 30 000 fichiers, les milestones début/fin prennent respectivement 4,13 et
3,98 s supplémentaires. Une substep de cinq edits représente donc environ
28,2 s de traitement local Implement contre 16,5 s Slop, hors préparation/fin
de tour et modèle. C'est une estimation fondée sur les mesures, pas un temps
intégralement supprimable sans préserver les garanties historiques.

La capture complète recalcule les hashes et reconstruit l'index privé après
chaque edit dans les deux modes. Implement ajoute l'attribution des changements
et les captures de progression. Git déduplique le stockage, mais ne supprime pas
ce travail de lecture/calcul répété.

Une piste supplémentaire, **non mesurée séparément**, est le `poll(..., 100)`
dans `core/process.cpp` : sorties fermées avant constat de fin du processus,
l'attente peut ajouter jusqu'à 100 ms. Cette hypothèse ne doit pas être présentée
comme l'explication établie des temps du microbenchmark.

Le rapport local détaillé est `build-ai/benchmarks/snapshot-cost/RESULTS.md`.

## Priorités proposées

Aucune optimisation de comportement n'est appliquée par cette investigation.
Les pistes à comparer expérimentalement sont : grouper les annonces avec les
outils lorsqu'aucune décision ne dépend du résultat intermédiaire, éviter les
steps/substeps de vérification redondants, limiter les relectures et résumés
répétés, puis stabiliser le contexte et mesurer le cache. Les captures
incrémentales et la suppression des captures redondantes restent utiles pour
les gros dépôts, mais elles ne résoudraient pas à elles seules le facteur ×3
observé sur ce petit Snake.
