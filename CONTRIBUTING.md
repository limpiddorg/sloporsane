# Conventions de développement

Les textes affichés par l'application (TUI, logs, erreurs et invites du daemon)
sont écrits en anglais.

## Commentaires

- Écrire les commentaires et docstrings en anglais, en expliquant l'intention ou
  une contrainte non évidente plutôt qu'en paraphrasant le code.
- Conserver les invariants de sécurité, de concurrence, de protocole et de compatibilité,
  ainsi que les mentions de licence. Éviter les notes de session et les formulations
  temporelles comme « nouveau » ou « désormais » sans référence de version utile.
- Garder les descriptions concises et à jour ; ne pas commenter chaque opération.

## Intégration par défaut

**Par défaut, les changements de TUI concernent uniquement la version
Python/Textual dans `integrations/tui/`.** L'intégration C++/FTXUI dans
`integrations/ftxui/`, ses tests et son build ne doivent être modifiés que si
l'auteur le demande explicitement pour cette version. Une correction ou une
évolution de Textual n'autorise pas son report automatique sur FTXUI, y compris
pour maintenir la parité entre les deux intégrations.

## C++

- Utiliser C++23, avec le standard requis et sans extensions du compilateur.
- Représenter les structures connues par des types C++ explicites. Réserver les
  valeurs JSON aux données dynamiques (arguments/résultats d'outils, données
  spécifiques aux providers). La désérialisation vers nos types sera traitée
  aux frontières d'entrée.
- Nommer les constantes en `SCREAMING_SNAKE_CASE`.
- Utiliser `.hpp` pour les headers C++ et `.cpp` pour les implémentations.
- Nommer les fichiers de code en minuscules, sans majuscules : par exemple,
  `agent.hpp` et `agent.cpp` pour la classe `Agent`.
- Placer les headers `.hpp` à côté des fichiers `.cpp` correspondants.
- Protéger les headers avec `#ifndef` / `#define` / `#endif`, sans `#pragma once`.
- Nommer la garde `SOS_{NOM_DE_CLASSE_EN_SCREAMING_SNAKE_CASE}_HPP`.
  Par exemple, `Agent` utilise `SOS_AGENT_HPP` et `AgentRuntime` utiliserait
  `SOS_AGENT_RUNTIME_HPP`. Pour un header sans classe, utiliser le nom du fichier :
  `main.hpp` utilise `SOS_MAIN_HPP`.

## Organisation CMake

- Le `CMakeLists.txt` racine ajoute les composants de `src/` avec `add_subdirectory`.
- Les exécutables sont générés dans `bin/` et les bibliothèques dans `lib/`,
  à l'intérieur du dossier de build choisi.
- Chaque composant possède un `CMakeLists.txt` à côté de son dossier `cmake/`.
- Lister explicitement ses fichiers `.cpp` et `.hpp` dans `cmake/sources.cmake`,
  inclus par le `CMakeLists.txt` du composant.
- `agent-runtime` est une bibliothèque statique, accessible via la cible
  `sos::agent-runtime`. Elle expose le chemin d'inclusion permettant d'écrire
  `#include <agent-runtime/agent.hpp>`.
- `sosd` est l'exécutable du daemon. Son transport utilise Asio standalone 1.36.0,
  téléchargé par CMake, sans Boost.

## Compiler et exécuter

Le runtime utilise CPR pour les requêtes HTTP, via un objet `cpr::Session`.
CMake utilise CPR installé (1.14.2 minimum) ou télécharge la version 1.14.2,
configurée pour utiliser libcurl système. Les fichiers de développement de
libcurl doivent être disponibles. La conversion HTML de `webfetch` utilise
libxml2 (`find_package(LibXml2 REQUIRED)`). Le runtime dépend aussi de
`nlohmann/json` (3.12.0 minimum). CMake utilise une version
installée compatible ou télécharge la version 3.12.0 via `FetchContent`.
OpenSSL Crypto est requis pour les aléas cryptographiques et le PKCE OAuth OpenAI.
Le transport Codex WebSocket utilise IXWebSocket 11.4.6 (téléchargé par CMake)
avec OpenSSL SSL, vérification TLS active, et repli HTTP si le handshake échoue.
Le frontmatter des skills utilise `yaml-cpp` 0.8, trouvé sur le système ou
téléchargé par CMake. Le format et la portée du chargement sont décrits dans
[`docs/skills.md`](docs/skills.md).
La facility de snapshots du daemon et ses tests nécessitent `git` dans le `PATH`.
Son API, son stockage privé et ses limites sont décrits dans
[`docs/git-snapshots.md`](docs/git-snapshots.md). Elle pilote `/undo` et `/redo`,
avec un journal des outils d'édition pour les fichiers ignorés ou hors dépôt.
Ce téléchargement nécessite un accès réseau à la première configuration.
L'analyse de bash utilise Tree-sitter 0.25.10 et la grammaire Bash 0.25.0,
téléchargés et compilés statiquement via `src/agent-runtime/cmake/tree_sitter.cmake`.

Le build complet nécessite aussi `uv`. Toute la construction et l'installation
sont orchestrées par CMake ; le Makefile des intégrations a été supprimé.
Le `CMakeLists.txt` racine ajoute `cmake/build-tui/` avec `add_subdirectory()`.
La cible `build-tui` dépend de `sosd`, synchronise les dépendances Python du groupe
`build` avec `uv.lock`, puis utilise PyInstaller pour embarquer la TUI, Python,
`sosd` et les bibliothèques natives nécessaires hors bibliothèques du système.
Elle produit `tui/dist/sos/` et `tui/dist/sos-<OS>-<architecture>.tar.gz`
dans le dossier de build. L'archive est autonome pour une
plateforme compatible ; elle ne requiert ni Python ni uv installés chez son destinataire.
Pour la partager, envoyer l'archive. Le destinataire l'extrait et lance `sos/sos`
depuis un terminal, en conservant le dossier extrait complet.
L'archive doit correspondre au système et à l'architecture du destinataire.
Sur macOS, une distribution téléchargée non signée peut être bloquée par Gatekeeper.
La CI sur `main`, les Releases en brouillon déclenchées par tag et le contrat du
futur backend pour le template `scripts/install.sh.in` sont décrits dans
[docs/distribution.md](docs/distribution.md). L'installateur public télécharge une
archive distante ; il est distinct de l'installation locale CMake.

