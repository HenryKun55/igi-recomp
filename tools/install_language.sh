#!/bin/sh
# Install the Brazilian Portuguese language into a game data directory.
#
#   tools/install_language.sh <pc dir>
#
# <pc dir> is the directory with IGI.exe and language/ (gamedata/pc on the
# Mac, ux0:data/igi/pc on the Vita - e.g. the Vita3K copy under
# ~/Library/Application Support/Vita3K/Vita3K/fs/ux0/data/igi/pc). It writes
# language/portuguese/, adds the language's name to the other languages' menu
# texts and its entry to the language screen (menusystem/mainmenu.qvm, the
# original kept as mainmenu.qvm.orig). Only the Python 3 standard library is
# needed. Safe to run again. The runtime enables the language when
# language/portuguese/menusystem.res exists (runtime/src/igi_language.c).
set -eu

if [ $# -ne 1 ]; then
    echo "usage: $0 <pc dir>" >&2
    exit 2
fi
here=$(cd "$(dirname "$0")" && pwd)
exec python3 "$here/lang/build_language.py" "$1" --src "$here/../translations/pt-br"
