"""One-directory distribution: shared daemon files remain available to other integrations."""

import os
from pathlib import Path

from PyInstaller.utils.hooks import collect_all

source = Path(os.environ["SOS_TUI_SOURCE"])
datas, binaries, hiddenimports = [], [(os.environ["SOSD_BINARY"], "sos_tui/bin")], []
for package in ("textual", "pygments"):
    package_data, package_binaries, package_imports = collect_all(package)
    datas += package_data
    binaries += package_binaries
    hiddenimports += package_imports

analysis = Analysis(
    [str(Path(SPECPATH) / "entrypoint.py")],
    pathex=[str(source / "src")],
    binaries=binaries,
    datas=datas,
    hiddenimports=hiddenimports,
)
archive = PYZ(analysis.pure)
executable = EXE(
    archive, analysis.scripts, [],
    exclude_binaries=True, name="sos", console=True, strip=False, upx=False,
)
distribution = COLLECT(
    executable, analysis.binaries, analysis.datas,
    name="sos", strip=False, upx=False,
)