La cible `install-tui`, incluse par défaut, installe cette distribution dans
`$XDG_DATA_HOME/sos/app/` (ou `~/.local/share/sos/app/`) et crée le lien `sos` dans
`~/.local/bin/`. Les versions installées sont séparées
par le hash de leur archive : un daemon déjà démarré conserve ainsi ses fichiers
lors d'un rebuild. Les anciennes versions ne sont pas supprimées automatiquement.
`SOS_TUI_INSTALL_DIR` et `SOS_TUI_BIN_DIR` permettent de choisir les destinations.
Pour les vérifications de l'agent, les deux doivent rester sous `build-ai/`.
Un échec du packaging ou de l'installation fait échouer le build. Les cibles
natives seules ne déclenchent pas le packaging.

Pour lancer la TUI localement depuis `integrations/tui/` : `uv run tui`, après
un build `build-tui` qui y place aussi une copie de développement de `sosd`.
La commande installée `sos` conserve le dossier depuis lequel elle est invoquée.

L'intégration native `integrations/ftxui/` utilise C++23 et FTXUI 6.1.9, sans
interpréteur Python. `SOS_BUILD_FTXUI` active sa compilation (par défaut `ON`) ;
`SOS_BUILD_TEXTUAL=OFF` désactive entièrement la configuration du packaging Python
et sa dépendance à `uv`. La cible `build-ftxui` produit `bin/sos-ftxui` et `bin/sosd`.
La cible explicite `install-ftxui` installe des versions séparées par hash dans
`$XDG_DATA_HOME/sos/ftxui/`, puis un lien `~/.local/bin/sos-ftxui`.
Pour les vérifications de l'agent, configurer aussi
`-DSOS_FTXUI_BIN_DIR="$PWD/build-ai/ftxui/bin"` et
`-DSOS_FTXUI_INSTALL_DIR="$PWD/build-ai/ftxui/installed"`.
Les tests `FtxuiE2E.*` sont eux-mêmes en C++ et pilotent le vrai exécutable dans
un pseudo-terminal, avec un daemon de protocole factice ou le vrai `sosd` isolé.
Les bibliothèques natives système du daemon restent nécessaires à l'installation.

Toujours configurer et compiler depuis le `CMakeLists.txt` racine.
**L'agent utilise exclusivement `build-ai/` pour ses vérifications.
Le dossier `build/` est réservé à l'humain.**

### Pour l'humain

Depuis la racine du repository :

```sh
cmake -S . -B build
cmake --build build
sos
```

### Pour l'agent

Depuis la racine du repository :

```sh
cmake -S . -B build-ai \
  -DSOS_TUI_BIN_DIR="$PWD/build-ai/tui/bin" \
  -DSOS_TUI_INSTALL_DIR="$PWD/build-ai/tui/installed"
cmake --build build-ai
./build-ai/tui/bin/sos
```

### Serveur de connexions

`sosd` écoute actuellement sur `/tmp/sosd.sock` et journalise chaque connexion.
On peut fournir un autre chemin et un dossier de sessions :
`sosd /chemin/serveur.sock /chemin/sessions`.
La TUI se connecte au daemon existant ou lance son `sosd` embarqué en arrière-plan,
avec l'environnement du premier lanceur (notamment les chemins XDG). Un verrou
par socket sérialise les démarrages concurrents. Un socket abandonné est remplacé,
mais un fichier ordinaire n'est jamais effacé. La TUI signale sa fermeture avec
`integration.close`. Le daemon détecte aussi une fermeture du socket et vérifie
chaque intégration par ping/pong : intervalle de 15 secondes, délai de réponse
de 30 secondes. Une intégration muette est fermée et son exécution annulée.
Après le départ de la dernière intégration et la fin des annulations/sauvegardes,
le daemon retire son socket et quitte. Au démarrage, il attend au plus 30 secondes
sa première connexion. Ces durées sont centralisées dans `src/daemon/config.hpp`.
Ses logs et son PID de lancement sont dans
`$XDG_STATE_HOME/sos/sosd.log`, ou `~/.local/state/sos/sosd.log`. Un daemon existant
n'est pas remplacé automatiquement lors d'une nouvelle compilation ; fermer ses
intégrations suffit pour que le prochain lancement utilise le binaire installé.
Pour se connecter depuis un autre terminal :

```sh
nc -U /tmp/sosd.sock
```

Ctrl+C arrête le daemon et retire son socket. Un chemin déjà occupé est refusé,
sans suppression automatique. Les connexions échangent du JSONL ; une intégration
est créée par connexion, sans session avant le premier message ou une demande de
rattachement. Voir [le protocole](docs/daemon-protocol.md).
Aucune clé API n'est nécessaire pour écouter/lister ou ouvrir `/connect`.
Cette commande connecte OpenRouter par clé API, ou OpenAI par clé API, OAuth
navigateur (callback local 1455) ou code appareil. Le daemon conserve les credentials
dans `$XDG_DATA_HOME/sos/auth.json`, par défaut `~/.local/share/sos/auth.json`,
en JSON non chiffré avec permissions `0600`, remplacé atomiquement. Les clés saisies
sont masquées et ne passent pas par les conversations. L'authentification utilise
exclusivement `auth.json` ; aucune clé n'est récupérée dans l'environnement.
La sélection provider/modèle/réflexion et le maximum de contexte sont propres à
chaque conversation. Le dernier choix explicite est aussi conservé dans
`$XDG_CONFIG_HOME/sos/config.json` comme défaut des nouvelles conversations.
`/resume` restaure les réglages enregistrés sans modifier ce défaut ; `Ctrl+T`
sauvegarde le niveau pour la conversation et le défaut global, y compris après
redémarrage. Les métadonnées des anciens tours restent historiques.
Le niveau `none` est affiché `No reasoning`. OpenRouter ne le propose que dans
`reasoning.supported_efforts` explicite ; une liste nulle ne prouve pas sa prise
en charge. Codex lit `supported_reasoning_levels` dans son catalogue authentifié,
y compris pour les alias rapides. Pour OpenAI API, les familles connues GPT-5.1,
GPT-5.2 et GPT-5.4 hors variantes Codex/Pro proposent aussi `none`. Le défaut de
réflexion reste résolu sans réseau ; la découverte des niveaux se fait sur worker.
Une requête avec `none` n'inclut ni résumé de réflexion ni demande de contenu chiffré.
Un changement via `/connect` détache la session au repos et démarre une nouvelle
conversation ; l'ancienne reste accessible via `/resume`, avec son provider enregistré.

