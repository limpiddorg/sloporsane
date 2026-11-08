# Contrats produit

Ces exigences complètent [AGENTS.md](../AGENTS.md). Lire les sections concernées avant
une modification ; les détails de protocole restent dans les documents liés.
Les interactions TUI décrites ici visent Python/Textual, pas une obligation de parité FTXUI.

## Discussion et plans

- Discussion Sane reste en pleine largeur, sans panneau de plan permanent. Sous le
  dernier message assistant de chaque tour terminé, un drawer replié conserve le plan
  existant à cet instant. Ne rien ajouter aux messages intermédiaires ni sans plan.
- Le titre est `Objective plan`, ou exactement `Objective plan · updated` pour un tour
  qui publie une version, y compris après reprise/pagination. Titre et flèche sont Sane ;
  le contenu ouvert a une bande verte et un fond vert discret.
- Publier seulement des versions complètes, via remplacement ou patch ciblé, en conservant
  les précédentes. Parcourir l'historique ne change pas le plan courant du modèle.
- Les boutons centrés `Implementation plan` et `Implement (Auto plan)` partent de la
  version affichée et restent en bas du plan avant lancement. Auto plan demande d'abord
  Step by step / Everything, puis ouvre les steps ; son bouton de génération a un spinner.
  Un plan finalisé démarre la construction sans acceptation supplémentaire du plan.
  Échec/annulation arrête le spinner et permet de réessayer avec le rythme choisi.
- Après lancement, remplacer les actions du bas par `Implementation`, aligné à droite
  sur la ligne du toggle et accessible sans ouvrir le drawer. Un plan déjà accepté ouvre
  sa branche existante. Masquer actions et accès Implementation dans l'aperçu Discussion.
- Le plan d'implémentation non accepté reste à droite du chat, avec séparateur Sane,
  y compris après restauration. Sa version acceptée apparaît dans le chat en Sane avec
  accès Implementation. `Implement` est centré sous le plan ; le contrôle du chat reste
  Sane, ou Cancel pendant génération, et ne lance jamais une génération de plan.
- Un seul onglet Implement regroupe planification et construction. Après acceptation,
  sélectionner le premier step. L’entrée `Implementation plan`, alignée à gauche avec retrait au-dessus des steps,
  affiche le plan figé dans les diffs avec Changes vide dès qu'on y revient avec les
  flèches de navigation, sans Entrée supplémentaire, comme pour les steps.
  Le premier plan et ses révisions
  utilisent une ligne pleine largeur, avec le même espacement vertical, surlignée au survol
  et bleue à la sélection. `Implementation plan` et `Conclusion` partagent ce format
   de ligne large alignée à gauche, en gras, avec fond des messages (`content-background`).
   Le fond bleu sélectionné couvre toute la ligne, sans marges grises latérales.
  Naviguer sur `Conclusion` affiche directement son contenu, sans Entrée.
  Sélectionner un step revient à son inspection ; la vue du plan n'inclut pas la construction.
- Drawers, panneaux et boutons de plan ne prennent pas le focus du composer.
- Pendant génération, Discussion est consultable en lecture seule sans détacher/annuler
  le travail. Pendant toute la construction, même entre steps, elle reste figée sans
  indicateur de génération ; son composer demande de terminer l'implémentation.
  Conserver la page et la position lors des retours.

## AI Check

- `/permissions` propose Ask before, AI Check et Danger, indépendamment de Sane/Slop.
  AI Check affiche `API: …` puis `Model: …` sur deux lignes et le seuil dès sa sélection,
  sans activation avant Save. Ne pas afficher de paragraphe de migration des seuils.
  Sans clé configurée ni modèle en brouillon utilisable, masquer les lignes API et Model
  plutôt que montrer les valeurs par défaut comme une configuration active.
  Ask before et Danger conservent leur sélection directe.
