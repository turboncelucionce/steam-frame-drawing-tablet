#!/usr/bin/env bash
# Installs everything: OpenTabletDriver (built from source), then the KWin plugin that brings
# its pen into the Steam Frame's desktop with pressure.
# Run it on the Frame, from the desktop's Konsole:   bash install.sh
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"

bash "$here/install-otd.sh"
bash "$here/kwin-otd-tablet/install.sh"