Les chemins globaux C++ sont centralisés dans la struct `PathResolution`
(`agent-runtime/common/path_resolution.hpp/.cpp`). Les overrides XDG relatifs ou
vides sont ignorés. `ProviderAuth` sépare les credentials du transport ;
`OpenAIProviderAuth` renouvelle les tokens OAuth sous verrou partagé par les agents
et la génération de titres. Le transport Codex lit un flux HTTP SSE jusqu'à
`response.completed` puis rend un objet Responses complet au runtime.
Le décodage SSE (`common/sse_decoder.hpp/.cpp`) ne connaît pas le protocole du
modèle. `ResponsesStreamAssembler` consomme les événements JSON, conserve les
éléments `response.output_item.done` par `output_index` et les fusionne avec
l'output final sans doublons. Les métadonnées finales et les données de replay
sont conservées, y compris quand Codex termine avec `output: []`. Un flux tronqué,
en erreur ou avec des éléments manquants est refusé avant de rejoindre le runtime.

`Integration` orchestre les commandes et les sessions ; `ProviderConnection`
possède le parcours de connexion local (démarrage, polling, annulation, drainage).
`ProviderConnections` encapsule les credentials partagés et expose `hasAuth`,
`requireAuth`, `saveApiKey` et la création des providers. Les détails OAuth restent
dans le runtime. Les attentes de workers passent par `transport/await_future.hpp`.
Voir [le plan de refactoring](docs/auth-refactor.md) pour les responsabilités et interfaces.
`ModelCatalog` charge et filtre les catalogues pour la sélection automatique de
`/connect`, qui n'affiche aucun champ modèle. Cette recherche se fait après authentification,
sur un worker annulable, avant de modifier la sélection ou détacher la session.
`/model` réutilise ces catalogues pour lister les modèles des providers connectés,
avec un filtre textuel local dans la TUI et une sélection explicite provider/modèle.
Pour Codex, SOS complète le catalogue authentifié avec les modèles visibles et
compatibles API du catalogue public officiel `openai/codex` (requête anonyme).
Les entrées authentifiées, y compris masquées, priment à identifiant identique ;
si le catalogue public est inaccessible, seul le catalogue authentifié est utilisé.
Cette liste supplémentaire ne garantit pas que le compte accepte une génération :
une erreur du serveur reste renvoyée au moment de l'appel.
Pour Codex, les capacités `service_tiers: [{"id":"priority"}]` ou l'ancien
`additional_speed_tiers: ["fast"]` produisent une entrée synthétique `…-fast`,
juste après le modèle d'origine, sans masquer un véritable identifiant du catalogue.
Elle hérite du contexte et de la réflexion ; le provider envoie le modèle d'origine
avec `service_tier: "priority"`. Ce choix peut consommer davantage de crédits.
Les alias indisponibles sont refusés sans retour silencieux au mode normal.
Les catalogues OpenAI par clé API et OpenRouter ne sont pas enrichis ainsi.
Les commandes `model.list`, `model.select` et `model.cancel` passent par
`ProviderConnection`. La sélection conserve la conversation et la réflexion ;
le prochain message utilisateur applique le changement de provider/modèle au contexte de
l'agent. Toutes les générations du tour déjà lancé gardent leurs réglages
et leurs métadonnées. `/new`, une reprise de session et la sélection finale de
`/connect` annulent et sauvegardent le principal avant de basculer.
`/model` demande ensuite un `context_limit` entier, minimum 32 768, plafonné au
maximum connu du catalogue. Cette valeur est enregistrée avec la conversation et
le dernier choix global dans `config.json`, sans table par modèle. La liste transmet `max_context`
et le daemon valide la saisie contre son propre catalogue, pas une borne du client.
Le budget choisi devient le plafond d'entrée de compaction (avec réserve habituelle),
la base de rétention et le total affiché. `/new` hérite du dernier choix global,
`/resume` restaure celui de la conversation et changer la réflexion le conserve.
`ApiKeyValidator` vérifie la clé candidate avant sa sauvegarde, sans utiliser
l'ancienne clé et sans génération : `/api/v1/key` pour OpenRouter, `/v1/models`
pour OpenAI. Le GET JSON authentifié, borné et annulable est partagé avec
`ModelCatalog` dans `providers/provider_http.hpp/.cpp`.

