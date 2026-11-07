# Project instructions and skills

## Project instructions

The principal agent automatically loads `<workspace>/AGENTS.md` before generating
its first response. It checks that file again between user turns. A successful
`read` inside a subdirectory also loads applicable `AGENTS.md` files from the
workspace root down to that directory, in order. Instructions include their path
and directory scope; more specific instructions apply within their subtree.

Unchanged instructions are not appended again. Loaded versions are persisted in
the session; compaction retains the latest exact instructions and skill catalog,
not just a model-written summary. Nested instructions are checked again when a
file in their scope is read. Removed instruction files are explicitly withdrawn
when their scope is checked again.

Only the session workspace and its descendants are searched. There is no parent,
home-directory, global, `CLAUDE.md`, `CONTEXT.md`, or remote fallback. Automatic
loading is disabled for a resumed workspace outside the integration's authorized
root. Instruction files must be regular text files of at most 128 KiB; symbolic
`AGENTS.md` files are not followed. Project instructions never override harness
rules, execution modes, or permissions.

## Skill format

SOS follows the core [Agent Skills format](https://agentskills.io/specification):
a skill directory containing `SKILL.md`, YAML frontmatter (`name`, `description`),
a Markdown instruction body, and optional `scripts/`, `references/`, and `assets/`
resources. Standard descriptive fields `license`, `compatibility`, and `metadata`
are supported too. Discovery exposes metadata, not instructions.

Arguments use Claude Code-style positional substitution and an optional
`argument-hint` for display. SOS adds optional `argument-descriptions` for
per-argument documentation in the preview panel. There is no value-validation
schema: no `required` flag, `{name}` substitution, or named invocation object.
This is not full Claude Code compatibility: discovery remains SOS-specific and
hooks, fork execution, `allowed-tools`, and `${CLAUDE_*}` variables are unsupported.
Unknown frontmatter fields are silently ignored, not returned to the model or
interpreted as permission grants. This includes `allowed-tools`, hooks, agents,
model selection, and fork directives. Known fields retain their type checks.

### Discovery

Skills are discovered only in these locations:

- Workspace: `<workspace>/.sos/skills/<name>/SKILL.md`.
- User: `$XDG_CONFIG_HOME/sos/skills/<name>/SKILL.md`, default `~/.config/sos/skills/<name>/SKILL.md`.

A workspace folder shadows the same user skill entirely, even if invalid. Its
diagnostic is shown instead of silently falling back. No `.claude`, `.agents`,
remote, or parent-directory discovery occurs. No directory is created just by
discovering skills. Global discovery grants no general access to the home directory.
The catalog refreshes between turns; a running agent uses its turn's snapshot.

Files are limited to 128 KiB. Discovery does not follow symlinks, including `.sos`,
`skills`, individual skill folders, and files. Invalid skills are excluded and
reported in the TUI without blocking valid ones.

### Frontmatter

```markdown
---
name: check-api
description: Check HTTP routes in a source directory.
argument-hint: "[directory] [note]"
arguments: [directory, note]
argument-descriptions:
  directory: Directory containing the routes to check.
  note: Additional guidance for the analysis.
license: MIT
compatibility: Requires Python 3.11+.
metadata:
  author: example-team
  version: "1.0"
---
Read scripts/check_routes.py before executing it.
Run it with directory $directory using the normal Bash tool.
Consider this note: $note
Analyze its output and report the problems found.
```

`name` and `description` are required. The name must match the directory name,
start with a lowercase ASCII letter, and contain only lowercase letters, digits,
`-` and `_` (at most 64 characters). The Markdown body must not be empty.
Duplicate supported frontmatter keys are rejected. Ignored fields are not validated.

`argument-hint` is optional literal display text, such as `[directory] [note]`
or `[task description...]`. It is not parsed or enforced. Suggestions and the
composer show the hint; the details header contains no hint. Completion inserts only the
name. At a leading invocation with no input yet, the composer shows the full hint
in gray; it disappears once input starts. It is never submitted as user input.
The text is preserved verbatim, including `<`, `>`, brackets, and other punctuation.
There is no per-position validation.

`arguments` is optional. It can be a YAML list (`[directory, note]`) or a
space-separated string (`directory note`). Names map to positions in declaration
order, without required/optional reordering. Names begin with an ASCII letter or
underscore and continue with ASCII letters, digits, underscores, or hyphens.
Duplicates and the reserved name `ARGUMENTS` are rejected. An empty list or string
is allowed. This field only defines `$name` aliases; it does not limit input length
or require values. A hint does not need a corresponding `arguments` field.

`argument-descriptions` is an optional SOS extension: a mapping of names to
nonempty description strings. The details panel shows an Arguments heading and
numbered names/descriptions, first in `arguments` declaration order. Additional
description-only labels follow in catalog order; they can document raw input
without defining positional aliases. Names without a description still appear
when the section exists. An absent or empty mapping hides the section entirely.
There are no required/optional flags, generated hints, argument binding, or value
checks. Invalid mapping types, empty descriptions, and duplicate description keys
are reported as definition errors. Other hosts may ignore this extension.

Optional descriptive metadata:

- `license`: nonempty text naming a license or referring to a license file.
- `compatibility`: nonempty text describing environment requirements, at most
  500 Unicode characters. The field is `compatibility`, not `compat`.
- `metadata`: a string-to-string mapping. Scalar YAML values are read as strings;
  quote versions to preserve intent. Empty strings and an empty mapping are allowed.
  Nulls, nested objects, lists, and duplicate keys are rejected.

These fields, `argument-hint`, and `argument-descriptions` are omitted from catalogs when absent and returned
as `Skill attributes (JSON data)` when present. They do not install dependencies,
check compatibility, open files, or grant permissions.

### Positional and raw substitutions

| Placeholder | Replacement |
| --- | --- |
| `$ARGUMENTS` | The complete raw argument text, including original quotes. |
| `$0`, `$1`, ... | The first, second, ... token, indexed from zero. |
| `$ARGUMENTS[N]` | The same indexed token as `$N`. |
| `$name` | The token at the position declared for `name` in `arguments`. |

For `/migrate "Search Bar" JavaScript TypeScript`, a body containing
`Migrate $0 from $1 to $ARGUMENTS[2].` becomes
`Migrate Search Bar from JavaScript to TypeScript.` before model generation.
Use `$ARGUMENTS`, rather than `$0` or a named alias, to receive an entire unquoted
sentence.

Indexed and named placeholders use shell-style quoting and escaping, without
running a shell, expanding environment variables, executing commands, or globbing.
An absent numeric index stays literal; an absent declared named value becomes
an empty string. Unknown names and all brace expressions remain literal.
A single backslash escapes a recognized dollar placeholder (`\$0`); the backslash
is removed. With two or more immediately preceding backslashes, expansion occurs
and the backslashes remain. Substitution is a single pass over the Markdown body,
including code blocks: inserted values are never expanded again. Frontmatter and
resource files are not substituted.

If no recognized placeholder receives input, nonempty input is appended as
`ARGUMENTS: <raw text>`. Missing numeric indices and escaped placeholders do not
count as receiving input. Declared named aliases do, even if their value is absent.
`$ARGUMENTS` and the fallback accept arbitrary prose, including unfinished quotes.
Only indexed or declared named substitutions require balanced quotes and complete
escapes. Malformed grouped input fails before generation. Missing values and extra
tokens do not fail. The agent should ask for information it cannot infer instead
of inventing it; a hint is guidance, not a software validation rule.

### Migration

The former SOS `arguments` mapping is rejected with a migration diagnostic.
Replace descriptions and `required` flags with an ordered list of names, move
usage guidance to `argument-hint`, descriptions to `argument-descriptions`, and
replace `{name}` with `$name`.
For a single argument intended to capture prose, replace `{task}` with `$ARGUMENTS`
instead of `$task`. Former `name=value` options have no binding semantics: they
are ordinary input tokens. User skills are not automatically rewritten.

New daemon and model-tool requests use only `name` and optional `input`:

```json
{"name":"check-api","input":"\"src/my routes\" \"explain failures\""}
```

The former invocation `arguments` object is rejected. Omitted `input` defaults to
an empty string; serialization includes it explicitly. Old persisted tool calls
and results remain readable as history, without being reloaded against the new
skill format.

## Display and resources

Suggestion rows show `/name` followed by `argument-hint`, if present. The details
header shows only the name, a muted license, and right-aligned
`[workspace]` or `[user]`. Compatibility appears below in muted color, and Metadata
appears last with one key-value entry per line. Missing fields and empty metadata
are hidden. Hint and description text are literal, never Rich markup.

The details panel shows Arguments when descriptions exist, then lists Scripts,
References, and Assets recursively, using sorted
skill-relative paths without reading their contents. Each category is limited to
1000 files and rejects symlinks. Errors are shown per category; empty categories
have no heading. Resource lists are not added to the model's discovery catalog.
Instructions can point to resources for explicit reading when needed.

The loader returns the skill's absolute location, optional descriptive attributes,
raw input as `Input (JSON string data)`, expanded Markdown under `Content`, and
associated script paths only. It never reads or executes the scripts. Loading is
a read operation. The model is instructed to inspect scripts before requesting
execution; this is an instruction, not an enforced review gate. Execution is a
separate normal `bash` call subject to Sane and permission checks. Loading does not
change the workspace, install dependencies, or grant extra permissions.

Skill call results also persist display-only metadata: `argument_names` in declaration
order and `argument_values` using the loader's shell-style grouping. Raw-input skills
with unmatched quotes keep their full input as one display value, without changing
loading behavior. Textual shows the skill name in green, then one `name: value`
line per declared argument and a separator before the unchanged result. Argument
labels use MISC without bold, as do ordinary tool argument labels. Without declared
names, the complete raw input is shown as one `argument:` value, never split into
words. Extra positional values after declared names use zero-based `argument N:`
labels. Older results without this metadata also show one raw argument; they never
read the current skill file to reconstruct historical names.

## Invocation

The model sees names, descriptions, positional names, origins, and optional
metadata, not skill bodies. It can select and load a skill with the `skill` tool.
Textual uses the same daemon loader when the user invokes `/skill-name`.
Native commands and skills share `/`; native names are reserved. Conflicting
skills are excluded from suggestions and reported without blocking native commands.
Completion replaces only the token at the cursor, anywhere in the message.
References inside prose remain ordinary text, not structured invocations.

Textual passes raw input without binding arguments or parsing quotes. Whitespace
before the input is a command separator; the remaining text, including trailing
whitespace, is preserved. `/skill-name` alone is valid. Arbitrary subcommands such
as `status` are interpreted by the model using the skill, not dispatched natively.

Up to six consecutive known skill names at the beginning form a chain:
`/caveman /check-api src/http` loads both in order with shared input `src/http`.
An unknown or native command, a quoted skill name, ordinary text, or a seventh
skill stops expansion; that token and the remainder become shared input. Quote a
skill-looking value when it is an argument rather than an invocation. No skills
are automatically expanded from the middle of prose.

The daemon loads the entire chain from one catalog before acceptance or title
and model generation. Unknown skills, invalid definitions, or malformed grouped
input reject the whole request without partial activity. Missing/extra arguments
are not errors. Each loaded skill gets its own synthetic call/result pair and
Activity entry, with distinct IDs even for repeated names. For example:

```text
User (provider text):
Call the skill "check-api" with input (JSON string data):
"src/http"

Assistant tool call:
skill({"name":"check-api","input":"src/http"})

Tool result:
[exact result returned by the shared loader]
```

The original slash command remains visible and persisted. Display provenance
`user_skill` is not sent to providers. Activity shows
`skill · <name> → Triggered by user command`; expanding it shows resolved tool
input and the real loaded result. No extra system injection or second execution
occurs. This gives the model an already-completed loading operation, but does not
technically forbid a redundant tool call. Model-initiated loading remains normal.

Resume reuses exact saved calls, results, and rewritten provider text without
reopening changed skill files. New invocations load the current files. System
messages are not projected as skill activities. Skill commands submitted during a turn use
the ordinary message queue.

Compaction retains complete recent turns including call/result pairs. Older
skill content may leave active context when summarized, while the exact result
remains in history. The recording sequence has not been crash fault-injection
tested; no atomicity guarantee for all three messages is claimed.

BTW conversations retain their frozen context and read-only tool allowlist;
they do not gain a skill tool or script execution.
