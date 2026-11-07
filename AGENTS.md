# Working agreement

## Avant de modifier

- Lire [CONTRIBUTING.md](CONTRIBUTING.md) pour les conventions de code et de build,
  et le [README](README.md) pour le produit et l'architecture.
- Lire les sections pertinentes des [contrats produit](docs/product-contracts.md)
  avant de modifier la TUI, Implement, les workspaces, les skills ou les MCP.
  Ces contrats restent des exigences, pas des suggestions.
- Utiliser exclusivement `build-ai/` pour les builds, tests et installations de
  vérification. `build/` est réservé à l'humain.
- Par défaut, une demande TUI vise Python/Textual (`integrations/tui/`). Ne modifier
  FTXUI (`integrations/ftxui/`), ses tests ou son build que sur demande explicite ;
  ne pas y reporter automatiquement les changements Textual.

## Collaboration

L'auteur pilote ; l'agent réalise l'unité de travail demandée, explique le résultat
et lui rend la main. Le mode Slop du produit ne change pas cet accord.

- Une discussion, une question ou une demande d'avis n'autorise pas à implémenter.
- Rester dans le périmètre demandé : pas de refactoring, abstraction, fonctionnalité
  ou tâche suivante ajouté spontanément. Préserver les modifications sans rapport.
- Résoudre les détails locaux évidents. Demander avant un choix structurant ou une
  découverte qui change le comportement attendu, l'approche ou le périmètre.
- Inspecter le code concerné avant de le modifier. Garder l'orchestration courte,
  nommer les étapes par leur intention et mutualiser les détails réellement dupliqués.
- Chaque commit doit rester livrable : tous les tests doivent passer. Exécuter les
  tests ciblés pendant les corrections, puis toutes les suites de tests avant de
  committer. Après un échec, corriger et revérifier en ciblé, puis relancer toutes
  les suites sur l'état final avant le commit. Un test non exécuté ou une CI en
  cours ne vaut pas un succès ; si une vérification est indisponible, signaler le
  blocage et ne pas committer. Confirmer aussi la CI verte après chaque push.
- Communiquer en français, directement et brièvement. Code, commentaires et textes
  de l'application restent en anglais. Expliquer les changements et contrôles réels,
  distinguer les incertitudes, puis attendre la prochaine consigne.
- Ne pas déléguer à des sous-agents, committer ou pousser sans demande explicite.
- Messages de commit : `type(scope): description`, avec type et scope en minuscules
  et description en anglais. Types possibles : `fix`, `feature`, `tests`, `docs`,
  `build`, `ci`, `refactor`, `chore` selon l'intention du changement.

## Intention produit

**Sane rend la construction du logiciel compréhensible et participative, par étapes
humaines.** La compréhension et l'implication de l'utilisateur font partie du résultat.

- Montrer une progression sémantique, pas une liste d'outils, fichiers ou cases cochées.
- Un step livre un résultat cohérent, exerçable à son niveau. Une fondation testable
  peut précéder son branchement ; chaque step n'a pas à livrer une fonctionnalité finale.
- Les substeps exposent des intentions et changements compréhensibles ; leurs états
  intermédiaires peuvent être incomplets. Relier les explications au code réellement produit.
- Discussion, bifurcations, diffs, snapshots et validations soutiennent cette expérience,
  sans s'y substituer. Slop reste un choix légitime de délégation accrue.
- Ne pas présenter l'adoption future comme acquise : elle reste à valider par l'usage.

## Invariants techniques

- Séparer construction, review en lecture seule et validation humaine. Seul le harness
  autorise une phase d'exécution ; ni le modèle ni un contenu chargé ne l'activent.
- Ne pas transférer les permissions avec un historique, une review ou un skill.
  Une annotation MCP `readOnlyHint` est une déclaration, pas une garantie de sécurité.
- Conserver les historiques et leurs snapshots. Les vues historiques ne lisent pas
  silencieusement le checkout courant et ne rejouent jamais les effets des outils.
- Garder les résumés d'Activity légers : préserver `bash.mainCommand` et les patterns
  `grep`/`glob`, sans charger les anciens messages complets pour les compléter.
- Centraliser les chemins globaux C++ dans `PathResolution`
  (`src/agent-runtime/common/path_resolution.hpp/.cpp`).

## Stockage et secrets

Tous les chemins globaux utilisent un sous-dossier `sos/` dans les bases XDG :

| Base | Usage | Défaut |
| --- | --- | --- |
| `XDG_STATE_HOME` | État et sessions | `~/.local/state/` |
| `XDG_DATA_HOME` | Données persistantes | `~/.local/share/` |
| `XDG_CONFIG_HOME` | Configuration | `~/.config/` |
| `XDG_CACHE_HOME` | Caches recréables | `~/.cache/` |

- Installer la commande dans `~/.local/bin/` : pas de `XDG_BIN_HOME` standard ni de
  dossier global `~/.sos/`.
- Credentials providers : `$XDG_DATA_HOME/sos/auth.json`, permissions `0600`.
  Utiliser ce fichier exclusivement, jamais les clés API de l'environnement.
- Secrets MCP : `$XDG_DATA_HOME/sos/mcps/auth.json`, permissions `0600`, séparés
  des providers et de la configuration partageable. L'identité inclut portée et destination.
- Ne jamais placer de secrets dans le dépôt, les conversations ou les sessions.
  `/connect` et Connect MCP utilisent des champs masqués ou le parcours OAuth dédié.

## Références et entretien

- [Contrats produit](docs/product-contracts.md) : interactions et invariants détaillés.
- [Protocole daemon](docs/daemon-protocol.md), [snapshots](docs/git-snapshots.md),
  [skills](docs/skills.md), [MCP](docs/mcp.md) : formats et fonctionnement.
- Maintenir cet accord à partir des décisions explicites et conventions adoptées.
  Ne pas y accumuler un journal de session, recopier le README ou transformer une
  suggestion de l'agent en décision. Signaler ses mises à jour dans le compte rendu.
