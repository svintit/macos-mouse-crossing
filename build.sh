#!/bin/bash
set -eu
root="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
app="${HAMMERSPOON_APP:-/Applications/Hammerspoon.app}"
headers="$app/Contents/Frameworks/LuaSkin.framework/Headers"
test -f "$headers/lua.h" || { echo "Hammerspoon Lua headers not found: $headers" >&2; exit 1; }
command -v xcrun >/dev/null
mkdir -p "$root/build"
xcrun clang -std=c11 -O2 -Wall -Wextra -Werror -Wno-error=deprecated-declarations -fblocks -bundle -undefined dynamic_lookup -I"$headers" -framework ApplicationServices "$root/native.c" -o "$root/build/portable_mouse_crossing_hid.so.next"
mv "$root/build/portable_mouse_crossing_hid.so.next" "$root/build/portable_mouse_crossing_hid.so"
xcrun clang -std=c11 -O2 -Wall -Wextra -Werror -Wno-error=deprecated-declarations -framework ApplicationServices -framework IOKit "$root/measure.c" -o "$root/build/measure"
