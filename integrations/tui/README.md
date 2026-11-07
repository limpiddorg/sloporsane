# TUI

TUI Textual dont le code se trouve dans `src/sos_tui/`. Le développement utilise
uv ; le build CMake produit une application autonome installée sous le nom `sos`.

Depuis `integrations/tui/` :

```sh
uv sync
uv run tui
```

Les scrollbars de toute la TUI partagent un style sombre au survol et pendant le
glissement : poignée `#5A6672`, fond `#131616`. Leurs couleurs au repos sont conservées.

### Skills SOS

Les skills de `.sos/skills/<nom>/SKILL.md` et de `$XDG_CONFIG_HOME/sos/skills/`
(`~/.config/sos/skills/` par défaut) apparaissent dans les suggestions `/`.
Chaque ligne affiche le nom suivi du `argument-hint` littéral s'il existe :
`/check-api [directory] [note]`. L'origine `workspace` ou `user` reste dans la
fiche ; le workspace est prioritaire à nom égal. La complétion insère seulement
le nom, sans le hint. Tab, Maj+Tab, les flèches ou un clic changent la sélection.

La fiche occupe la moitié de l'écran : nom sans hint, description, puis catégories
Scripts, References et Assets avec les chemins relatifs récursifs triés, sans
lire leurs contenus. Les catégories vides sont masquées ; les erreurs de listing
restent visibles. La licence suit le titre en muted, l'origine reste à droite.
La compatibilité apparaît sous le titre en muted ; Metadata termine la fiche,
une ligne par paire `clé: valeur`. Les champs absents et Metadata vide sont masqués.
Le champ optionnel `argument-descriptions` (mapping nom → description) rétablit
la section Arguments numérotée avant Scripts, dans l'ordre de `arguments` ; les
libellés décrits sans alias suivent dans l'ordre reçu du catalogue. Sans descriptions,
la section reste masquée. Aucune marque obligatoire/optionnel ni validation des valeurs.

Le hint est une aide visuelle, jamais validée ni envoyée. La saisie l'affiche en
grisé tant que les arguments n'ont pas commencé, uniquement pour la chaîne de
skills initiale. Dès qu'on saisit du texte, il disparaît entièrement ; il ne
calcule pas les arguments restants. Sans hint déclaré, aucune aide n'est inventée.
Son texte est affiché tel quel, chevrons et crochets compris : c'est à l'auteur du
skill de choisir la ponctuation, pas à la TUI de la supprimer.

La TUI transmet tout le texte final brut, sans binding ni découpage des arguments.
L'ancien schéma SOS (`description`, `required`, `{nom}` et objet d'invocation nommé) est
supprimé. `arguments: [directory, note]` ou `arguments: directory note` définit
seulement les alias positionnels `$directory` et `$note`, dans l'ordre déclaré.
Le chargeur remplace aussi `$ARGUMENTS` (texte complet), `$0`, `$1` et
`$ARGUMENTS[N]` (tokens avec guillemets de type shell, sans exécution). Pour une
phrase entière sans guillemets, utiliser `$ARGUMENTS`. Les indices absents restent
littéraux, les alias déclarés absents deviennent vides. Sans placeholder recevant
le texte, `ARGUMENTS: ...` est ajouté. Les valeurs insérées ne sont pas réinterprétées.
Aucun argument manquant ou supplémentaire ne bloque l'envoi ; un ancien mapping
SOS est rejeté avec un diagnostic de migration. Les skills personnels ne sont
pas réécrits automatiquement.
Les champs frontmatter supplémentaires sont ignorés sans erreur : ils n'activent
ni hooks, ni sous-agents, ni permissions ou outils préapprouvés.

Les commandes natives et les skills utilisent `/` ; les noms natifs sont réservés.
Un skill en conflit est exclu des suggestions et signalé sans bloquer la commande
native. La complétion remplace uniquement le mot au curseur, partout dans le message.
Une référence dans une phrase reste du texte libre à l'envoi.
En tête, jusqu'à six skills consécutifs sont chargés dans l'ordre :
`/caveman /check-api src/http` transmet `src/http` à chacun. Le premier mot qui
n'est pas un skill connu, ou le septième skill, et la suite deviennent le texte
commun. Tous sont chargés avant génération : une erreur de chargement rejette
l'ensemble et conserve le brouillon. Chaque skill accepté a sa propre Activity.
Pendant un tour actif, l'invocation rejoint la file des messages.