- Les pourcentages sont la probabilité estimée de `allow`, pas le champ `confidence`
  du fournisseur : autoriser à partir du seuil inclus. Conserver les valeurs enregistrées,
  documenter leur signification et l’absence de garantie de sécurité ou
  d’équivalence de calibration entre modèles. À 0, aucun appel au vérificateur ; les
  restrictions Sane/review restent prioritaires.
- Save applique ensemble le modèle, le seuil et la clé en brouillon, puis ferme après
  acknowledgement. Une erreur laisse les réglages corrigeables. Fermer sans Save
  abandonne le brouillon et efface sa clé temporaire.
- Change model ouvre une popup : choisir OpenAI Decisions ou TypeSafe, puis renseigner
  endpoint complet, modèle libre et clé masquée. Proposer les endpoints officiels,
  préserver les paramètres historiques OpenRouter ; aucun appel daemon avant Save
  dans le parent. Use model ne fait que revenir au parent avec le brouillon.
- Échap dans les champs revient au choix d’API et efface la clé saisie ; Échap dans
  ce choix annule la popup. Une clé sauvegardée n’est jamais relue dans le formulaire.
  Sa réutilisation exige le même protocole et endpoint ; changer seulement le modèle
  peut la conserver. Les secrets restent dans auth.json protégé, jamais dans le chat.
- Exiger la capacité `permission_check_version: 1` pour configurer AI Check. Un ancien
  daemon ne doit pas ignorer silencieusement modèle ou nouvelle sémantique des seuils.
  Les historiques restent immuables, avec leurs scores enregistrés d’origine.

## Interactions Textual

- L'outil `wait` attend la fin d'une opération extérieure via une commande de surveillance,
  avec les mêmes permissions que `bash`. Tests, builds et installations restent dans `bash`,
  quelle que soit leur durée. Sans commande d'attente native, le modèle fournit un polling
  borné avec conditions explicites de succès/échec. Les sorties intermédiaires ne relancent
  pas le modèle ; fin du processus, timeout ou annulation rendent la main. Le timeout par
  défaut est d'une heure. Arrêter le watcher local n'annule pas l'opération distante.
- Pendant un appel `wait` actif, Activity garde son spinner et affiche une ligne de statut
  dessous, après le contenu quand le drawer est ouvert, alignée sur le A d'Activity.
  Le modèle fournit `status`, une phrase de cinq mots maximum (de préférence quatre ou cinq),
  souvent `Waiting…`, mais sans préfixe imposé, ni points finaux. `.`, `..`, `...` bouclent
  dans un espace stable. Un reflet lent nettement blanc traverse uniquement le texte de
  gauche à droite, sans éclairer les points ni le fond.
  Arrêter l'animation au résultat ou à l'interruption ; ne pas l'animer dans l'historique terminé.
- Les titres d'outils ne sont pas en gras. Pour bash, afficher `mainCommand` en gris entre
  statut et exit code éventuel ; ce libellé descriptif ne change ni exécution ni permissions.
  Préserver libellés et patterns grep/glob dans les résumés légers, sans lire les anciens bodies.
- Chemins des titres, lignes read et en-têtes de fichiers : une seule ligne, raccourcie au
  milieu en gardant début/fin, recalculée au redimensionnement. Résumés gris, contenu DEPTH.
- Dans Activity, décaler les flèches internes de deux colonnes. Le contenu ouvert commence
  deux colonnes après la première lettre du titre, y compris Reasoning, outils et Compaction.
- Modales : Entrée valide, Échap revient/ferme, sans boutons Confirm/Connect/Cancel/Close
  redondants. Les actions distinctes restent visibles ; gauche/droite choisit l'action,
  validation/Save sélectionnée par défaut. Préserver gauche/droite pour éditer les champs.
- Haut/bas pilote la dernière liste visible et active utilisée, même depuis un champ/action ;
  sans liste, conserver la navigation normale. Dans Implement, haut/bas pilote toujours les
  steps, même depuis Changes ou la review. Afficher une aide clavier adaptée.
- Les chats suivent les messages uniquement si l'utilisateur est en bas ; ne pas déplacer
  une personne remontée dans l'historique.

## Version et mises à jour

