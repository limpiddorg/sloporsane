# CI and distribution

## Workflows

`.github/workflows/ci.yml` builds and tests pushes to `main` only. Pull requests
and other branches do not trigger it. `.github/workflows/release.yml` builds the
exact commit tagged `vMAJOR.MINOR.PATCH` (optionally with a prerelease suffix).
Both use `.github/actions/build` for native tests, Textual tests, installer tests,
bundling and a real standalone startup smoke test. No provider credentials are
needed. All project build, test and installation paths are under `build-ai/`.

Supported release targets:

| Asset | Runner | Compatibility baseline |
| --- | --- | --- |
| `sos-macos-arm64.tar.gz` | `macos-15` (ARM64) | macOS 15 ARM64; older versions not guaranteed |
| `sos-linux-x86_64.tar.gz` | `ubuntu-24.04` | glibc 2.39; older distributions not guaranteed |

There is no macOS Intel or Linux ARM64 asset. Bundles are not cross-compiled.
macOS distributions are not Developer ID signed or notarized; Gatekeeper can
block downloaded software. Linux bundles are not static universal executables.

After both release builds pass, the release job creates a **draft** containing:

- Both archives, each containing the complete `sos/` directory.
- `SHA256SUMS`, covering both archives and the installer template.
- `install.sh.in`, the backend-rendered installer template.
- This distribution contract.

Publish the draft manually in GitHub after reviewing the notes and assets.
No release is created if either platform fails. Only the draft job has
`contents: write`; build jobs have read-only repository access. Existing releases
are not overwritten on reruns. If a draft needs rebuilding, delete that draft
explicitly before rerunning; do not replace already published version assets.
A tag does not have to wait for the separate main CI: the release workflow runs
all checks itself on the tagged commit. Use a commit from `main` when tagging.

```sh
git tag v0.1.0
git push origin v0.1.0
```

GitHub Actions artifacts are retained for seven days. Published release assets
are the durable distribution source, not files committed into Git.

## Backend rendering contract

The backend is not implemented in this repository. Its `/install` endpoint must:

1. Resolve a published release, never a draft, once for the request. Do not select
   archives and checksums independently using changing `latest` URLs.
2. Obtain `install.sh.in` from that release, not from `main`. Retain the template
   associated with old releases if their archive layout ever changes.
3. Replace `@@VERSION_BASE64@@` with the UTF-8 version encoded as standard Base64
   without line breaks, and `@@RELEASE_URL_BASE64@@` with the similarly encoded
   HTTPS base URL of that release's assets, without a trailing slash.
4. Require exactly one occurrence of each placeholder before rendering and no
   remaining placeholders afterward. Insert only Base64 characters, never raw
   request input or shell-escaped strings. Do not evaluate the template.
5. Serve the rendered bytes as plain text. Cache them by resolved version and
   download configuration; invalidate that cache when storage locations change.

Example values before Base64 encoding:

```text
version: v0.1.0
release_url: https://github.com/OWNER/REPOSITORY/releases/download/v0.1.0
```

The URL is backend configuration, not embedded in old templates. A repository
rename or storage move requires changing the backend mapping and preserving the
old release assets, not modifying those templates. Already downloaded/rendered
scripts retain their old URL. Do not inject authentication secrets into public
scripts; public installation requires publicly downloadable assets.

The user runs the rendered script with Bash, for example through `curl ... | bash`.
It first warns in English that the user's running SOS applications and daemon will
be closed. An interactive Yes/No selector reads `/dev/tty`, with **No selected by
default**. Enter on No cancels without writing or stopping processes; a missing
terminal is an error, never implicit approval. Left/Right changes the selection.
After successful download and extraction, confirmation permits SIGTERM for exact
`sos` and `sosd` process names owned by the current user. If they do not stop within
five seconds, installation fails without switching the launcher; no SIGKILL.
Other users' processes and unrelated terminal programs are not targeted.
The script detects its platform, downloads one archive and `SHA256SUMS`, requires
HTTPS even across redirects and checks the archive hash before extraction.
Base64 avoids shell syntax injection during substitution; it is not encryption.
The checksum detects corruption but is not a release signature and does not
protect against compromise of the distribution origin.

The complete bundle is installed under `$XDG_DATA_HOME/sos/app/<archive-sha256>/`
(or `~/.local/share/sos/app/` for unset/relative XDG), with `~/.local/bin/sos`
pointing to its launcher. No sudo or shell-profile edits. Old versions are retained;
a failed download or verification does not replace the current launcher.
Concurrent installers are rejected by a lock. An uncatchable termination may
leave `.install.lock`; remove it only after confirming no installer is running.

## Version and in-app updates

`SOS_VERSION` is a CMake cache variable, defaulting to `dev`. Release builds set it
from the validated Git tag; ordinary CI builds stay `dev`. The value is written
into the bundle, not inferred from the install directory or overridden at runtime.
It appears as `dev` or `vMAJOR.MINOR.PATCH` at the right of the tabs row, and in
`sos --version`.
For local testing (no installation into the real home is needed):

```sh
cmake -S . -B build-ai -DSOS_VERSION=v0.0.1
cmake --build build-ai --target build-tui
build-ai/tui/dist/sos/sos --version
# Launch this exact bundle, not a previously installed `sos` from PATH.
build-ai/tui/dist/sos/sos
# Restore development builds explicitly; CMake caches the value.
cmake -S . -B build-ai -DSOS_VERSION=dev
```

A `dev` build performs no update request. A stable version checks
`https://limpidd.com/api/releases/latest` once per TUI launch in the background.
The endpoint returns JSON `{"version":"v0.0.2"}` from the same validated release
and five-minute cache as `/install`, respecting backend release pinning. Drafts
and prereleases are not offered. Compare numeric major/minor/patch values; failures,
invalid responses, equal/older versions and unavailable network are silent.

The update dialog defaults to **Not now** and explains that all of this user's
SOS applications and daemon will close. Acceptance exits Textual and execs Bash
in the original terminal. Bash fetches `/install` over HTTPS and runs it with
`--yes --expected-version VERSION`: there is no second prompt. If the backend's
selected release has changed since confirmation, abort before installing. The
installer must advertise `# SOS_UPDATE_PROTOCOL=1`; older templates that do not
understand the confirmation/version arguments are rejected, not executed.
The independent Bash process is not named `sos`/`sosd` and survives their shutdown.
After installation, it execs `~/.local/bin/sos` in the originating workspace with
the same socket argument, resetting PyInstaller's environment. Other closed TUI
instances are not reopened. A failed install does not restart SOS; diagnostics stay
in the terminal and the previous launcher remains unchanged.

The backend code lives in the separate webapp repository; deploy its endpoint
before shipping a version that checks for updates. Existing `v0.0.1` bundles do
not gain in-app updating retroactively.

## Local verification

```sh
bash -n scripts/install.sh.in
python3 -B scripts/test_installer.py build-ai/installer-tests
```

Installer tests simulate downloads and platform detection without accessing the
network or the real user home. Actual runner builds and GitHub release publication
must also be verified in Actions after the workflows are pushed.
