# Mesures des tests

Mesures locales dans le worktree `optimize-tests`, macOS, Python 3.14.8,
Textual 8.2.8 et dépendances de `uv.lock`. Le build natif est en Release.
Installation des dépendances, compilation, packaging et FTXUI sont exclus des
chronométrages. Les suites Python et C++ ont été mesurées séparément, sans les
faire concourir pour le CPU.

## Résultats

| Suite | Référence séquentielle | Exécution optimisée, 4 workers | Gain |
| --- | ---: | ---: | ---: |
| Textual | 825,18 s | 127,39 s | ×6,48, soit −84,6 % |
| C++ runtime/daemon | 349,62 s | 98,95 s | ×3,53, soit −71,7 % |

La référence Textual découvrait 911 tests, dont 11 ignorés faute de daemon.
La mesure optimisée exécute 916 tests, sans aucun ignoré : les cinq tests du
lanceur ont été ajoutés et les 11 tests daemon sont maintenant exécutables.
La référence C++ comptait 687 tests ; la suite corrigée en compte 689, dont trois
ignorés dans les deux mesures.
Ces chiffres sont des observations sur cette machine, pas des seuils garantis.

Les principaux coûts Textual initiaux étaient `test_implementation_workspace.py`
(227,50 s, 141 tests) et `test_chat.py` (98,81 s, 84 tests). Ensemble, ils
représentaient environ 40 % du temps total. Une expérience sur huit cas a mesuré
26,40 s avec le debug asyncio contre 16,84 s sans, à résultats identiques.

Le test natif
`ImplementationWorktreeTest.ResumePartialPreparationAllowsSlowCleanupAndBaselineHashing`
prend désormais environ 1,2 s contre 64 s. Il inspecte les options réellement
transmises au processus, notamment le budget de dix minutes supérieur au timeout
générique, puis exécute toujours les vraies commandes Git de reprise. Les deux
`sleep 31` ont été supprimés, pas les contrôles de nettoyage et de baseline.

## Changements

- Le lanceur `integrations/tui/run_tests.py` répartit les cas individuels entre
  processus créés avec `spawn`, plutôt que de laisser un gros fichier monopoliser
  un worker. Chaque lot a des chemins XDG temporaires propres.
- Le debug asyncio est optionnel. Les interactions, animations, assertions et
  nettoyages `unittest` ne sont pas désactivés.
- Les tests purs du compteur de substeps, des couleurs de diff/bullets et du
  formatage des durées n'ouvrent plus une application ou un serveur factice.
- Les tests d'expiration vérifient les délais enregistrés et déclenchent les vrais
  callbacks, sans patienter trois secondes réelles.
- Les tests d'interaction concernés attendent explicitement les effets différés
  utiles : focus de proposition, fin de l'état actif d'un bouton, scroll de fin de
  montage. L'attente OAuth utilise un timeout mural borné plutôt qu'un nombre
  d'itérations dont la durée dépendait du debug asyncio.
- Les sockets E2E sont créés sous un chemin temporaire court. Dans ce worktree,
  leur ancien chemin mesurait 132 caractères et échouait avec
  `AF_UNIX path too long`.
- Les tests de démarrage du vrai daemon utilisent le binaire `build-ai/bin/sosd`,
  sans imposer la fabrication ou l'installation d'un bundle utilisateur.

## Vérification et réserves

La référence n'était pas verte : 12 échecs et trois erreurs Textual, six échecs
C++. La mesure Textual complète optimisée a signalé 17 échecs et trois erreurs.
Les corrections de synchronisation ont été effectuées après cette mesure ;
seuls les tests ciblés ont été relancés ensuite, pas toute la suite.

La dernière vérification de 30 cas ciblés comprend le lanceur, les tests de scroll
et les interactions devenues sensibles au timing. Elle signale uniquement quatre
échecs de scroll déjà présents dans la référence. Les répétitions ciblées des
interactions permission/proposition et des previews ont également passé.
Une vérification ciblée de 33 cas, incluant le lanceur et les E2E, passe sur
Python 3.11.9. Les 21 tests `ImplementationWorktreeTest` passent.

Le test de Conclusion longue exigeait aussi un défilement automatique jusqu'à
Close. Cette attente a été retirée après clarification de l'auteur : le bouton
doit recevoir le focus par défaut, même hors de la zone visible, sans obligation
de défilement. Le test `test_final_validation_focuses_close_with_long_conclusion`
conserve la vérification du focus et du retour aux steps, sans changement produit.

La première validation C++ complète optimisée (89,18 s) retrouvait les six échecs initiaux :
`SessionTest.WorktreeToolMovesLiveConversationWithoutConfirmationAndResumeKeepsSourceConfiguration`,
`SessionTest.WorkspaceToolMovesAcrossPlainFoldersRepositoriesAndWorktreesKeepingSourceConfiguration`,
`SessionTest.SkillInvocationValidatesBeforeGenerationAndPersistsExactLoadedContent`,
`SessionTest.AutomaticCompactionStopsIfTheSummaryStillExceedsTheThreshold`,
`SessionTest.CompactionPreservesFullHistoryAndRestoresOnlyActiveContextAcrossRepeatedCompactions`
et `PermissionModesTest.LoadingSkillNeverRunsItsScriptAndBashStillEnforcesSane`.
À la demande de l'auteur, ces scénarios ont ensuite été corrigés : invocations de
skills avec `input` au lieu de l'ancien objet `arguments`, isolation/restauration de
`XDG_CONFIG_HOME` dans les fixtures, recherche du skill hérité par son nom, et
assertions de compaction adaptées aux réductions par blocs. Le contrôle de rejet
d'un contexte encore trop gros est conservé dans un scénario distinct, sans
activation du checkpoint. Un nouveau test de compaction classique couvre le
découpage, les trois réductions et le repli final borné. Le mécanisme de production
n'a pas été retiré ni assoupli.

La validation ciblée correspondante passe ses 101 tests. La validation native
complète après correction passe en 98,95 s : 689 tests enregistrés, 686 réussis,
trois ignorés et aucun échec. Les réserves Textual ci-dessus restent inchangées.

Les rapports locaux détaillés sont dans `build-ai/test-measurements/` :
`baseline.json`, `final-tui.json`, `last-targeted.json`, `python311.json`,
`native.xml`, `final-native.xml`, `skills-compaction-targeted.xml` et
`final-native-fixed.xml`, avec leurs logs associés. Le rapport
`baseline-conclusion-fast.log` conserve la reproduction sur le test de `HEAD`.
Ces artefacts sont ignorés par Git et ne sont pas livrés avec le code.

Voir les commandes reproductibles dans [CONTRIBUTING.md](../CONTRIBUTING.md#tests-pythontextual).