- Afficher seulement `dev` ou `vMAJOR.MINOR.PATCH` en haut à droite, sur la ligne des tabs. La version est
  embarquée au build via `SOS_VERSION`, `dev` par défaut et tag pour les releases.
- Un build `dev` ne consulte jamais le backend pour les mises à jour. Une version
  stable vérifie au lancement, en arrière-plan et sans bloquer, la version publiée
  servie par le même backend que l'installateur. Échec réseau silencieux.
- Nouvelle version : popup en anglais, `Not now` sélectionné par défaut, Échap
  ferme. Avertir que toutes les TUI SOS et le daemon de l'utilisateur seront fermés.
- Après accord, quitter Textual et passer à un processus Bash indépendant dans le
  même terminal. Une seule confirmation ; préparer/vérifier les téléchargements
  avant d'arrêter les autres processus. Le processus d'installation survit à l'arrêt
  de SOS et du daemon, puis relance uniquement l'instance initiatrice dans son
  workspace, avec son socket. En cas d'échec, garder le lanceur précédent et
  afficher l'erreur dans le terminal. Refuser une version différente de celle validée.

## Construction et inspection

- Un step ne se déplie qu'une fois construit. Cliquer inspecte, sans Cancel ni validation.
  Entrée sur le step actif déclenche Cancel avec confirmation, ou Continue s'il est interrompu.
  À la réouverture d'une construction interrompue/en échec, sélectionner le step de travail
  avec Continue ; ne pas forcer cette sélection en live ni dans les archives.
- À la réouverture, afficher immédiatement Loading implementation et un spinner dans les
  diffs jusqu'à réception de l'état ; arrêter l'attente et signaler les erreurs.
- Conserver sélection et diff pendant les mises à jour live. Le retour sur une substep
  restaure sa zone de diff (fichier/hunk), mémorisée uniquement dans la TUI par version
  et substep, jamais dans la session. La navigation garde la ligne sélectionnée visible
  par le défilement minimal, sans mouvement si la sélection ne change pas.
  Le menu plan/steps/Conclusion utilise un seul scroll externe, pas de scroll interne des steps.
  PgUp/PgDown font défiler le panneau de contenu affiché (diff, texte du step, plan,
  conclusion), indépendamment du focus ; si la review est ouverte via Tab, ils ciblent
  son chat à la place.
- Montrer dès le début tous les titres numérotés des substeps prévues ; activité/explication
  seulement lorsqu'elles existent, ou pour le travail en cours. Les titres/descriptions du
  contenu gardent leurs couleurs normales, sans marqueurs `[x]`/`[ ]` ni vert de validation.
- Sous la description du step : compte terminé/total à gauche d'une barre texte `[###---]`,
  largeur stable et marges, en MISC. Compter les finalisations réelles, pas une promesse du
  modèle ; les substeps explicitement écartées comptent. La barre reste pleine en réparation.
- Les timers Activity suivent l'exécution réelle, même hors écran. Afficher la durée
  active fournie par le daemon et prolongée localement, sans redémarrer à la navigation.
- Pendant validation technique : `Validating` au-dessus, reflet blanc rapide synchronisé
  sur mot/barre en aller-retour, arrêté à la fin/interruption. Après les substeps, montrer
  `Validating the step`, sans numéro, en MISC, avec Activity mais sans description.
- Réserver tests, builds, linters et inspections de vérification à la validation technique,
  pas aux substeps de construction ; les lectures nécessaires à l'implémentation restent
  permises. Les réparations de validation ciblent les substeps d'origine. Capturer l'appartenance pendant les mutations/frontières, sans
   redistribution globale des hunks finale ni perte des contributions inchangées.
- Garder la validation proportionnée aux changements du step et aux exigences explicites
  du plan. Réutiliser les résultats réussis encore valides ; pas de relecture globale ni
  de répétition de contrôles sans incertitude ou risque de régression concret. Après une
  réparation, relancer les contrôles affectés. Le dernier step ne déclenche pas un nouvel
  audit de toute l'implémentation. Les contrôles requis restent obligatoires.