Les providers personnalisés Textual sont des instances nommées OpenAI-compatible
Chat Completions : `provider.create` découvre `/models` avant de sauvegarder la
configuration globale (`custom_providers` dans `config.json`) et la clé facultative
séparément dans `auth.json`. Les IDs `custom-…` sont générés et stables ; les noms
servent à l'affichage. `ChatCompletionsProvider` adapte les items Responses internes
vers les messages Chat et normalise les réponses pour conserver les transformers,
le journal et la boucle d'outils existants, sans envoyer de requête Responses au
serveur custom. Les capacités restent inconnues, sans filtre de familles OpenAI ni
paramètre de raisonnement imposé. `/model` découvre leurs modèles et permet de
fixer le budget de contexte. `provider.delete` préserve les sessions et refuse le
provider courant tant qu'un autre n'a pas été sélectionné. Voir
[le protocole](docs/daemon-protocol.md#custom-providers).

Les générations OpenAI (API et Codex) et OpenRouter utilisent aussi ce transport
partagé : connexion limitée à 15 secondes, requête entière à 10 minutes, inactivité
de réception à 10 minutes. Pour Codex (SSE), l'inactivité est surveillée dès le départ
et chaque fragment reçu, y compris les heartbeats, remet ce délai à zéro. Sans
streaming (OpenAI API et OpenRouter), le provider peut ne rien envoyer pendant son
raisonnement : le délai d'inactivité commence donc aux premiers octets du corps,
avec le plafond total de 10 minutes maintenu pendant toute l'attente. Les erreurs
distinguent l'inactivité, le timeout de requête et les catégories de transport,
sans exposer les diagnostics bruts ou les credentials. L'annulation reste prioritaire.
Les délais des catalogues et de l'authentification ne sont pas modifiés.

Les sessions sont sauvegardées dans `$XDG_STATE_HOME/sos/sessions`, ou
`~/.local/state/sos/sessions`. Chaque dossier possède `session.sqlite`, source de
vérité pour les messages typés, données provider et métadonnées de reprise, et
`info.json`, cache lisible de ces métadonnées. Le registry garde seulement des
références faibles aux sessions actives. Une session ne peut être attachée qu'à
une intégration à la fois dans ce daemon.

SQLite 3.50.4 est téléchargé depuis l'amalgamation officielle avec SHA-256 épinglé
et lié statiquement au daemon (`src/daemon/cmake/sqlite.cmake`). Son code est dans
le domaine public : https://www.sqlite.org/copyright.html. Aucun outil `sqlite3`
ni bibliothèque SQLite système n'est requis chez l'utilisateur.

`SessionJournal` stocke chaque message individuellement avec un résumé léger.
Le mode WAL et `synchronous=FULL` donnent des commits transactionnels synchronisés,
sous réserve des garanties du stockage. Messages et métadonnées de reprise sont
validés ensemble. Une compaction ne tronque plus de journal en cas d'échec.
`info.json` est remplacé atomiquement à la création, aux frontières de tour,
aux changements de réglages et aux sauvegardes explicites ; il n'est pas réécrit
pour chaque résultat d'outil. Son échec de publication n'annule pas un commit.
Il peut être périmé après interruption et est régénéré lors de la reprise.

Seule une base neuve (version 0) ou le schéma courant 6 est accepté. Les anciens
schémas et bases corrompues sont refusés sans réécriture ni remplacement.
Les fichiers JSONL et anciennes métadonnées ne sont jamais importés ou modifiés.

`SessionHistoryService` expose pages, Activities et détails. La lecture d'un corps
ne charge pas un lot entier. `SessionRecoveryIndex` maintient
les appels interrompus, refus et approbations dans la transaction des messages.
Ces lectures ne parcourent ni les résumés ni les corps.
`SessionHistoryIndex` est la projection incrémentale d'affichage
mise à jour dans la même transaction. Le service lit les pages par ancrages et
plages indexées, sans reconstruire les anciens regroupements ni charger tous les
résumés. Une Activity repliée ne lit que son agrégat. Aucune reconstruction
d'anciens agrégats ni lecture de corps pour compléter les noms de skills.
Les gros messages individuels et les Activities ouvertes (dont la dernière
préchargée) sont encore lus en entier. Voir [le suivi du stockage](docs/history-index-plan.md)
pour les limites et la pagination restant à implémenter.

Pour sauvegarder une session, fermer proprement le daemon puis copier le dossier,
ou utiliser l'API de sauvegarde SQLite. Ne pas copier seulement `session.sqlite`
pendant une écriture et ne pas supprimer les fichiers `-wal` et `-shm` d'une base
ouverte. Un seul daemon par dossier de sessions reste requis.

Le contexte de l'agent est préparé sur un worker au premier envoi ou `/compact`.
La dernière compaction permet de lire directement les instructions initiales,
le checkpoint, ses messages conservés et les messages suivants. Sans checkpoint,
la préparation lit encore tout le contexte, mais pas la simple consultation.
L'événement `execution.preparing` affiche `Preparing conversation…` dans la TUI.
Annulation et déconnexion empêchent l'installation d'une préparation incomplète ;
aucun nouveau message utilisateur n'est sauvegardé avant son acceptation.
Les appels d'outils interrompus restent réparés sans exécution automatique.

Les événements du runtime sont transférés par `asio::post` vers la file propre à
l'intégration. Celle-ci sauvegarde puis transmet chaque événement, dans l'ordre,
sans que le worker attende l'écriture disque, sauf pour le checkpoint de compaction
automatique dont il attend la sauvegarde avant d'activer le nouveau contexte et les
frontières de construction Implementation. Ces dernières sont capturées et enregistrées
sur une connexion SQLite propre au worker avant l'outil suivant, afin de conserver
l'appartenance des changements aux substeps. La fermeture demande l'annulation,
traite les événements restants puis libère la session. L'annulation est coopérative
et interrompt les requêtes provider via le callback de progression libcurl, ainsi
que les outils interruptibles. `execution.cancel` arrête la run sans fermer la
session ; les appels interrompus et non exécutés sont enregistrés pour la reprise.
Les écritures locales sont synchrones sur le thread Asio ; les transactions SQLite
utilisent `synchronous=FULL` pour demander la synchronisation des commits.

### Compaction

`/compact` envoie `session.compact` au daemon, uniquement au repos et dans une
session existante. `CompactionService` reçoit une `Conversation`, génère un résumé
sans outils avec le provider/modèle/réflexion sélectionnés et renvoie une nouvelle
conversation active. L'ancien historique reste intégralement dans `Session` et
`session.sqlite` ; le service d'historique alimente la TUI, pas le contexte actif.
Le résumé est un message assistant portant un checkpoint `compaction` avec les IDs
des messages récents conservés et le mode de session. La requête utilisateur
synthétique qui précède ce résumé n'existe que dans le contexte actif.

La sélection conserve des tours complets, sans séparer un appel d'outil de ses
résultats. Le budget récent est estimé à un token pour quatre octets de JSON, avec
25 % de la limite de contexte du modèle plafonnés à 30 000 tokens, sans plancher, ou
8 000 si la limite est inconnue. Au moins le plus ancien tour est résumé. Les
résultats d'outils de la transcription à résumer sont limités à 2 000 octets
UTF-8 (sans couper un caractère) et les pièces jointes remplacées par leur
type. Le résumé précédent est intégré aux compactions suivantes.

Toutes les synthèses utilisent un mécanisme borné de blocs puis de fusions
récursives. La compaction ordinaire demande au maximum 1 % de la fenêtre d'entrée
effective, avec un plancher de budget de 2 000 tokens, moins si possible. Elle
accepte en interne jusqu'à 5 %, avec le même plancher, sans annoncer cette tolérance
au modèle. Les deux budgets restent plafonnés par la limite de sortie du modèle ;
le résumé n'a aucune longueur minimale. Chaque synthèse permet trois appels : initial, puis
deux corrections de taille. Les résumés de reviews gardent leur budget distinct
et deux corrections suivies d'une synthèse d'urgence. Le compteur TUI reste masqué
au premier appel de chaque bloc/fusion ; seules les corrections affichent `2 / 3`
ou `3 / 3`. Conserver le candidat le plus court puis, si nécessaire, le tronquer
à une frontière UTF-8 avec un avertissement. Les erreurs provider, réponses vides
et annulations ne sont jamais remplacées silencieusement par une troncature.
Ces budgets sont des heuristiques d'octets, pas une tokenisation exacte.
L'historique original n'est jamais supprimé.

Avant chaque appel modèle, l'agent soumet un `CompactionContext` (conversation active,
`ModelInfo`, outils et frontière du dernier usage mesuré) au hook de préparation
installé par la session. `CompactionService::needCompaction` est un calcul sans
effet de bord ; il accepte aussi des messages en attente sans les ajouter. L'estimation
utilise l'usage réel entrée + sortie du dernier appel, augmenté des nouveaux messages
à raison d'un token pour quatre octets UTF-8 de JSON, arrondis au supérieur. Sans
frontière mesurée, elle estime tout le contexte et les définitions d'outils. Les
métadonnées d'affichage et de checkpoint sont exclues. Ce n'est pas un tokenizer
et cela ne garantit pas l'absence de dépassement, notamment avec des médias.