Le chargement apporte les instructions, leur localisation, le texte brut et les
chemins des scripts, sans les lire ni les exécuter. Les outils ordinaires restent
soumis aux permissions et à Sane/Slop. Voir
[le format et le chargement d'AGENTS.md](../../docs/skills.md).

### Conversations `/btw`

Pendant un tour principal, `/btw Pourquoi ce choix ?` crée une conversation
informative parallèle, nommée `btw-1`, puis `btw-2`, etc. Son panneau s'ouvre entre
le message utilisateur principal et Activity. Sa flèche est alignée avec celle
d'Activity ; le contenu est indenté de deux espaces supplémentaires et encadré
par deux traits MISC. L'en-tête, la bande utilisateur et l'identifiant utilisent
le bleu lagon MISC. L'identifiant précède le modèle, la réflexion et la durée en
bas à droite de chaque réponse pour continuer avec `/btw btw-1 Précise ce point`.
Après chaque envoi BTW accepté, le champ vide est prérempli avec `/btw btw-N `,
curseur à la fin. Effacer ce préfixe permet de revenir au fil principal. Un
brouillon déjà saisi pendant l'attente de l'acceptation n'est pas remplacé.
La création et la continuation fonctionnent aussi une fois le principal terminé.
Une branche créée au repos apparaît à la suite du fil, avec sa flèche alignée
sur les messages classiques. Elle conserve le contexte principal figé, mais son
instruction système BTW n'annonce pas de tâche principale en cours. Sa position
et cette distinction sont conservées après `/resume`. Une conversation principale
vide ne permet pas de créer un BTW. Un envoi dans une branche encore occupée
est refusé en conservant le brouillon.
Au premier envoi d'une branche, le message utilisateur principal est placé en
haut du chat pour montrer son contexte ; pour une branche créée au repos, c'est
l'en-tête du panneau BTW. Aux envois suivants, c'est le dernier
message utilisateur de la branche qui est placé en haut. Les réponses reçues
ne forcent pas un retour à cette position après un défilement manuel.

Une nouvelle branche replie les autres panneaux ; un nouveau message principal
les replie tous, sans arrêter leurs générations. Double Échap annule uniquement
le principal. Il n'y a pas d'annulation manuelle du BTW dans l'app ; fermer ou
déconnecter la TUI interrompt ses branches et sauvegarde les résultats déjà reçus.

Chaque branche garde une copie figée du contexte actif principal à sa création,
images comprises, plus son propre historique. Rien n'est réinjecté dans le fil
principal. Le modèle et la réflexion sont ceux sélectionnés au lancement de chaque
échange. Le daemon n'autorise que `read`, `glob`, `grep`, `view_image` et `webfetch`,
jamais Bash ni les outils d'écriture, même en Slop/Danger. Les lectures externes
au workspace autorisé sont refusées. Les messages BTW saisis sont textuels.

Contexte, messages, outils, métadonnées et état des branches sont persistés dans
`session.sqlite` (schéma 5). `/resume` restaure les panneaux et permet de continuer
les branches ; les générations interrompues ne reprennent pas automatiquement.
Le contexte BTW n'est pas compacté automatiquement dans cette version : un
dépassement de fenêtre est signalé dans la branche. Le protocole passe en **7** :
reconstruire TUI et daemon ensemble, puis fermer les anciennes intégrations.

Au premier lancement, utiliser `/connect` : OpenRouter par clé API ; OpenAI par
clé API, ChatGPT/Codex via navigateur ou code appareil. Le bouton « Open browser »
ouvre l'URL d'autorisation et reste actif pendant le polling OAuth. L'URL est un
lien cliquable, coloré et souligné, également accessible au clavier par Tab puis
Entrée ; elle reste affichée pour une ouverture sur une autre machine.
La connexion navigateur utilise un callback local sur le port 1455 ;
le code appareil ne nécessite pas ce callback. Échap ou Cancel depuis l'étape
méthode/clé annule l'opération en cours, efface ses champs et revient aux providers.
Échap ou Cancel depuis la liste des providers ferme la popup.

Les clés saisies sont masquées et sauvegardées par le daemon dans
`$XDG_DATA_HOME/sos/auth.json` (par défaut `~/.local/share/sos/auth.json`), permissions
`0600`, hors conversations. Le fichier contient les clés ou tokens en clair.
Les tokens OAuth sont renouvelés automatiquement. Après authentification, le
premier modèle compatible du dernier provider connecté avec succès est choisi :
OpenRouter demande `sort=newest`, OpenAI API trie par `created` décroissant et
Codex trie par `priority` croissant, comme le client officiel. Ce choix est déterministe et
mémorisé entre les sessions. La popup n'affiche aucun champ modèle ; le modèle retenu
apparaît dans la barre d'informations de l'interface. `/connect` demande une nouvelle
authentification qui remplace celle enregistrée pour le provider. OpenRouter ouvre
directement la saisie de clé, sans choix de méthode, même s'il est déjà connecté.
La popup affiche `Checking API key…` avant l'enregistrement : OpenRouter est vérifié
avec `GET /api/v1/key`, OpenAI avec `GET /v1/models`. Aucun appel de génération
n'est effectué. Un refus, une erreur réseau ou une annulation conserve l'ancienne
clé. Ce contrôle valide l'authentification et l'accès à l'endpoint, pas le crédit
disponible ni l'accès à tous les modèles. L'authentification utilise exclusivement `auth.json`, sans fallback
vers des clés dans l'environnement.

Plusieurs providers peuvent être connectés simultanément, avec une seule
authentification enregistrée par provider. Une reconnexion remplace entièrement
l'ancienne authentification de ce provider, en conservant les autres. Le badge
`Connected` est affiché sur chaque provider connecté et sur la ligne de sa méthode
active lorsqu'un choix de méthodes est proposé, jamais sur les titres ou la saisie
de clé. Pour OpenRouter, seul le provider porte cette indication.
La dernière méthode est mémorisée et présélectionnée, sans lancer la connexion.
Les anciennes entrées OAuth sans méthode précise marquent uniquement le provider ;
une reconnexion permet d'identifier la ligne navigateur ou code appareil sans la deviner.
`Maj+Tab` ou `←` depuis la liste des méthodes revient aux providers. Sur les boutons,
`←` et `→` parcourent les actions visibles et activées ; dans la clé, elles déplacent
le curseur. La bordure du champ reste muted, y compris au focus.
Le catalogue OpenRouter est filtré sur texte en entrée/sortie et support des outils.
L'accès Codex utilise son catalogue dédié et ses modèles visibles. L'API OpenAI
ne fournit pas les capacités dans `/models` : le filtre conserve les familles
Responses connues et leurs snapshots, en excluant les modèles audio, embeddings
et les alias chat-only. Un échec de catalogue garde la conversation actuelle et
permet de réessayer.

Valider le provider démarre une nouvelle conversation ; l'ancienne reste dans
`/resume`. Le choix provider/modèle/réflexion est sauvegardé dans `$XDG_CONFIG_HOME/sos/config.json`
(par défaut `~/.config/sos/config.json`), séparément des credentials. Les sessions
conservent leurs métadonnées historiques, mais reprennent avec le choix global courant.
La TUI se connecte à `/tmp/sosd.sock` ou démarre automatiquement son daemon
embarqué si aucun ne répond. `--socket /autre/chemin.sock` permet d'utiliser
un autre socket. Aucune session n'est créée avant le premier message.
Le daemon est partagé tant qu'au moins une intégration reste connectée. La TUI
annonce sa fermeture ; après le départ de la dernière intégration et les
sauvegardes en cours, le daemon quitte et retire son socket. Il envoie aussi
un ping toutes les 15 secondes et ferme les connexions sans pong sous 30 secondes.
La réponse est automatique, y compris pendant l'utilisation de `$EDITOR`.
Il hérite de l'environnement de son lanceur ; un changement de clé nécessite son
redémarrage. Fermer toutes les intégrations puis relancer `sos` suffit.
Les démarrages simultanés sont sérialisés par un verrou. Les logs du daemon sont
dans `$XDG_STATE_HOME/sos/sosd.log` ou `~/.local/state/sos/sosd.log`.
Pour `uv run tui`, construire `build-tui` au préalable afin de disposer de la
copie de développement de `sosd` dans le package.

L'onglet « Discussion » occupe une ligne en haut. La dernière ligne affiche le
workspace à gauche et le titre reçu du daemon à droite. Avant la création de la
session, le titre affiché est `-`. Le daemon génère un titre LLM de 6 mots maximum
à partir du premier message, puis le renvoie avec `session.created`.
La TUI affiche chaque message envoyé et son volet Activity animé sans attendre
l'acceptation du daemon, y compris le premier pendant la préparation du titre.
L'écho du message confirme cet affichage
sans le dupliquer ni redémarrer le chronomètre. En cas d'échec, d'annulation ou de
déconnexion avant acceptation, cet affichage provisoire est retiré et le texte
est restauré dans le composer, devant le nouveau brouillon éventuel.
La palette se trouve dans `src/sos_tui/config.py`.

À l'envoi, la question s'aligne en haut du chat si elle tient à l'écran ; sinon la
vue descend jusqu'à sa fin pour rester en suivi. Les échanges précédents restent
accessibles en remontant. Le dernier tour dispose au minimum de la hauteur du chat :
s'il tient à l'écran, le scroll s'arrête avec la question en haut et du vide dessous ;
s'il est plus long, on peut descendre jusqu'au bas du contenu, activités dépliées
comprises. Cet espace s'adapte au contenu visible et à la taille du terminal.
Chaque texte assistant, intermédiaire ou final, déclenche le suivi si l'utilisateur
attendait au bas de la conversation. Les activités ne déplacent pas la vue, mais
leur croissance ne coupe pas cette attente : le prochain texte rejoint la nouvelle
limite basse. Remonter interrompt le suivi, y compris pendant le montage d'une
réponse ; revenir en bas permet de le reprendre. L'éviction des anciens widgets
conserve le point de lecture lorsqu'il est encore dans la fenêtre.

Pendant un tour en cours, Entrée place le texte dans un unique prochain message
local, sous tout le bloc assistant en cours, avec `[Pending]` en bleu clair
(`MISC_MD`) au-dessus. Les nouveaux textes, activités et permissions s'insèrent
avant ce bloc. Les envois suivants complètent le même message avec un saut de
ligne entre chaque ajout, en vidant la saisie pour permettre un nouveau brouillon.
Le bloc ne compte pas comme un message d'historique et reste conservé lorsqu'on
pagine ; il est visible sur la page la plus récente.

Seul `execution.completed` déclenche son envoi, pas un texte intermédiaire ni
une compaction automatique. Le mode Sane/Slop utilisé est celui du départ effectif.
Le message devient alors un tour utilisateur normal, sans effacer le brouillon
que l'utilisateur a commencé depuis, même si ce brouillon est dans `$EDITOR`.
Les commandes `/…` ne sont pas mises en queue.

Pendant un tour accepté, les erreurs temporaires de connexion, de transport ou
de stream du modèle déclenchent jusqu'à dix retries : immédiatement, puis après
2, 5, 10, 15 secondes, puis 15 secondes pour les tentatives restantes. Le retry
concerne uniquement l'appel modèle courant, jamais les outils déjà exécutés.
Une ligne en gras rouge Slop avant Activity affiche par exemple
`Connection problem · Retry 2/10 · Retrying in 2s`, avec un décompte chaque seconde,
puis `Reconnecting…`. Elle disparaît après récupération. Échap permet toujours
d'annuler pendant l'attente. Après dix retries échoués, le spinner s'arrête et
l'erreur finale apparaît sous Activity ; le message envoyé reste dans le chat,
sans revenir dans le composer. Les erreurs permanentes ne sont pas retentées.
Les titres, recaps, compactions et déconnexions entre TUI et daemon ne sont pas
concernés par ce mécanisme. Les notices de retry et d'échec sont affichées en direct,
sans ajout au contexte modèle ni à l'historique persistant.

La lecture du catalogue bénéficie aussi des retries : une panne réseau affiche
`Connection problem`, pas une erreur de support image. Un catalogue récupéré sans
déclaration de capacité reste bloquant par prudence ; un modèle explicitement
sans images est refusé sans retry. Si le catalogue reste inaccessible avant
l'acceptation du message, le brouillon et ses images sont restaurés, car le
message n'a pas encore été envoyé ni enregistré dans la session.

Si un pending existe, hors confirmation de sortie active, un seul Échap le retire
et remet son texte devant le brouillon actuel, séparé par un saut de ligne. Cet
appui est consommé et remet la confirmation d'interruption à zéro : il faut deux nouveaux Échap pour arrêter
l'exécution. Une erreur d'exécution, une annulation ou une déconnexion restitue
également le texte, sans relance automatique. Cette queue appartient uniquement
à la TUI en mémoire ; elle n'est pas sauvegardée avant son envoi.

`/recap`, dans une session au repos, génère un court repère de lecture pour
l'utilisateur : objectif courant, progrès récents et prochaine étape si connue.
L'appel est séparé, sans outils, avec le modèle et le niveau de réflexion
sélectionnés. Il demande un paragraphe de deux phrases courtes maximum et au plus
45 mots, dans la langue de l'utilisateur : ce qu'on fait concrètement, à quoi cela
sert et ce que l'utilisateur peut essayer ou ce qui vient ensuite. Les résultats
de tests, builds et détails internes sont omis, sauf s'ils sont le sujet principal
ou un blocage qui exige son intervention (les retours visuels dépendent de la largeur du
terminal). La source est le texte récent du contexte actif, limité à 24 000 octets
UTF-8, sans sorties brutes d'outils ni reasoning ; un checkpoint de compaction
peut fournir le contexte antérieur.

Chaque résultat ajoute une carte « Conversation Recap » : titre gras et bandeau
gauche en `MISC_MD` (`#81D8D0`), fond `CONTENT_BACKGROUND_COLOR` (composer atténué),
contenu en texte brut. Les cartes sont conservées dans SQLite et retrouvées au
même endroit via `/resume` et la pagination. Elles ne sont jamais injectées dans
le contexte de l'agent, les prochains recaps ou la compaction. Plusieurs recaps
peuvent coexister sans se remplacer.

La carte apparaît dès le lancement, avec son titre et son bandeau MISC : un
spinner occupe la place du texte, puis le résultat le remplace dans la même carte.
Seul ce spinner s'anime, sans activer la barre de chargement du bouton en bas à
droite. Échap deux fois annule la génération. Une erreur, une annulation ou une
déconnexion retire la carte provisoire, sans l'enregistrer. Le brouillon reste
éditable ; un message explicitement mis en queue part après la réussite, ou est
restitué en cas d'échec. Il apparaît sous la carte, dont l'emplacement reste réservé
pendant la génération. Le recap ne relance pas l'agent de lui-même et ne modifie
pas les compteurs du dernier tour.

La TUI déclenche aussi un recap après **cinq minutes continues sans envoi de
message et sans travail en cours**, dans une session ouverte et connectée. Le
compte à rebours est local à cette TUI, basé sur une horloge monotone et vérifié
chaque seconde : ni le temps passé TUI fermée, ni l'ancienneté de la session ou
du daemon ne comptent. Une exécution, une compaction ou un nouvel envoi remet
le délai à zéro ; une longue génération exige donc cinq nouvelles minutes au
repos après sa fin. Changer de session ou rouvrir la TUI repart également de zéro.
Les chargements, popups et l'éditeur externe suspendent le comptage, qui repart
ensuite pour cinq minutes complètes. Écrire un brouillon sans l'envoyer ne remet
pas le délai à zéro ; le déclenchement automatique préserve son texte et sa sélection.

Un seul recap est demandé par période d'inactivité, y compris si la requête échoue
ou est annulée : pas de répétition automatique toutes les cinq minutes sur une
discussion inchangée. Un `/recap` manuel satisfait aussi cette période. Un nouvel
envoi ou un nouveau travail réarme le délai, mais **l'automatisme reste bloqué si
le dernier élément de l'historique affichable est déjà un Conversation Recap**.
Cette information vient de l'index SQLite du daemon, indépendamment de la page
consultée : fermer puis rouvrir la TUI ou reprendre avec `/resume` ne permet donc
pas d'ajouter un deuxième recap automatique à la suite. Les événements système
invisibles ne changent pas cette dernière entrée affichable. `/recap` manuel
reste disponible à la demande, même après un autre recap. Aucun timer de recap
ne tourne dans le daemon.

`/compact`, dans une session au repos, résume les anciens échanges pour réduire
le contexte envoyé au modèle. La discussion complète reste affichée ; un volet
repliable « Compaction », en gras rouge SLOP avec un trait rouge épais au-dessus,
expose le résumé. Son spinner apparaît dès le lancement. Les messages
récents conservés dans le contexte ne sont pas affichés une seconde fois. `/resume`
restaure tout l'historique, y compris les compactions. Le bouton indique une
activité pendant la génération ; Échap deux fois permet de l'annuler. Un échec ou
une annulation garde le contexte précédent. Un `/compact` manuel ne reprend pas
le travail de lui-même ; si l'utilisateur a explicitement mis un message en queue
pendant la compaction, celui-ci est envoyé après sa réussite.

Lorsque les limites du modèle sont connues et permettent la réserve de sécurité,
le daemon compacte aussi automatiquement avant un appel modèle trop proche du
plafond. Le même volet apparaît, le tour reste en cours et le travail reprend après
le résumé, en conservant le tour courant et ses appels/résultats d'outils. Échap
annule également cette génération. Les brouillons restent éditables et ne sont
pas envoyés automatiquement.

Les textes de réponse de l'assistant sont rendus en Markdown : `TITLE_MD` colore
les titres, `BOLD_MD` le gras, `DEPTH_MD` les liens et `MISC_MD` le code inline,
l'italique, les puces et les citations. Les blocs de code utilisent notre table de
coloration dans `src/sos_tui/response_markdown.py` : chaînes en `ALGAE_MD`, mots-clés
en `BOLD_MD`, fonctions/types/nombres en `TITLE_MD`, identifiants/commentaires en
`DEPTH_MD` et opérateurs en `MISC_MD`. Les autres tokens et langages inconnus utilisent
`TEXT_COLOR`, sans repli vers la palette native. Le fond du code utilise
`CODE_BACKGROUND_COLOR` (`#1E2226`), mélange précalculé à 25 % de `#303A48` sur
`#181B1B`. `INPUT_BACKGROUND_COLOR` est réservée au composer.
Les messages utilisateur restent en texte brut. Le contenu des volets Reasoning
utilise le même rendu Markdown et la même palette que les réponses.

`read` affiche uniquement une ligne `read · chemin` en sable doré (`TITLE_MD`), sans volet ni résultat
détaillé ; une erreur reste affichée si la lecture échoue.
Les autres outils ont un volet replié avec un titre corail sur le fond du chat.
Il apparaît à l'annonce de l'appel, puis son résultat complète le même volet via
`callId`. Les sections sont nommées « Arguments » et « Results ». Les résultats
sont mis en forme à la première ouverture : chemins/lignes pour `glob` et `grep`, sortie et code de retour pour `bash`,
diffs transmis par les outils pour `write`/`edit`/`apply_patch`, et rendu adapté au
format demandé pour `webfetch`. Les erreurs utilisent `SLOP_COLOR`. Les anciens
résultats sans métadonnées et les outils inconnus restent lisibles en texte brut.
Cette présentation est définie dans `src/sos_tui/tool_view.py`.

Reasoning et outils sont regroupés dans des volets d'activité fermés par défaut,
en suivant l'ordre des contenus reçus. Le premier volet contient l'activité entre
la requête utilisateur et le premier texte de réponse. Après chaque texte visible,
l'activité suivante ouvre un nouveau volet à sa suite, avant le texte suivant.
Les résultats d'outils complètent leur appel dans son volet d'origine. Ce découpage
est aussi appliqué lors de la reprise d'une session, entièrement côté TUI.
Le libellé `▸ Activity` en `MUTED_COLOR` précède les carrés colorés, puis le spinner,
le nombre d'entrées (`1 step`, `12 steps`), un `|`, la durée et le débit si connu :
`Activity {carrés} {spinner} 12 steps | 23s - 42 tok/s`. Le compteur et son séparateur
sont absents sans entrée. Un raisonnement ou un appel d'outil compte pour une entrée ;
son résultat et son éventuel refus ne le comptent pas une seconde fois. Le compteur
est restauré avec l'historique, sans charger les détails du volet.
Cliquer dessus révèle l'activité en dessous, décalée de deux colonnes à droite,
et remplace la flèche par `▾`. Un second clic la masque.
La réponse suit immédiatement Activity, sans séparateur ni ligne vide,
que le volet soit fermé ou ouvert.
Pendant l'exécution, le tiret tourne (`-`, `\`, `|`, `/`) grâce au timer du bouton
pulsant, sans timer supplémentaire. Les petits carrés signalent les éléments reçus :
corail pour les outils autres que `read`, lagon pour le reasoning et sable doré (`TITLE_MD`)
pour les lectures. Ils restent après la fin du tour. Les titres des volets de
reasoning utilisent `MISC_MD`, ceux des outils `BOLD_MD` ; `read` utilise `TITLE_MD`.
Les clics sur les titres des volets ne déplacent pas le focus clavier : le composer
le conserve, sans aller-retour de focus ni recalcul des styles de tout le volet.
Les titres restent focalisables pour leurs commandes clavier. Les événements
continuent à remplir le volet fermé et les toggles internes conservent leur état.
Le message utilisateur et les réponses restent visibles. Le composant est dans
`src/sos_tui/turn_activity.py`.
À droite des carrés, du spinner et du compteur, Activity affiche la durée et le débit.
Les activités intermédiaires conservent leur compteur mais n'ont pas de spinner.
Le modèle et l'effort restent au pied des réponses. Cela vaut aussi après reprise de l'historique ;
une durée absente des anciennes métadonnées n'est pas inventée.
La durée évolue depuis l'envoi grâce au même timer que le spinner, puis se fige sur
la mesure du daemon enregistrée dans le message final. Les formats sont `12s`,
`2m 08s`, puis `1h 05m` (sans passage aux jours). Les anciens tours conservent leurs
propres modèle, effort et durée.

Tab bascule entre Sane (par défaut, bleu-vert) et Slop (rose). Le mode est transmis
au daemon, qui applique les permissions et sauvegarde le mode dans la session,
y compris lors d'un changement sans nouveau message. `/resume` restaure ce mode ;
les anciennes sessions sans mode enregistré utilisent Sane.
La bande colorée à gauche de la saisie indique le mode actuel et suit
sa hauteur, avec une colonne vide avant le texte et le fond du composer conservé ; chaque message
utilisateur conserve la couleur du mode au moment de son envoi, sur une bande
d'une colonne suivie d'un espace. Son fond utilise `CONTENT_BACKGROUND_COLOR`
(`#242A31`), mélange précalculé à 50 % de `#303A48` sur `#181B1B`.
Les réponses du modèle sont alignées à gauche, sans label de rôle.

En Sane, les outils d'écriture, d'édition et de patch sont interdits, ainsi que
les commandes bash classées modificatrices ou inconnues. En Slop, ces opérations
affichent une carte de permission dans le chat, sous l'activité du tour, à la
position de la prochaine réponse. Les accès aux dossiers externes détectés
demandent aussi une permission dans les deux modes. Seul le workspace de lancement
est autorisé initialement, même après la reprise d'une session d'un autre projet.

La carte propose, dans cet ordre, `Yes`, `Always yes`, `No`, sur une seule ligne
de boutons compacts. Elle garde le fond du chat, un titre jaune « Allow … » et une
bande jaune d'une colonne sur toute sa hauteur, séparée du contenu par un espace,
sans cadre ni texte explicatif de portée. La commande, le working directory,
les chemins et le diff éventuel restent affichés.
À l'arrivée d'une demande, la vue se déplace si nécessaire pour rendre le bouton
`Yes` visible, puis lui donne le focus après le rendu. Une nouvelle tentative fait
de même pour le choix précédent. Si un bouton focalisé est ensuite sorti de l'écran
par le scroll, Entrée le réaffiche sans envoyer de décision ; il faut valider à
nouveau une fois le choix visible.
Après résolution elle disparaît, et l'outil dans Activity reçoit
` -> Yes`, ` -> No` ou ` -> Always yes`. Plusieurs décisions peuvent accompagner
un même appel, par exemple accès externe puis écriture. Ces traces sont restaurées
avec l'historique. `No` annule le tour comme Échap, sans relancer les outils suivants.
L'attente reste annulable et les heartbeats continuent de fonctionner.

`Always yes` vaut uniquement pour la connexion actuelle : dossier et descendants,
toute la famille d'édition (`write`, `edit`, `apply_patch`) ou portées bash déduites
des commandes. L'accès au dossier de lancement et à ses descendants est déjà
autorisé, mais ne vaut pas autorisation d'éditer ou d'exécuter une commande
modificatrice. L'accord reste valide après
`/new` et `/resume`, mais disparaît à la fermeture de la TUI. Il ne permet jamais
de contourner une interdiction Sane. L'analyse bash est une politique de permissions,
pas une isolation système des effets d'un programme.

### Confiance Jev

Dans `/permissions`, le réglage `How confident should Jev be before running an action?`
apparaît directement sous la liste des trois modes uniquement quand Jev porte le badge
`Current` et que sa ligne est surlignée. Sur Ask ou Danger, le bloc et ses boutons
sont masqués ; revenir sur Jev restaure le brouillon. Il n'y a plus d'écran séparé pour
le pourcentage. Entrée sur Jev utilise directement la clé OpenRouter enregistrée.
Sans clé, le formulaire de saisie masquée apparaît avec `Cancel` ; une fois la clé
validée, Jev devient actif et la liste réapparaît sans fermer la popup.
Il n'y a plus de bouton `Use saved key`.
Juste sous la question, un champ entier 0–100 court, sur une ligne et sans bordure,
règle la confiance minimale pour exécuter ; les flèches gauche/droite restent disponibles.
Seules les explications 0 % et 100 % figurent à droite, sans presets ni texte supplémentaire.
`Save` (ou Entrée dans le champ) ferme la popup après confirmation de l'enregistrement
du seuil. En cas d'erreur, elle reste ouverte avec le brouillon. `Change API key` ouvre le formulaire
de remplacement de clé ; Échap ou Cancel revient à la liste sans enregistrer sa saisie.
Échap depuis la liste ferme la popup sans sauvegarder un pourcentage encore en brouillon.
Le seuil est global, persistant et applicable aux prochains contrôles du tour courant.

La confiance est toujours celle d'exécuter. Un `deny` à 98 % devient 2 % ; ce même
score sert à décider et à afficher. Par défaut, le seuil est 80 %. À 0 %, aucun
appel Jev n'est effectué, comme Danger, mais Sane reste prioritaire. Le bouton
`Skip Jev` a été retiré ; saisir 0 colore l'explication 0 % à droite en alerte
Slop avant Save, sans ajouter de ligne. À 100 %, il
faut un score de 100 %, sans que ce score constitue une garantie de sécurité.

Dans `Refused tools`, les lignes affichent `bash • Confidence 2%` en Slop,
sans suffixe de refus. Après `Approve`, ` -> Manually approved` s'ajoute et toute
la ligne devient verte. Le score historique est conservé. Les anciens refus et
les échecs sans score affichent `Confidence unavailable`, jamais un zéro inventé.
L'historique inférieur reste immuable.

Au prochain envoi, un message système fournit au modèle l'action exacte et un ID
de rejeu. `rerun_approved_action` n'accepte que cet ID : outil, arguments, répertoire
et workspace sont retrouvés côté daemon, sans Jev. Une révocation est vérifiée
avant chaque frontière de permission ; Sane et l'annulation restent prioritaires.
Le clic d'approbation n'exécute rien. Les accords restent réutilisables dans cette
session et survivent à `/resume`. Activity montre le véritable outil exécuté et
ses arguments, avec `-> Manually approved`.

La ligne au-dessus du composer affiche le modèle et l'effort de reasoning à gauche,
`contexte du dernier appel / capacité du modèle` au centre, puis un bouton de
largeur fixe à droite, sans action pour le moment. Son texte centré passe de
« Sane implementation » à « Slop » et son fond suit le mode.
Pendant une requête, le bouton affiche « Cancel » et le fond sombre avance depuis la gauche
jusqu'à ne laisser qu'une colonne colorée à droite, puis recule. La largeur du
bouton ne change pas. Le texte revient après succès, erreur ou déconnexion ; son timer
est suspendu au repos. Le rendu se trouve dans `src/sos_tui/mode_button.py`.
Le rapport est suivi du pourcentage entre crochets, arrondi à l'entier le plus
proche (`[13%]` pour 12,5 %). Quand la largeur diminue, les champs disparaissent
dans cet ordre : rapport, effort de reasoning, pourcentage, modèle. Le bouton
reste prioritaire. Les champs réapparaissent quand la largeur augmente.
Ces informations viennent du daemon ; une valeur de contexte inconnue apparaît
comme `—`. Le compteur est la somme des tokens d'entrée et de sortie du dernier
appel de discussion, pas un cumul des appels ni un comptage des ajouts ultérieurs.
L'effort initial vient de `DEFAULT_REASONING_EFFORT` côté daemon. Ctrl+T demande
le niveau supérieur parmi ceux exposés par le provider, à partir de `low`, puis
revient au minimum après le maximum. Les niveaux non supportés sont sautés ;
en l'absence de niveaux disponibles, une erreur est affichée sans changer le réglage.
Ce choix est global et sauvegardé immédiatement dans `config.json`, même avant
le premier message. Il reste identique après `/new`, `/resume` et redémarrage.
Pendant une génération, il s'applique au prochain message :
la barre affiche le nouveau choix, Activity conserve celui du tour en cours.
Le reasoning disponible est replié dans
« ▸ Reasoning », ouvrable au clic.

Entrée envoie le message ; Maj+Entrée insère un retour à la ligne. La TUI accepte
les encodages Kitty/CSI-u, xterm et LF (notamment Zed). Ctrl+J insère également
un retour à la ligne. Sur Unix, elle active `modifyOtherKeys` avant le protocole
Kitty de Textual, sans modifier la configuration utilisateur. Ces modes sont
quittés avant l'éditeur externe ou le shell, puis réactivés à la reprise.
Les retours à la ligne et l'envoi sont traités dans la même file que les caractères
du champ, pas comme des raccourcis prioritaires : si plusieurs touches arrivent
dans un seul paquet du terminal, le texte précédent est inséré avant le retour
à la ligne ou l'envoi. Tab respecte aussi cet ordre avant de choisir une suggestion.
Un terminal ancien qui envoie le même code pour Entrée et Maj+Entrée sans
protocole étendu reste indiscernable ; Ctrl+J sert alors d'alternative.
La saisie grandit de 1 à 6 lignes, retours visuels
compris, puis défile. Elle prend cet espace sur le chat.
Un collage d'au moins 3 lignes ou 200 caractères devient un bloc compact jaune
`[les trois premiers mots…]`, sans numéro ni compteur. L'aperçu est limité à
24 colonnes, points de suspension compris (hors crochets). Les petits collages
restent du texte normal. Plusieurs blocs, même avec le même aperçu, conservent
chacun leur contenu original. Les flèches passent d'un côté à l'autre du bloc ;
Retour arrière/Suppr le retirent en une fois. Annuler/rétablir restaure aussi son
contenu. Un clic ouvre une popup avec le texte intégral, `Close` et `Remove`.
L'envoi, la mise en attente et la copie utilisent le contenu original, jamais
l'aperçu. L'éditeur externe reçoit aussi le texte intégral : s'il ne le modifie
pas, les blocs restent repliés ; s'il le modifie, le résultat revient en texte
normal. Un texte rétabli après un envoi refusé revient intégralement en clair,
sans altérer les blocs du nouveau brouillon déjà saisi.
Au repos, `/undo` retire le dernier tour utilisateur + IA et restaure les fichiers
concernés ; le message utilisateur et ses images reviennent dans la saisie.
`/redo` rétablit le tour et les fichiers sans relancer les outils ni le modèle.
Un nouvel envoi abandonne la branche redo. Les changements de fichiers capturés
dans Git incluent ceux de Bash ; pour les fichiers ignorés/hors dépôt et les
workspaces sans Git, seuls `write`, `edit` et `apply_patch` sont journalisés.
Un conflit bloque l'opération et laisse le brouillon et l'historique affiché
intacts. Les tours annulés ou en erreur sont couverts, sauf lorsqu'un crash brutal
a empêché de vérifier leur état final. Voir [les limites](../../docs/git-snapshots.md).

Un `/` en début de saisie affiche les commandes au-dessus du composer, par ordre
alphabétique : `/btw`, `/compact`, `/connect`, `/model`, `/new`, `/permissions`, `/recap`, `/redo`, `/resume`, `/undo`. La liste est
filtrée par préfixe (`/mo` ne montre que `/model`). Aucune suggestion n'est
sélectionnée initialement, même s'il n'en reste qu'une. Haut/Bas choisissent une
suggestion sans quitter la saisie ; Tab sélectionne la première s'il n'y a pas de
sélection, puis descend et revient à la première après la dernière. Maj+Tab fait
l'inverse : dernière suggestion sans sélection, puis remontée avec bouclage.
Cela vaut pour les commandes, les skills et les citations. Lorsque cette
liste est visible et la saisie a le focus, Tab et Maj+Tab ne changent pas le mode.
Hors suggestions, Maj+Tab conserve la navigation vers le contrôle précédent.
Chaque choix par clavier ou clic remplace uniquement le mot au curseur par
la commande ou le skill. La liste conserve les propositions du préfixe initial pendant la
navigation, sans se refiltrer sur le nom inséré. Une touche d'édition ou de
déplacement du curseur ferme cette liste et conserve le texte choisi.
Entrée soumet le texte présent dans la saisie. Pour les skills, seul `/nom` est
inséré : le `argument-hint` reste une indication grisée, jamais envoyée.
Un skill peut être invoqué sans argument ; le modèle demande les précisions nécessaires.
Les remplacements sont annulables ; aucune navigation ne déclenche d'envoi.
Une commande inconnue, ou des arguments sur une commande qui n'en accepte pas,
affichent une notification d'erreur locale : le brouillon reste éditable et rien
n'est envoyé au modèle. Un slash au milieu d'un message ordinaire reste du texte.
Un `>` seul sur une ligne de la saisie propose les paragraphes du dernier message
de l'assistant : chaque ligne non vide est un choix, y compris chaque bullet,
indépendamment des retours visuels dus à la largeur du terminal. Le premier choix
est sélectionné par défaut ; Tab avance et boucle, Maj+Tab recule et boucle,
Haut/Bas changent de choix. Dès qu'on navigue ou clique, le texte `> texte` remplace
directement la ligne courante, sans toucher aux lignes autour. La liste reste
ouverte pour choisir une autre citation.
Entrée valide la citation et place le curseur juste dessous, sans envoyer le
message. Les touches de déplacement ou de suppression quittent aussi la navigation
en passant dessous, sans supprimer la citation. Écrire, taper Espace, coller ou insérer un saut de ligne
valide également la citation puis applique cette saisie sur la ligne dessous.
Un saut de ligne ajoute donc une ligne vide après la citation. Sans source à citer,
continuer à écrire retire simplement le marqueur. Les choix et leur validation
ont des étapes d'annulation séparées et préservent le
reste du brouillon. Le texte cité reste du texte Markdown ordinaire à l'envoi et
dans les sessions enregistrées. Dans les messages utilisateur affichés, les lignes
citées ont un fond plus clair que le composer (`#445266`),
avec une demi-barre `▌` en MUTED à gauche,
en plus de la bande Sane/Slop. Leur texte est en TEXT_COLOR, sans italique ; la
question qui suit retrouve le style normal.
Le composer affiche la même décoration, y compris sur les lignes repliées à
l'écran, avec une colonne d'espace entre la bande Sane/Slop et la barre de citation.
Le texte normal dessous commence à la même colonne que cette barre.
Deux espaces séparent la barre du texte cité, dans le composer comme après envoi,
y compris sur les lignes repliées.
Le préfixe `>` reste dans le texte éditable et envoyé ; la décoration
ne change ni la copie, ni la sélection, ni l'annulation.
La suppression dans une citation retire tout le bloc de lignes `>` consécutives,
avec son retour final s'il existe, sans toucher au texte dessous. Backspace,
Suppr, Ctrl+W et Couper respectent ce bloc atomique ; annuler le restaure.
Ctrl+W supprime le mot précédent dans le texte normal, y compris avec les
encodages clavier étendus des terminaux.
Les clics sur le contenu et les titres du chat conservent le focus courant. Les
autres interactions rendent le focus au composer lorsque la saisie est active,
sauf pour les boutons de permissions. Une popup conserve le focus pendant son
ouverture, notamment pour la navigation au clavier dans `/resume`.
Au retour dans le terminal (événement FocusIn, notamment après Alt+Tab), la saisie
récupère le focus avant la prochaine touche s'il n'appartient pas à une popup ou
à un bouton d'autorisation. Ce retour ne déplace ni le curseur du brouillon ni le
défilement du chat. Aucun refocus n'est forcé pendant que la fenêtre est inactive,
que l'éditeur externe est ouvert ou que la saisie est désactivée. Si Zed redonne
le focus à son éditeur plutôt qu'au terminal intégré, la TUI ne peut pas le reprendre
à sa place : ces événements et les touches doivent d'abord parvenir au terminal.
Dans la discussion, lorsqu'une demande d'autorisation est présente, Gauche/Droite
pilotent ses choix même si le focus a quitté les boutons. La navigation reprend
au dernier choix, revient sur la carte et ne déplace pas le curseur du brouillon.
Après une réponse, ces touches restent réservées jusqu'à la résolution de la
demande, sans pouvoir répondre deux fois. Les autres touches de saisie ne sont
pas interceptées. Les popups et l'éditeur externe gardent leur propre navigation.
Sans demande, Gauche/Droite retrouvent leur comportement normal.
Sélectionner du texte dans la discussion ou une popup le copie automatiquement
au relâchement de la souris, puis retire le surlignage. Dans le brouillon, le
texte sélectionné est aussi copié, mais la sélection reste disponible pour
l'édition. Un clic sans sélection ne remplace pas le presse-papiers ; les champs
de mot de passe ne sont pas copiés automatiquement.
Chaque copie déclenche un badge `Copied` de trois lignes en bas à gauche, juste
au-dessus de la barre modèle/réflexion (en bas de l'écran dans une popup), sans
cadre : une bande dorée à gauche sur toute la hauteur, le fond de la saisie et
le texte centré verticalement entre deux lignes vides, dans le jaune de
la palette (`TITLE_MD`). Il disparaît après 1,4 seconde ; une nouvelle copie
relance ce délai sans empiler de badges, déplacer le contenu ou prendre le focus.
La copie utilise OSC 52 et, localement, le presse-papiers natif disponible
(`pbcopy` sur macOS, `wl-copy`/`xclip`/`xsel` sur Linux, PowerShell sur Windows),
sans installer d'outil ni modifier les réglages du terminal. En SSH, seul OSC 52
est utilisé pour viser le terminal client. Son acceptation dépend du terminal
et des éventuels multiplexeurs. Aucune infobulle n'est affichée dans la TUI.
Un clic dans le texte ou une zone vide de l'historique ne donne pas le focus au
conteneur : cela évite de recalculer les styles de tous ses descendants. La sélection
de texte, les liens, les volets et le défilement restent actifs.

Ctrl+C vide la saisie si elle contient du texte, sans préparer la sortie. Si elle
est vide, un premier appui remplace toute la ligne modèle/réflexion/contexte/mode
par une barre couleur Slop (`SLOP_COLOR`) avec `Exit ? Confirm with CTRL-C` centré. La barre se vide
en trois secondes, à partir de la même échéance que le
raccourci. Un second Ctrl+C dans ce délai quitte la TUI ; sinon, les informations
actualisées réapparaissent. Reprendre la saisie ou envoyer un message annule cette
confirmation. La ligne conserve sa hauteur et le compte à rebours ne redémarre
pas lors d'un redimensionnement ou d'une mise à jour des informations. Pendant ce
délai, Échap est ignoré : il ne ferme pas la confirmation de sortie, ne retire pas
le pending et n'arme pas l'interruption. Il retrouve son comportement normal à
l'expiration du délai.
Les confirmations de sortie et d'annulation d'exécution restent distinctes :
Échap (ou le bouton Cancel) ne confirme jamais une sortie, et Ctrl+C ne confirme
jamais une annulation. Ctrl+Q conserve sa sortie directe.
Ctrl+G ouvre le brouillon dans `$EDITOR`, avec ses arguments éventuels. Si cette
variable n'est pas définie, la TUI affiche une erreur et conserve le brouillon.
La TUI reprend à la fermeture de l'éditeur et replace le
texte sauvegardé dans le composer, sans l'envoyer. Pour un éditeur graphique,
configurer son option d'attente, par exemple `EDITOR="code --wait"`.
Pendant l'édition, les rafraîchissements sont bloqués avant la suspension du
terminal et ne reprennent qu'après le redémarrage de son driver. La boucle
asynchrone reste active, notamment pour répondre aux pings du daemon ; aucune
image n'est envoyée à la file d'écriture du driver arrêté.
Ce raccourci est disponible dès que la saisie est activée, y compris pendant une
génération, hors popup ou autre édition externe en cours. L'agent peut répondre
ou terminer sans fermer l'éditeur ; le brouillon n'est jamais envoyé automatiquement.
Si un message non accepté est restauré pendant l'édition (refus, annulation ou
déconnexion), il est conservé devant le brouillon édité au retour. En cas d'échec
de l'éditeur, le brouillon d'origine est conservé avec ce texte restauré.
Une permission en attente est rendue visible au retour et conserve la priorité
du focus clavier pour permettre une réponse explicite.

`/resume` suivi d'Entrée ouvre une popup centrée de reprise de session, même pendant
une génération. Confirmer une session annule et sauvegarde le principal en cours
avant de basculer. La liste défilante montre les titres, avec l'heure et la date locales
à droite (`HH:MM  JJ/MM/AAAA`), de la dernière activité la plus récente à la plus
ancienne. Les identifiants restent internes. Les flèches haut/bas changent la
sélection et l'aperçu en dessous affiche la dernière requête utilisateur ; les
réponses d'aperçu devenues obsolètes sont ignorées. Entrée reprend la session et
restaure son historique, son workspace et son titre. Le modèle et la réflexion
globaux restent sélectionnés pour les prochains tours ; les anciennes Activity
affichent les valeurs réellement utilisées à l'époque.
Échap ferme la popup. Une reprise échouée conserve la conversation actuelle.

Le daemon ouvre un index disque léger puis lit les derniers messages visibles,
sans construire l'agent ni relire les gros résultats d'outils. Les détails sont
lus à l'ouverture des activités. Une ancienne session sans index nécessite une
première indexation en arrière-plan. Au premier envoi après reprise, le statut
`Preparing conversation…` indique la restauration du contexte de l'agent,
annulable comme une génération. Avec une compaction, seuls le checkpoint et les
messages nécessaires sont relus ; sans compaction, tout le contexte reste requis.

Pendant une reprise de session ou le retour au présent avant un envoi depuis une
ancienne page, une barre bleu clair (`MISC_MD`) et grasse de type
`[####                  ]` remplace les compteurs de contexte dans la barre d'agent.
Elle est centrée lorsque la largeur le permet et se raccourcit sur terminal étroit,
sans chevaucher le bouton de mode. Le composer et son brouillon restent visibles,
avec la même hauteur. L'animation est indéterminée, pas un pourcentage réel ; les
compteurs de contexte actualisés réapparaissent à la fin du chargement.
Les événements sans changement d'informations ne rafraîchissent pas la barre.
Seuls les libellés modifiés sont mis à jour ; redimensionnements et changements
d'état du chargement continuent à recalculer leur disposition.
La pagination au scroll reste silencieuse : pas de barre de chargement ni de
changement de statut, et le champ de saisie garde son focus et reste éditable.
L'envoi reste bloqué jusqu'à la fin de la navigation, sans effacer le brouillon ;
les erreurs de chargement restent affichées.
Seule la liste des lignes du dernier volet Activity est préchargée, même s'il
contient plus de 50 outils ou est encore en cours. Les autres volets arrivent sous
forme de résumés et leur liste est demandée à l'ouverture. Une ancienne fenêtre
ne précharge pas son propre dernier volet à la place du dernier de la session.
À l'import, chaque ligne d'outil contient uniquement de quoi afficher son titre,
son état et ses permissions. Pour `read`, cela inclut le chemin et une éventuelle
erreur, jamais le contenu d'une lecture réussie. Pour un autre outil, cliquer pour
le déplier demande alors ses arguments et son résultat complet, avec métadonnées,
diffs et pièces jointes. Le volet Reasoning charge aussi son texte uniquement à
l'ouverture. Les erreurs de chargement permettent de fermer/rouvrir pour réessayer.
Le protocole passe en version 4 : reconstruire et redémarrer daemon et TUI ensemble.
Ce chargement à deux niveaux concerne l'historique importé ; les événements live
de génération restent inchangés.
Les contenus explicitement ouverts restent en cache dans la fenêtre affichée
jusqu'au prochain envoi. À chaque nouveau message, tous les anciens volets sont
repliés et vidés : widgets de détail, données préchargées ou demandées et requêtes
d'activité en cours. Leurs titres, indicateurs et métadonnées restent visibles ;
les rouvrir recharge le groupe correspondant. Seule la nouvelle activité se remplit
alors en direct. Ce réglage ne modifie ni le journal disque ni le contexte du modèle.
L'historique forme une fenêtre glissante de 20 messages utilisateur/assistant,
déplacée par blocs de 10. Les 10 messages communs sont conservés avec leur position
visuelle : redescendre légèrement après un chargement ne déclenche pas aussitôt le
chargement inverse. Aux extrémités, le déplacement peut être plus petit. Les nouveaux
messages acceptés rejoignent cette même fenêtre ; au-delà de 20, les plus anciens
et leurs widgets associés sortent de la vue, sans effacer le journal. Le dernier
tour réserve seulement l'espace nécessaire à son alignement en haut ; ce vide ne
crée ni message supplémentaire ni lot séparé dans l'historique.
Un envoi depuis la fenêtre la plus récente ne recharge rien : les anciens volets se
ferment et se vident localement, puis le message part directement. Depuis une fenêtre
plus ancienne, le nouveau tour apparaît immédiatement en haut et reste visible
pendant le chargement de la page récente, avant l'envoi au daemon. Un échec de ce
chargement retire l'aperçu et restaure le brouillon sans envoyer le message.
L'ancienne page reste affichée jusqu'au montage de la nouvelle, préparée hors
layout, puis les widgets sont échangés en un lot. Les messages inchangés de la
fenêtre courante réutilisent leurs widgets ; les pages évincées ne sont pas mises
en cache. Les résumés de compaction fermés ne construisent leur Markdown qu'à
l'ouverture. Lors d'une reprise, la popup disparaît avant le rendu de la page,
laissant la conversation précédente visible avec la barre de chargement.
Le scroll est verrouillé dès la demande de fenêtre et jusqu'au rendu de sa position
finale : molette, pavé tactile et commandes clavier ne déplacent pas la vue pendant
le transfert, le montage et le repositionnement. Les événements anciens ne sont
pas rejoués après le chargement. Le layout et l'ancre sont recalculés dans le même
lot de rendu, sans afficher une image intermédiaire au mauvais offset. Une erreur
ou une annulation libère également le verrou.

`/resume`, `/connect` et `/model` partagent la charte de `dialog_screen.py` : bordure carrée
muted, titre corail, sections sable et indications en bas muted. La liste des
sessions garde les en-têtes fixes `Title` / `Last activity`. L'aperçu `Last user
request` occupe au maximum quatre lignes et défile indépendamment si nécessaire.

`/model` suivi d'Entrée ouvre le sélecteur de modèles, même pendant une génération. Il concatène
les modèles compatibles des providers connectés (OpenAI puis OpenRouter),
du plus récent au plus ancien pour OpenRouter (`sort=newest`) et OpenAI API
(`created` décroissant, dates absentes ou invalides en fin, égalités stables).
Codex trie par `priority` croissant (priorités absentes ou invalides en fin,
égalités stables) : c'est le classement du provider, pas une chronologie.
Les variantes rapides restent juste après leur modèle de base.
Le modèle est à gauche et la colonne provider tout à
droite, avec les libellés alignés sur leur première lettre : `Openai (Codex)`,
`Openai (Api key)` ou `Openrouter (Api key)`, selon l'authentification enregistrée. Un même
modèle reste sélectionnable séparément chez plusieurs providers. Le modèle courant
est marqué `Current` ; le premier résultat est présélectionné à l'ouverture et
à chaque modification de la recherche.

Le champ `Search`, sans encadrement même au focus, filtre instantanément une portion du texte provider/modèle,
sans distinction de casse ni expression régulière. Le filtre est local en mémoire,
sans lancement de ripgrep ni nouvelle requête réseau. `↑`/`↓` parcourent les résultats,
même pendant la saisie ; Entrée confirme, Tab change de champ, Échap ferme et annule
le chargement éventuel. Les erreurs de catalogue sont affichées sans masquer les
modèles des autres providers. Une liste vide invite à utiliser `/connect`.

La sélection mémorise le couple provider/modèle globalement, conserve la réflexion,
le mode et la conversation. La barre en bas à gauche change immédiatement ; l'appel
modèle déjà lancé garde ses réglages, et le prochain appel du même tour applique
la nouvelle sélection. S'il n'y a plus d'appel, elle s'applique au prochain message.
Le pied de réponse conserve le modèle réellement utilisé. Les anciens volets
Activity gardent leurs métadonnées historiques. Une erreur de sélection garde la
popup ouverte pour réessayer.

Après le choix du modèle, un champ demande le maximum effectif en tokens. Il est
prérempli avec le maximum du catalogue, ou la valeur sauvegardée pour le modèle
courant. Si le maximum est inconnu, le champ est vide. La TUI et le daemon refusent
les valeurs inférieures à 32 768 et supérieures au maximum connu. Entrée sauvegarde
la valeur avec le modèle global ; Échap revient à la liste sans rien modifier.
Ce n'est pas une préférence par modèle : changer de modèle propose son maximum.
Le budget s'applique à l'affichage et à la compaction, avec la réserve habituelle.

`/new` suivi d'Entrée annule et sauvegarde une éventuelle exécution principale,
puis revient à l'état initial : chat vide,
titre `-`, workspace de lancement et mode Sane. Le modèle et la réflexion globaux
sont conservés.
La session précédente reste sauvegardée et accessible avec `/resume`. Aucune
nouvelle session n'est créée avant l'envoi du prochain message.
`/connect` et `/permissions` sont également accessibles pendant le tour. Confirmer
le provider via `/connect` arrête le principal avant de détacher sa session.
Les permissions changent pour les contrôles d'outils en cours et suivants, sans
annuler les effets d'un outil déjà lancé. Une demande Ask en attente est réévaluée
si l'on passe à Auto ou Danger.
En revanche, `/compact` et `/recap` pendant une exécution affichent un avertissement
à bande jaune invitant à attendre la fin. Ils ne sont pas mis en file d'attente.
Les réponses complètes, le raisonnement disponible et
les appels/résultats d'outils sont affichés dans le chat. Le composer reste actif
pendant l'exécution pour préparer le prochain message. Entrée le place dans le
bloc `[Pending]` pour un envoi après la fin complète du tour ; un brouillon non
soumis reste dans la saisie et n'est jamais envoyé automatiquement.
Le texte envoyé est retiré immédiatement du composer. Si le daemon refuse le
message avant acceptation, il est restauré devant le nouveau brouillon éventuel.
Ctrl+Q permet de quitter. Il n'y a pas de streaming.
Pendant le travail, un premier clic sur « Cancel » ou Échap sans pending remplace
la même ligne par la barre dégressive couleur Slop, avec
`Cancel Execution ? Confirm with ESC` centré. Un second clic sur cette barre ou
Échap dans les trois secondes envoie `execution.cancel`. La ligne reste alors
couleur Slop avec `Cancelling…` jusqu'à la réponse terminale, sans nouveau compte à
rebours. Sans confirmation, les informations habituelles reviennent. L'animation
réutilise le timer du bouton de mode, qui continue aussi à animer Activity.
Le départ du prochain tour attend l'événement terminal du daemon. Pendant l'accusé
de réception d'un choix de permission, Entrée dans le composer est ignoré pour ne
pas mettre le brouillon en queue par un double appui involontaire.
Une annulation pendant la création du titre
conserve le brouillon et ne crée pas de session.
Le workspace transmis au daemon est le dossier depuis lequel la TUI est lancée.
Le modèle est celui par défaut du daemon ; sélection et reprise viendront ensuite.

## Construction et partage

Depuis la racine, CMake compile `sosd`, construit la distribution autonome puis
installe `sos` :

```sh
cmake -S . -B build
cmake --build build
```

Le sous-projet `cmake/build-tui/` contient le packaging PyInstaller et
l'installation. `cmake --build build --target build-tui` produit seulement la
distribution ; `--target install-tui` la construit et l'installe.

L'archive à partager est `build/tui/dist/sos-<OS>-<architecture>.tar.gz`.
Son destinataire extrait le dossier complet puis lance `./sos/sos` ; conserver
`_internal/` à côté du lanceur. Python, les packages de la TUI et le daemon sont
embarqués. Aucun environnement uv ni checkout du projet n'est nécessaire.
L'archive cible l'OS et l'architecture du build, avec des versions système
compatibles ; construire séparément pour macOS/Linux et chaque architecture.

L'installation locale garde des distributions versionnées sous
`$XDG_DATA_HOME/sos/app/` ou `~/.local/share/sos/app/`, puis crée/remplace le
lien `sos` vers l'exécutable autonome installé, dans le dossier retourné par
`~/.local/bin/`. Aucune commande globale `tui` n'est installée.
Le repository et son `.venv` ne sont plus nécessaires pour lancer cette copie.
Le build CMake complet lance aussi cette installation automatiquement.

On peut ensuite invoquer `sos` depuis n'importe quel dossier : ce dossier reste
le dossier de travail de l'application.
Le dossier `~/.local/bin/` doit être dans le `PATH`.
Si nécessaire, exécuter `uv tool update-shell`, puis rouvrir le terminal.

## Images

Coller ou glisser un chemin de fichier PNG, JPEG ou WebP dans le composer crée
un bloc `[Image • /chemin/capture.png]`. Les espaces et chemins entre guillemets
sont acceptés ; une phrase qui mentionne un chemin reste du texte. Le contenu
est capturé immédiatement : supprimer ou modifier le fichier ensuite ne change
pas la pièce jointe. Les blocs de texte longs portent le préfixe `Pasted text`.

`Ctrl+V` lit explicitement une image du presse-papiers natif et crée un bloc
`[Pasted Image]`. macOS utilise `osascript`, Wayland `wl-paste` et X11 `xclip`.
Pas de surveillance du presse-papiers. `Cmd+V` dépend du terminal : s'il ne
transmet pas de contenu image, utiliser `Ctrl+V` ou coller un chemin. Sur SSH,
le presse-papiers natif du client n'est pas accessible ; le chemin doit désigner
un fichier accessible à la TUI distante.

Un clic sur un bloc affiche son format, ses dimensions, son poids et son origine,
avec `Close` et `Remove`. Suppression, annulation et rétablissement portent sur
le bloc entier. La file d'attente et la restauration après échec conservent les
octets capturés. L'éditeur externe ne reçoit pas les pixels : les images sont
conservées même si leurs mentions sont supprimées dans cet éditeur ; utiliser
`Remove` dans le composer pour retirer réellement une image.

Limites locales : 8 images par message, 5 Mio par image, 10 Mio au total,
16 384 pixels par côté et 40 mégapixels. Les limites du provider peuvent être
plus restrictives. Le daemon et la TUI doivent tous deux utiliser le protocole 6.

À l'envoi, la TUI vide immédiatement le composer et affiche le message provisoire
avec l'Activity animée. Ce retour visuel n'est pas une acceptation : le daemon
vérifie le support image avant de sauvegarder le message ou d'appeler le modèle,
y compris à la sortie de la file d'attente et pour les images du contexte actif.
En cas de refus, la TUI retire le tour provisoire, remet le texte et les images
dans le composer sans écraser un nouveau brouillon, et affiche l'erreur en SLOP.
Le contrôle préalable séparé ne bloque plus le composer.

Les images sont envoyées comme contenu multimodal et sauvegardées dans SQLite,
avec déduplication dans la session. `/resume` les restaure sans relire leurs
fichiers d'origine. Après compaction, les anciens pixels restent sauvegardés mais
ne sont pas envoyés automatiquement au résumeur. Le modèle peut les relire avec
`view_image` et leur ID `img_…` ; cette relecture consomme à nouveau du contexte.
L'historique affiche les métadonnées, pas de miniatures terminal.