- La phase de validation reste une frontière obligatoire, pas une obligation de nouveaux
  contrôles. Si aucun contrôle supplémentaire n'est nécessaire, l'expliquer et clôturer
  le step. Le plan précise les vérifications nécessaires ou la raison de leur absence,
  sans inventer de substep de vérification ni d'audit final systématique.
- Pendant la construction normale, finaliser ou écarter une substep active automatiquement
  la première substep restante, ou la validation si toutes sont finalisées. Persister cette
  transition atomiquement avec la finalisation, sur le même snapshot et sans lui attribuer
   de mutations. Activer aussi la première substep avant l'exécution, sans appel modèle ;
   une reprise conserve la frontière existante. Les cibles de réparation/révision restent explicites.
  Une activation n'atteste pas qu'une édition a commencé et ne vaut pas validation humaine.
- Dimensionner plans et tests selon la demande et les risques concrets, sans multiplier
  les steps ou les cas limites hypothétiques du seul fait de Sane. Privilégier les tests
  existants et contrôles courts ; ne pas créer un framework de simulation pour remplacer
  un navigateur indisponible sans nécessité explicite. Clôtures et conclusion réutilisent
  les résultats capturés sans récapitulatif redondant, tout en signalant les limites réelles.
- Soumettre `complete_implementation_step` avant une fin normale. Une soumission réussie
  termine le tour sans rappel modèle ni outil suivant. Réutiliser les dernières explications
  completed/unnecessary sauf révision explicite ; pas d'inspection systématique des diffs
  pour les constructions à appartenance capturée. Grouper les annonces/outils ordonnés
   lorsqu'aucune décision ne dépend d'un résultat intermédiaire.
- Terminer le travail manquant dans le périmètre, réparer et revérifier. Blocage, refus ou
  annulation reste une interruption explicite, jamais une complétion fictive. Les contrôles
  requis non exécutés, échecs inexpliqués et régressions bloquent. Un échec démontré sur la
  baseline, préexistant et sans rapport, peut rester documenté dans step/conclusion sans
  bloquer la clôture technique, sans le déclarer réussi ni autoriser une réparation annexe.
- Une substep sans diff finale sort automatiquement des diffs à valider ; conserver son
  activité/explication. Une substep barrée affiche une raison générique indiquant que les
  changements précédents la rendent inutile, sans fabriquer du travail.
- Durée cumulée et débit agrégé : seulement sous les titres du menu, pas dans le contenu.
  Débit = tokens de sortie / temps des appels modèle ; absent si le provider ne donne pas
  les tokens. Statistiques muted, ou TITLE jaune sur la sélection bleue.
- Step actif MISC : ligne sous le titre avec `[terminé/total]`, deux premiers mots du
  substep courant, `...` et spinner. Steps validés Algae, futurs muted, construits non
  validés blancs. Vert de validation seulement dans menu/récapitulatif Human validation.
  Validate et Cancel de construction sont DEPTH ; Cancel du chat est Sane, libellé Sane
  noir et centré hors génération.

## Arbre Changes

- Lire les enfants directs de la racine et des dossiers finaux contenant des changements.
  Ajouter leurs ancêtres sans charger les voisins intermédiaires ; lire les autres dossiers
  seulement à leur ouverture explicite. Aucun inventaire récursif daemon/TUI.
- Toujours lire le snapshot historique, jamais le checkout courant. Compacter les chaînes
  sans bifurcation en préservant branches/dernier dossier ouvert ; raccourcir les chaînes
  au milieu si nécessaire, jamais les noms de fichiers.
- Chaque dossier final affiche cinq éléments au total, avec tous les changements et leurs
  branches obligatoires, quitte à dépasser cinq. `...` seulement pour des voisins masqués,
  non cliquable. Changes reste manipulable à la souris sans prendre le focus des steps.
- Si voisins masqués ou chemin raccourci, `Show all` en dernière ligne accessible au scroll
  révèle voisins et structure complète. Ouvrir seulement les ancêtres des changements en
  lisant leurs enfants directs, pas récursivement les autres dossiers. `Compact` restaure
  la projection compacte. Conserver le choix en live, réinitialiser au changement de step/snapshot.