Le seuil est le budget maximal d'entrée, sinon le contexte maximal, moins une
réserve de `max(10 % du budget, 16 384)`. Avec une fenêtre globale, la réserve couvre
aussi la limite de sortie connue. Une limite absente/nulle ou un budget trop petit
pour cette réserve ne déclenche pas l'automatisme. OpenRouter remonte le contexte
et `top_provider.max_completion_tokens`. Avec l'authentification Codex, OpenAI remonte
`context_window` depuis le catalogue authentifié, ou `max_context_window` en repli.
L'affichage utilise en priorité `max_context_window`, sans changer le budget
opérationnel utilisé pour la compaction ; à défaut, il affiche la fenêtre courante.
Le catalogue est mis en cache par instance de provider et compte ; un échec de
chargement se propage, sans devenir une capacité image inconnue. Les erreurs réseau
temporaires sont retentées pendant le précontrôle image et la préparation du tour ;
une annulation se propage. Seul un catalogue récupéré sans déclaration donne une
capacité inconnue. Les seuils spécifiques de
Codex ne remplacent pas notre politique de réserve. L'accès OpenAI par clé API
reste sans limite connue, sans inventer de capacité de modèle.

Si nécessaire, la session résume avec le provider du tour courant, sans outils,
et conserve impérativement le dernier tour complet, même au-delà du budget de
rétention. Les ajouts sont déjà enregistrés localement, mais pas encore envoyés au
modèle. Après sauvegarde du checkpoint sur le thread Asio, le worker active le
nouveau contexte et reprend le flux sans message « Continue » synthétique. Il ne
compacte pas récursivement le résumeur. Si aucun historique antérieur n'est
résumable, si la génération échoue ou si le nouveau contexte dépasse encore le
seuil estimé, le tour échoue sans activer le résumé. Pas de récupération automatique
des erreurs de contexte renvoyées par le provider dans cette version.

La sauvegarde du checkpoint précède son activation. Échec et annulation ne
suppriment aucun message et n'activent pas de résumé partiel. L'annulation et la
fermeture de connexion interrompent le worker provider. `/resume` reconstruit le
contexte depuis le dernier checkpoint, sans rappeler le modèle. Les IDs des
messages conservés restent stables ; leurs données d'affichage restent intactes,
mais leur replay provider devient portable pour ne pas réutiliser du reasoning
opaque dépendant du contexte supprimé. `provider_context_start` désigne une
frontière dans l'historique complet, traduite vers le contexte actif au chargement.
Le compteur de contexte est remis à inconnu après compaction jusqu'à la prochaine
génération normale, sans confondre consommation du résumeur et contexte de l'agent.

### Permissions

