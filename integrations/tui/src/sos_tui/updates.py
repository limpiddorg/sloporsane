"""Release identity, bounded update discovery, and terminal-preserving installer handoff."""
import json
import os
from pathlib import Path
import re
from urllib.request import HTTPRedirectHandler, build_opener

VERSION_FILE = Path(__file__).with_name("VERSION")
VERSION = VERSION_FILE.read_text().strip() if VERSION_FILE.is_file() else "dev"
LATEST_URL = "https://limpidd.com/api/releases/latest"
INSTALL_URL = "https://limpidd.com/install"
STABLE_VERSION = re.compile(r"v?(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\Z")


def stable_version(value):
    match = STABLE_VERSION.fullmatch(value) if isinstance(value, str) else None
    return tuple(map(int, match.groups())) if match else None


class HTTPSRedirects(HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, newurl):
        if not newurl.startswith("https://"):
            raise ValueError("Update redirects must use HTTPS")
        return super().redirect_request(request, fp, code, message, headers, newurl)


def latest_update(current=VERSION):
    """Network failures are intentionally silent; dev never makes a request."""
    if current == "dev" or stable_version(current) is None:
        return None
    try:
        with build_opener(HTTPSRedirects()).open(LATEST_URL, timeout=3) as response:
            data = response.read(16_385)
        if len(data) > 16_384:
            return None
        version = json.loads(data)["version"]
        parsed = stable_version(version)
        if parsed is not None and parsed > stable_version(current):
            return version
    except (OSError, ValueError, KeyError, TypeError):
        pass
    return None


# exec replaces the TUI process with Bash, outside the set of SOS process names.
# No daemon or old bundle is needed while installing. Positional args avoid shell
# interpolation of paths, versions or launch arguments. Keep the controlling TTY.
HANDOFF = r'''set -eu
version=$1
url=$2
launcher=$3
workspace=$4
shift 4
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
trap 'exit 1' HUP INT TERM
curl --fail --silent --show-error --location --proto '=https' --proto-redir '=https' --connect-timeout 15 --max-time 60 "$url" --output "$work/install.sh"
grep -qx '# SOS_UPDATE_PROTOCOL=1' "$work/install.sh" || {
    printf 'Error: This release does not support in-app updates. Install it manually.\n' >&2
    exit 1
}
/bin/bash "$work/install.sh" --yes --expected-version "$version"
rm -rf "$work"
trap - EXIT
cd "$workspace"
export PYINSTALLER_RESET_ENVIRONMENT=1
exec "$launcher" "$@"
'''


def install_and_restart(version, workspace, arguments):
    if stable_version(version) is None:
        raise ValueError("Invalid update version")
    # Never execute a restart command received from the network.
    launcher = str(Path.home() / ".local/bin/sos")
    environment = dict(os.environ)
    # PyInstaller's private libraries must not leak into system curl/Bash or the
    # newly installed bundle. Restore the pre-launch loader paths, if present.
    for name in ("LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH"):
        original = environment.pop(name + "_ORIG", None)
        environment.pop(name, None)
        if original is not None:
            environment[name] = original
    environment["PYINSTALLER_RESET_ENVIRONMENT"] = "1"
    os.execve("/bin/bash", ["bash", "-c", HANDOFF, "sos-update", version, INSTALL_URL,
                            launcher, str(Path(workspace).resolve()), *arguments], environment)