## Review et corrections

- Review seulement sur le premier step non validé construit et ses substeps, dans les deux
  rythmes. Ailleurs, Tab ou `/` signale l'indisponibilité sans bloquer l'inspection.
- Depuis le menu, `/` ouvre/focalise la review et insère `/` sans remplacer le brouillon.
  Suggestions : skills et `/connect`, `/model`, `/permissions`, `/mcps` seulement ; refuser
  explicitement les autres commandes natives. Les skills ne changent pas les permissions.
- Chaque step conserve sa conversation, son contexte de construction arrêté à sa fin,
  et les résumés précis des reviews précédentes. Chaque question inclut fichier/hunk regardés.
- Le composer reste éditable pendant réponse ; conserver le prochain brouillon sans lancer
  un second tour. Strictement vide et focalisé, gauche/droite navigue les diffs sans fermer
  la review ni déplacer le focus ; avec texte, conserver l'édition normale.
- Une proposition focalise Accept ; gauche/droite choisit Accept/Decline, même depuis le
  chat vide, jamais les diffs. Accepter ferme la review et sélectionne immédiatement le step
  corrigé avec Preparing pendant les opérations asynchrones.
- Montrer une seule version complète d'implémentation à la fois. Le sélecteur
  `← Version N →` est au-dessus d'Implementation plan dans le menu gauche,
  avec libellé centré en gras, fond du composer (`input-background`) sur le bandeau et
  flèches désactivées aux bornes. Avec plusieurs versions, le libellé est sélectionnable
  à la souris ou via ↑ depuis le plan ; ←/→ change alors la version sans parcourir les diffs,
  et ↓ revient au plan. Le panneau principal affiche `Version N` centré en titre gras
  tant que le libellé est sélectionné. Avec une seule version, cette entrée n'est pas focalisable.
  Les flèches ont un fond légèrement plus clair que le composer (`#3A4655`) ;
  `Show all` / `Compact` ont le fond classique des boutons de popup. Tous passent au
  bleu au survol, sans fond bleu permanent. Il pilote ensemble le plan, les steps, substeps,
  Activities, explications, validations techniques et diffs issus des snapshots de cette version,
  y compris les steps retirés depuis. Aucun Previous results, plan numéroté intercalé,
  ancien step barré ni sélecteur propre à un step. Le contenu est aligné à gauche et en haut,
  même pour la version initiale seule ; le bouton Implementation plan est aligné à gauche.
  Après acceptation d'une correction, afficher immédiatement la nouvelle version avec Preparing,
  puis sa progression normale. Résumé global pour l'initial, demande acceptée pour une correction.
  Les versions anciennes restent en lecture seule et stables en live ; les parcourir ne relance
  rien et ne change ni la version à valider ni l'état réel de l'exécution.
- Seule l'acceptation intègre le segment courant de review à la construction, comme historique
  sans replay ni permissions. Persister sa frontière ; conserver les échanges visibles mais
  ne plus les injecter séparément dans la review suivante. Les résumés des steps suivants
  portent uniquement sur le segment non intégré.

## Résumés et compaction

- Préparer les résumés de reviews au premier message du step suivant, puis les persister
  et réutiliser. Même génération, Activity et annulation normales, sans popup, validation
  ou annonce spéciale. Modèle sélectionné, contexte dédié, échanges originaux avant/après
  compaction, jamais messages de compaction ni construction.
- Bound sources by chunks and recursive merging. Review summaries retain two corrections
  followed by an emergency synthesis. Ordinary compaction requests MAXIMUM 1% of the
  effective input window with a 2,000-token budget floor, less whenever possible,
  with essential continuation state only. Accept up to 5% internally with the same
  2,000-token floor, without advertising this tolerance to the model. Both budgets
  are capped by the model output limit; the floor is not a minimum summary length. Allow at
  most three attempts per synthesis (initial call and two size corrections), keeping the
  shortest candidate. If still too long, truncate at a UTF-8 boundary with a warning;
  never discard original history or hide provider failures/cancellation.