`/permissions` choisit une politique indépendante de Sane/Slop : `ask`, `auto` ou
`danger`. Le mode global est enregistré sous `permission_mode` dans `config.json`
et appliqué aux contrôles d'outils pendant le tour courant ; `/new` et `/resume`
ne le remplacent pas. Passer d'Ask à Auto ou Danger réévalue une demande en attente.
Le sélecteur utilise `DialogScreen` et décrit brièvement le mode surligné. Les lignes
partagées `dialog_row` affichent `Current` (permissions et modèles) et `Connected`
(providers et méthodes d'authentification) à droite, en corail (`BOLD_MD`).
Danger supprime les confirmations, jamais les interdictions Sane ni les limites
du système d'exploitation. Son avertissement mentionne explicitement les pertes
de fichiers, et recommande AI Verified en cas de doute.

Auto intervient aux endroits qui demanderaient une permission humaine. Le composant
`PermissionVerifier` appelle `https://openrouter.ai/api/alpha/decisions` avec
`typesafe/jev-1.13`, une question `choice` et les réponses `allow` / `deny`. Le prompt
autorise le développement courant dans le repo (sources, builds, tests, dépendances,
scripts locaux et nettoyage des artefacts) sans zèle sur le
contenu des sources, et cible les risques concrets : DB de
production, suppression de fichiers personnels, dommages système, exfiltration.
Le seul fait de lancer une commande depuis le repo ne rend pas ses effets externes sûrs.
La confiance est toujours celle d'exécuter : `allow` conserve son score,
`deny` à 98 % devient 2 %. Ce score normalisé est comparé au seuil entier global
`permission_confidence` (0–100, défaut 80) dans `config.json`. Une confiance de
refus faible peut donc passer un seuil bas. À 0, aucun appel Jev : même politique
que Danger, sans contourner Sane. À 100, il faut un score de 100 %, sans garantie
de sûreté calibrée. Les erreurs et scores invalides bloquent l'action lorsque les
contrôles sont activés. Les scores sont normalisés à douze décimales pour éviter
les erreurs de soustraction aux bornes des seuils entiers.
Une erreur d'outil explique au modèle que l'action n'a pas été exécutée, qu'il
ne doit pas réessayer ou contourner le refus sans validation manuelle. Il peut
proposer d'ouvrir Activity, de déplier l'action sous `Refused tools`, de cliquer sur `Approve`, puis d'envoyer un
nouveau message ; sinon il continue sans cette action. Cette consigne est transmise au modèle, pas imposée par un
arrêt de la boucle agent. Contrairement au `no` humain, ce refus n'annule pas le tour.
Les décisions Auto émettent directement `permission.resolved` avec `ai_accepted`
ou `ai_refused`, puis sont conservées dans `metadata.permissions` du résultat.
Le champ optionnel `confidence` (0–1) contient toujours la confiance d'exécuter,
y compris dans les résumés légers SQLite. Un score manquant n'est pas un zéro.
La TUI affiche `-> Accepted by AI (Jev)` ou `-> Refused by AI (Jev)`, en direct et
après reprise. Ces valeurs ne sont jamais acceptées comme réponses humaines.
La requête est annulable et limitée à 30 secondes ; le verrou des permissions
n'est pas tenu pendant le réseau. Sane est revérifié après l'appel.

Le contexte envoyé conserve les arguments complets de Bash. Pour `write`, `edit`
et `apply_patch`, une liste blanche ne conserve que les chemins résolus et les
opérations (création/modification et existence pour write/edit ; ajout, modification,
suppression et déplacement pour les patches). Les contenus, arguments bruts et diffs
sont exclus, y compris lors des contrôles hors workspace. Jev évalue les cibles,
pas le comportement du code écrit. Les diffs locaux restent inchangés.
Le contexte inclut le workspace et au plus 2 000 octets UTF-8 du dernier message utilisateur. Aucun
historique complet ni sortie d'outil n'est ajouté. Un état dépassant 48 000 octets
est refusé sans requête distante, plutôt que tronquer l'action.
Les commandes, chemins et extraits utilisateur peuvent contenir des données privées ; cet envoi
est documenté dans le README. La clé dédiée est saisie masquée,
validée via `ApiKeyValidator`, puis stockée sous `permission_verifier` dans
`auth.json` par `AuthStore`, sans remplacer les connexions de conversation.
La disponibilité de la clé est transmise via `info.has_permission_key`, jamais
la clé elle-même. Entrée sur Auto utilise directement la clé enregistrée ; sans
clé, un champ masqué est proposé avec Échap pour revenir sans rien changer.
`Change API key` ouvre ce même formulaire pour remplacer la clé ; le bouton
`Use saved key` a été retiré. Un échec de sélection avec la clé enregistrée affiche
le formulaire et l'erreur sans changer le mode courant.
Quand les réglages Jev sont visibles à l'ouverture, `Save` reçoit le focus :
Entrée sauvegarde, gauche/droite choisit entre `Save` et `Change API key`, Tab
permet d'éditer le seuil. Les flèches dans le champ numérique ajustent sa valeur.
En revenant sur la ligne Jev, la liste garde le focus et `Save` redevient
l'action sélectionnée, visible en fond bleu et texte clair ; Entrée sur la
ligne permet de rejoindre les actions sans sauvegarder immédiatement.
Le seuil et ses boutons apparaissent directement sous les trois modes uniquement
quand Jev est `Current` et que sa ligne est surlignée. Sur Ask ou Danger, ils sont masqués, sans
écran de réglage séparé : sous « How confident… », un champ numérique court sur
une ligne, sans bordure, ajustable par les flèches. Seules les explications 0/100 %
figurent à droite ; pas de presets ni de paragraphe supplémentaire.
La validation d'une nouvelle clé ou l'utilisation automatique de la clé enregistrée
active Jev puis revient à cette liste,
sans fermer la popup. `Save` (ou Entrée dans le champ) ferme la popup après confirmation
de l'enregistrement du seuil ; un échec la garde ouverte avec le brouillon et l'erreur.
`Change API key` permet de remplacer la clé ; il n'y a plus de bouton `Skip Jev`.
Le seuil 0 reste disponible ; son explication à droite prend la couleur d'alerte à 0.
La présence d'`info.permission_confidence` est requise par
ce parcours pour qu'un ancien daemon ne puisse pas ignorer silencieusement le seuil.

Les refus IA sont aussi affichés sous `Refused tools` dans une section séparée d'Activity,
au-dessus d'un séparateur Muted aligné sur les flèches. Un carré Slop supplémentaire
La section sous ce séparateur porte le titre `History` en corail, aligné avec
`Refused tools` ; ces titres et le séparateur restent masqués sans refus ou au repli.
reste sur Activity, même après validation et avant le chargement des détails historiques
(type d'activité `refused`). La zone inférieure reste le journal immuable.
La ligne affiche `tool_name • Confidence X%` en Slop, ou `Confidence unavailable`
pour les anciens refus et les erreurs de vérification. Le bouton `Approve` devient
`Revoke approval` après validation ; ` -> Manually approved` s'ajoute au titre,
entièrement en Algae, sans modifier le score historique. Révoquer retire le suffixe
et remet le titre en Slop. Le bouton n'est
créé qu'au dépliage et est retiré au repli ; les champs de contexte/erreur vides sont masqués.
Le bouton émet `permission.manual` : le
daemon retrouve les arguments dans le résultat refusé, sans faire confiance à des
arguments fournis par la TUI. La bascule ajoute un événement système d'audit
`manual_approval` au journal ; elle ne modifie pas l'agent en cours et n'exécute rien.
Au prochain message, un rapport chronologique décrit les bascules depuis le dernier
message utilisateur. Les accords sont chargés dans le gestionnaire à cette frontière,
valables sans limite de tentatives pour outil, arguments, répertoire et workspace
identiques, en Ask comme en Auto, sans contourner Sane. Une révocation retire cet
accord au prochain tour. Ils sont propres à la session, restaurés après reprise,
et indépendants des accords `always` propres à la connexion. Les tentatives autorisées
sont marquées `manually_approved` dans leur propre résultat, jamais dans le refus initial.
Les résumés SQLite conservent les identifiants des refus, les événements d'audit
et les compteurs/durées des appels au modèle pour le débit moyen par Activity ;
les arguments restent dans le journal complet. Les anciens refus sans cible enregistrée
sont visibles mais leur bouton est désactivé.

Le rapport système fournit aussi des IDs de rejeu propres à la session, et les
accords actifs sont rappelés à chaque tour, y compris après compaction/reprise.
`rerun_approved_action` n'accepte qu'un ID, puis retrouve outil, arguments exacts,
répertoire et workspace enregistrés. Aucun argument ne peut être surchargé.
Le resolver du worker utilise sa propre connexion SQLite, vérifie que l'ID était
disponible au début du tour et revérifie les révocations aux frontières de permission.
Le rejeu contourne Jev, jamais Sane ni l'annulation. Il reste réutilisable jusqu'à
révocation et n'est pas exposé aux branches BTW. Le journal provider conserve
l'appel de rejeu ; `metadata.approved_action` permet à Activity de montrer le
véritable outil et ses arguments. Aucune migration SQLite n'est nécessaire.

Les paragraphes suivants décrivent la politique de base **Ask before**.

Le daemon possède un `PermissionManager` par connexion, avec un mode typé
Sane/Slop, le workspace initial autorisé et les accords temporaires. Le mode affiché
change immédiatement, mais les permissions du tour actif restent inchangées ;
le nouveau mode s'applique au prochain message utilisateur.
Le mode sauvegardé dans SQLite et reflété dans `info.json` correspond au dernier lancement
d'un message utilisateur et est restauré lors d'un rattachement ; le champ `mode`
est requis dans les métadonnées courantes. Le prompt initial explique les deux modes
et annonce celui du premier message. Un envoi dans un mode différent ajoute une
annonce système juste avant le message utilisateur puis actualise le mode de session.
Aucune annonce n'est ajoutée au switch ou entre les appels modèle d'un tour en cours ;
le préfixe existant reste inchangé. Les anciennes sessions gardent leur prompt original.
Les accords `always` restent propres à
la connexion et ne sont pas sauvegardés avec le mode. La TUI
configure ce workspace au lancement ; reprendre une session ne change pas cette
racine d'autorisation. En Sane, `write`, `edit`, `apply_patch` et les commandes bash
modificatrices ou inconnues sont refusés. En Slop, ces opérations nécessitent une
confirmation. Les accès externes détectés nécessitent une confirmation dans les
deux modes. `webfetch` et les lectures/recherches internes restent autorisés.

Les types de requête, réponse et trace de permission vivent dans
`agent-runtime/tools/permission.hpp`. Le runtime identifie l'appel d'outil et
exécute un contrôle préalable des familles interdites. Le gestionnaire du daemon
émet des `PermissionEvent` vers la file Asio et attend une réponse annulable sur
le worker. La TUI présente une carte au niveau du chat, puis garde la décision
sur l'activité. Un refus utilisateur annule le tour entier ; un refus de politique
Sane est un résultat d'erreur d'outil. Les décisions sont sauvegardées dans les
métadonnées du résultat, jamais les autorisations réutilisables.

`Always yes` mémorise, pour cette connexion seulement, le dossier et ses
descendants, toute la famille d'édition (`write`, `edit`, `apply_patch`), ou les
portées de commandes affichées. L'accès au workspace de lancement et à ses
descendants est autorisé d'emblée ; ce droit d'accès ne dispense pas de l'accord
d'édition ou de commande bash.
La restriction Sane reste prioritaire. Les outils d'édition libèrent leur verrou
pendant l'attente humaine, puis vérifient à nouveau les fichiers sous verrou.

Bash est analysé syntaxiquement avant exécution : liste prudente de commandes
de lecture, détection de redirections, substitutions, commandes inconnues et
chemins externes explicites. Les programmes inconnus conservent une approbation
exacte plutôt qu'une autorisation de tout leur interpréteur. Comme dans OpenCode,
cette politique n'est pas une sandbox : les chemins calculés et les effets internes
des scripts ou configurations de programmes ne sont pas entièrement déductibles.

### Ripgrep et outils filesystem

`core::ensureRipgrep()` cherche `rg` dans le `PATH`, puis dans
`$XDG_DATA_HOME/sos/bin` (ou `~/.local/share/sos/bin`). S'il manque, il télécharge
la version 15.1.0 depuis les releases officielles de ripgrep et conserve sa propre
copie, sans installation système. Cette première version couvre macOS et Linux
en arm64/x86_64 et utilise `/usr/bin/tar` pour l'extraction.

`glob` et `grep` utilisent ce binaire sans passer par un shell et limitent les
résultats à 100. Les appels de processus sont limités à 30 secondes et 8 MiB de
sortie par flux. Les tests de recherche nécessitent `rg` dans le `PATH` ; ils ne
déclenchent pas de téléchargement réseau.

Les contrats des outils suivent ceux du clone OpenCode, sans intégration LSP ni
formatage automatique. `edit` applique une correspondance exacte puis un fallback
par lignes ignorant les espaces en bordure ; les autres heuristiques OpenCode
ne sont pas reproduites. Les aperçus de permission et les résultats de `write`,
`edit` et `apply_patch` partagent un diff unifié calculé par lignes, avec trois
lignes de contexte et des blocs séparés pour les changements éloignés. Le calcul
Myers bidirectionnel conserve deux fronts de recherche, sans table LCS quadratique
ni dépendance à un exécutable externe. Les fins de ligne, le BOM et l'absence de
saut de ligne final restent représentés dans le diff ; le calcul de cet aperçu
ne modifie pas la logique d'application des changements.
`apply_patch` prépare tous les changements avant permission/écriture, mais n'est
pas une transaction multifichier en cas d'erreur d'I/O pendant l'application.
Les destinations d'ajout/déplacement existantes et les chemins répétés dans un
même patch sont refusés.

### Outils communs

`tools/common/bash` expose les arguments OpenCode `command`, `workdir`, `timeout`.
Il lance `/bin/bash -c`, avec stdin fermé et un nouveau shell par appel. Le timeout
par défaut est 120 000 ms. stdout et stderr sont fusionnés, le code de sortie est
renvoyé, et timeout/annulation terminent le groupe de processus. Les sorties au-delà
de 2 000 lignes ou 50 Kio sont conservées dans `<workspace>/.sos/tool-output/` ;
l'outil renvoie leur fin avec le chemin de la sortie complète.
L'analyse Tree-sitter et la politique de permissions sont appliquées avant
l'exécution. Pas de sélection PowerShell/cmd ni d'aperçu de sortie en direct.

`tools/common/webfetch` expose `url`, `format` (`markdown` par défaut, `text`, `html`)
et `timeout` en secondes (30 par défaut, plafonné à 120). Il utilise CPR, suit au
plus dix redirections HTTP(S) dans le même budget de temps et limite le contenu
reçu/décompressé à 5 Mio. Les images JPEG/PNG/GIF/WebP deviennent des pièces jointes.
Les sorties texte longues sont sauvegardées dans le même dossier, avec leur début
renvoyé au modèle. La conversion HTML utilise libxml2 et couvre les structures
courantes ; elle n'est pas une reproduction complète de Turndown d'OpenCode.
Les URL HTTP restent HTTP, comme dans le code OpenCode consulté ; aucun navigateur,
JavaScript ou mécanisme de contournement anti-bot n'est utilisé.

## Tests

Utiliser GoogleTest pour les tests C++. Les tests sont enregistrés dans CTest
et compilés avec le projet quand `BUILD_TESTING` est activé (par défaut).
CMake utilise GoogleTest installé (1.17.0 minimum) ou télécharge la version
1.17.0 via `FetchContent`.

Après compilation, l'agent lance :

```sh
ctest --test-dir build-ai --output-on-failure
```

L'humain utilise la même commande avec `--test-dir build`.

### Tests Python/Textual

Les tests Textual sont une suite `unittest` distincte de CTest. Pour préparer
ses dépendances sans packaging ni installation utilisateur :

```sh
UV_PROJECT_ENVIRONMENT="$PWD/build-ai/tui-venv" \
  uv sync --project integrations/tui --frozen --no-default-groups
```

Le lanceur rapide utilise au plus quatre processus par défaut. Il répartit les
cas individuels, y compris ceux des gros fichiers, et isole les dossiers XDG de
chaque lot. Le debug asyncio est désactivé, mais les assertions, interactions
Textual, animations et nettoyages `unittest` restent actifs.

```sh
# Suite complète. Compiler build-ai/bin/sosd pour inclure les vrais tests daemon.
build-ai/tui-venv/bin/python integrations/tui/run_tests.py

# Un fichier, une classe ou un cas précis ; -j 1 conserve l'ordre séquentiel.
build-ai/tui-venv/bin/python integrations/tui/run_tests.py -j 1 \
  test_implementation_workspace.ImplementationLogicTest

# Diagnostic asyncio complet, également possible avec unittest directement.
build-ai/tui-venv/bin/python integrations/tui/run_tests.py -j 1 --asyncio-debug test_chat

# Réutiliser les durées d'une exécution précédente pour mieux répartir le travail.
build-ai/tui-venv/bin/python integrations/tui/run_tests.py \
  --timings-from build-ai/test-measurements/tui.json
```

`--jobs N` règle le parallélisme ; `--pattern 'test_*e2e.py'` filtre les fichiers
et `--list` affiche les cas sélectionnés. Le rapport par défaut est
`build-ai/test-measurements/tui.json`, avec durées murales/CPU et statuts individuels,
accompagné d'un `.log` contenant les détails. `--report chemin.json` permet de
conserver plusieurs mesures. Les échecs, erreurs et succès inattendus produisent
un code de sortie non nul ; les tests ignorés sont signalés, pas déclarés réussis.

Pour les nouvelles vérifications, garder les calculs purs dans des `TestCase`
sans montage de l'application. Réserver les fixtures complètes aux interactions
qui en ont besoin. Tester les expirations en vérifiant le délai enregistré et
en déclenchant son callback plutôt qu'en attendant plusieurs secondes réelles.
Les fixtures utilisant des sockets Unix doivent choisir un chemin temporaire
court, indépendant de la profondeur du checkout. Ne pas désactiver globalement
les pauses ou animations Textual pour gagner du temps.

Les tests natifs peuvent aussi être parallélisés explicitement, par exemple :

```sh
ctest --test-dir build-ai --parallel 4 --output-on-failure
```