- Display `Compaction` and its spinner without a counter on the initial call of each
  chunk or merge. Only size corrections show `2 / 3` or `3 / 3`, without a bullet or
  the word `Attempt`. Ordinary chunk/merge calls do not increase the retry counter.
- Vérifier construction + résumés + question avant injection ; compacter d'abord la
  construction si nécessaire. La review compacte aussi pendant ses réponses.
- Chaque vraie compaction : drawer rouge `Compaction`, flèche alignée sur Activity,
  spinner pendant exécution, sans barre, entre les Activities. Même règle en construction
  et validation ; une compaction initiale peut précéder la première Activity. À la fin
  réussie, afficher l'Activity suivante et son spinner sans attendre son premier message.
- Après compaction, conserver la dernière annonce de phase exacte après le rappel Sane/Slop ;
  ne jamais réactiver une ancienne autorisation d'exécution pendant une review.

## Validation humaine et conclusion

- Human validation ne montre que le récapitulatif des substeps, pas la description globale.
  Disponible seulement sur le premier step non validé ; Échap ferme sa confirmation.
- Importer avant de valider globalement le step. Désactiver Validate pendant import et
  bloquer les doubles envois. Après confirmation, replier le step et sélectionner le résumé
  du suivant avec titres/substeps et Preparing ; ne pas avancer avant confirmation ou sur conflit.
- Conclusion figure après le dernier step, consultable dès que tous sont construits.
  Le dernier `complete_implementation_step` fournit un récapitulatif global un peu plus
  détaillé que les explications, sans répétition exhaustive, avec contrôles/réserves, conservé
  dans l'archive. Pas de Close avant dernière validation humaine ni dans les archives.
- Après dernière validation, ouvrir Conclusion et focaliser immédiatement le bouton Close
  classique en bas du contenu, même hors écran, sans obligation de scroll automatique.
  Sélectionné, il est bleu DEPTH ; Entrée revient à Discussion, ferme Implement et fige l'archive.

## Workspaces et isolation

- `worktree` liste/crée/sélectionne dans le dépôt actif ; `workspace` déplace la conversation
  vers un dossier existant, avec/sans Git, autre dépôt inclus. Création/bascule seulement
  sur demande directe ou prescription claire d'un skill explicitement invoqué. Un fichier
  découvert ne suffit pas. Pas de popup supplémentaire ; autorisé en Sane, pas pendant
  Implement/review. La liste reste disponible. Commit/merge/push restent des actions Bash séparées.
- Les skills/MCP restent hérités du workspace source ; AGENTS suit le dossier actif. Pas de
  copie de configuration ou de changements locaux lors d'un déplacement. `/resume` restaure
  le workspace ; une nouvelle session démarre dans le dossier courant sans création automatique.
  `/undo` reste lié aux chemins d'origine, Implement importe dans le workspace actif.
- Créations utilisateur depuis HEAD sous
  `$XDG_DATA_HOME/sos/workspaces/<repo>-<identifiant>/<nom>/`, branche `sos/<nom>`.
  Identifiant issu du Git commun canonique, partagé entre worktrees mais distinct entre clones.
  Ne pas déplacer/renommer les worktrees existants ni les nettoyer automatiquement.
  Une PR ne déclenche ni bascule ni avertissement particulier.
- Le sélecteur `/worktree` et sa suppression sont Slop seulement. Liste et `New…` : Entrée
  sélectionne/crée, Échap revient/ferme. Select par défaut à gauche, Delete à droite.
  Confirmation de suppression exceptionnelle : branche/emplacement seulement, Cancel
  sélectionné à gauche, Validate à droite, gauche/droite choisit, Entrée exécute, Échap annule.
- Interdire la suppression du checkout principal et des destinations utilisées par les
  intégrations ou sessions attachées/en chargement ; afficher Current / Opened in another
  session en corail. Revérifier et réserver côté daemon. Ne forcer ni sale/verrouillé,
  ne supprimer aucune branche. Les sessions fermées ne bloquent pas, mais une destination
  disparue bloque leur reprise sans repli silencieux.
- Implement utilise `$XDG_DATA_HOME/sos/worktrees/`, distinct des workspaces utilisateur.
  La baseline reproduit l'état local uniquement sur la branche SOS, sans stash ni modification
  de branche/index utilisateur. Commits de stockage sans hooks/signature. Validation humaine :
  import du seul step dans les fichiers, sans commit utilisateur ; conflit = step bloqué et
  bandeau Slop existant. Interruption conserve le checkout ; Close retire checkout/branche
  temporaire mais conserve archives et objets épinglés pour inspection historique.
- Sans Git ni ancêtre Git, dépôt privé hors workspace, aucun `.git` utilisateur ni remote.
  Respecter `.gitignore`/exclusions globales, toujours exclure `.sos/` même réinclus.
  Parcourir les dépôts imbriqués à toute profondeur comme fichiers ordinaires sans leurs `.git`.
  Copie CoW si disponible (`clonefile` / `FICLONE`), sinon classique, jamais hardlinks vers
  l'original ; capturer ensuite la baseline. Ce n'est ni une sandbox ni une copie instantanée garantie.

## Skills

- Découverte : `<workspace>/.sos/skills/` puis `$XDG_CONFIG_HOME/sos/skills/`, workspace
  prioritaire à nom égal ; jamais `.claude/` ou `.agents/`. Détails : [skills.md](skills.md).
- Préfixe `/` partagé avec les commandes natives ; noms natifs réservés. Exclure un skill
  en conflit des suggestions et signaler l'erreur sans bloquer la commande native.
- Suggestions : skills correspondants triés alphabétiquement d'abord, Algae, commande à gauche,
  `Skill` gras à droite ; natifs triés ensuite, MISC, titre d'aide sans `/`. Tab sélectionne,
  Échap masque avant toute fermeture/annulation. Compléter au curseur partout sans changer
  le texte autour ; une référence dans une phrase n'est pas une invocation structurée.
- Afficher le `argument-hint` littéral après le nom dans les suggestions, pas dans le titre
  d'aide ; hint grisé en saisie seulement avant le texte d'arguments de la chaîne initiale.
  Ne pas retirer ses chevrons/crochets. Jusqu'à six skills consécutifs en tête de message,
  chacun reçoit le même texte brut final, sans binding/validation TUI.
- `arguments` : liste de noms ou chaîne séparée par espaces, uniquement des alias positionnels.
  `argument-descriptions` : mapping optionnel nom → description, liste numérotée dans l'ordre
  des arguments ; descriptions seules possibles. Aucun required, `{nom}` ni objet nommé.
  Ignorer les autres champs frontmatter sans diagnostic ni activation de mécanismes.
- Substitution unique de `$ARGUMENTS`, `$0`, `$1`, `$ARGUMENTS[N]` et alias, guillemets shell
  sans exécution. Indices absents littéraux, alias absents vides ; sans placeholder recevant
  le texte, ajouter `ARGUMENTS: ...`.
- Activity : `skill · nom · statut` en live/reprise, sans ouvrir les arguments pour lire le nom.

## MCP et providers

- Codex conserve les messages `system`/`developer` dans l'ordre de l'input : ne pas
  extraire les annonces tardives vers le champ initial `instructions`. Ce champ requis
  reste constant ; l'ajout d'un message ne réécrit pas le préfixe déjà envoyé. Cela ne
  supprime pas les frontières explicites de compaction ni ne garantit un cache hit.
  En Sane comme en Slop, envoyer les messages `system` sous forme EasyInputMessage
  sans `type`, acceptée par Codex ; cette adaptation porte uniquement sur la copie
  réseau, sans changer ordre, rôle, contenu ni historique stocké. L'API OpenAI
  classique conserve son format existant.
- Client MCP daemon : stdio et Streamable HTTP, pas HTTP+SSE historique. Config JSON
  `mcpServers` dans `<workspace>/.sos/mcps.json` ou `$XDG_CONFIG_HOME/sos/mcps.json`,
  entrée workspace remplaçant entièrement le global homonyme. `url` ou `command` chaîne
  avec `args` tableau / `env` public ; pas de version requise. Refuser les anciens formats
  sans réécriture. Activation par défaut, approbation séparée obligatoire pour chaque nouvelle config.
- `/mcps` gère seulement Connect/Disconnect et Enable/Disable ; ajout/édition/suppression
  par fichier, pas `/add`, Refresh, Credentials ou Approve séparé. Liste compacte
  Server / Scope / Connected / Enabled, actions sur une ligne, sans Close. Premier serveur
  et Connect/Disconnect sélectionnés ; aide haut/bas serveur, gauche/droite action, Entrée,
  Échap. Guide de configuration succinct dans le contexte du modèle, sans secrets.
- Connect enchaîne approbation, authentification automatique nécessaire et connexion.
  Approbation/token : popups séparées, champ token masqué. OAuth seulement sur ce parcours
  explicite : popup au-dessus de la liste, lien réel et Open in Browser dès découverte
  de l'URL, jamais inventée ; Échap annule et revient à la liste. Ne pas agrandir la liste.
- Connected et Enabled indépendants : désactiver cache/refuse les nouveaux appels sans
  fermer la connexion ni annuler l'appel actif ; connexion explicite possible désactivé.
  Seul `readOnlyHint: true` rend un outil admissible en Sane, sous les autres permissions ;
  absent/faux = potentiellement mutateur. Ce n'est pas une garantie de sécurité.
- Choix persistés immédiatement par nom, globalement dans
  `$XDG_CONFIG_HOME/sos/mcp-preferences.json`, jamais par session/workspace. Restaurer les
  connexions souhaitées même désactivées avec config/credentials/approbation du workspace.
  Pas de serveur inventé si son nom est absent, pas d'OAuth automatique.
- Restauration en arrière-plan, sans attendre le handshake dans `integration.configure`.
  Connecting dans la liste ; les outils d'un tour attendent la restauration dans son Activity.
  Échec dans liste + bandeau, sans changer le choix ni retry automatique dans ce daemon.
  Disconnect persiste. Sans préférence, respecter fichier et connexion au premier besoin
  des serveurs activés/approuvés. Préférence globale prime sur enabled sans modifier le fichier.
- Pas de resources/prompts ni gestion avancée OAuth/secrets en TUI ; options avancées par
  fichier. Pas de lecture autonome des resources par le modèle ni injection à réception
  de notifications. Secrets séparés selon [AGENTS.md](../AGENTS.md) et [mcp.md](mcp.md).
- Une authentification/méthode par provider ; reconnecter remplace seulement son entrée.
  `/connect` choisit et mémorise le premier modèle compatible du dernier provider connecté.
  OpenRouter : `sort=newest` ; OpenAI API : created décroissant, absents en fin ; Codex :
  priority croissant, absents/invalides en fin ; égalités stables. Sélecteur Textual sur
  premier résultat à l'ouverture et à chaque recherche, sans tirage aléatoire.
- Modèle/réflexion/maximum de contexte : triplet par conversation, persisté ; dernier choix
  explicite aussi défaut des nouvelles conversations dans `config.json`. `/resume` restaure
  sans modifier défaut/autres sessions ni métadonnées historiques. Maximum choisi au moins
  32 768 tokens, au plus le maximum connu du provider.

## Références d'expérience

Des approches voisines existent : [AI Code Walkthrough](https://github.com/mmuminovic/AI-Code-Walkthrough),
[Kiro](https://kiro.dev/docs/specs/), [Superpowers](https://github.com/obra/superpowers)
et [Cline](https://docs.cline.bot/features/checkpoints). Elles ne prouvent ni l'unicité
ni l'adoption future de Sane ; un retour spontané sur une tâche réelle est un signal
plus fort qu'un intérêt déclaré.
