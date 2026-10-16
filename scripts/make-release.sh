#!/usr/bin/env bash
# Builds the Linux release archive from the Release build:
#   ~/axell-gate-release/AxellGate-<version>-linux-x86_64{/,.tar.gz} + SHA256SUMS.txt + release-notes.md
# Run scripts/build.sh and the test suites first.
set -euo pipefail
cd "$(dirname "$0")/.."
VERSION=$(sed -n 's/^project(AxellGate VERSION \([0-9.]*\)).*/\1/p' CMakeLists.txt)
NAME="Axell Gate"
BUNDLE="build/AxellGate_artefacts/Release/VST3/$NAME.vst3"
OUTROOT="$HOME/axell-gate-release"
PKG="AxellGate-$VERSION-linux-x86_64"
OUT="$OUTROOT/$PKG"

[ -d "$BUNDLE" ] || { echo "No Release build. Run scripts/build.sh first."; exit 1; }
grep -q "\"Name\": \"$NAME\"" "$BUNDLE/Contents/Resources/moduleinfo.json" || { echo "moduleinfo.json does not name $NAME"; exit 1; }
grep -q "\"Version\": \"$VERSION\"" "$BUNDLE/Contents/Resources/moduleinfo.json" || { echo "bundle version is not $VERSION"; exit 1; }

rm -rf "$OUT" "$OUTROOT/$PKG.tar.gz"
mkdir -p "$OUT"
cp -r "$BUNDLE" "$OUT/"
strip --strip-unneeded "$OUT/$NAME.vst3/Contents/x86_64-linux/$NAME.so"
cp LICENSE THIRD_PARTY_NOTICES.md "$OUT/"

cat > "$OUT/install.sh" <<'EOS'
#!/usr/bin/env bash
# Installs Axell Gate into ~/.vst3 (the per-user VST3 folder that Linux hosts scan). Replaces an older copy of
# Axell Gate; nothing else in ~/.vst3 is touched.
set -euo pipefail
cd "$(dirname "$0")"

NAME="Axell Gate.vst3"
DEST="$HOME/.vst3"
TARGET="$DEST/$NAME"

[ -d "$NAME" ] || { echo "Error: '$NAME' not found next to install.sh. Run it from the extracted release folder."; exit 1; }
[ "$(uname -m)" = "x86_64" ] || { echo "Error: this build is for x86_64 Linux, this machine is $(uname -m)."; exit 1; }

is_bundle() {
    grep -q '"Name": "Axell Gate"' "$1/Contents/Resources/moduleinfo.json" 2>/dev/null && return 0
    [ -f "$1/Contents/x86_64-linux/Axell Gate.so" ]
}

mkdir -p "$DEST"
if [ -e "$TARGET" ] && ! is_bundle "$TARGET"; then
    echo "Error: '$TARGET' exists but does not look like Axell Gate. Not touching it."
    exit 1
fi
# copy to a temporary name first so an interrupted install never leaves a half-copied plugin
TMP=$(mktemp -d "$DEST/.axell-gate-install.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
cp -r "$NAME" "$TMP/"
[ -e "$TARGET" ] && { echo "Replacing older copy: $TARGET"; rm -rf "$TARGET"; }
mv "$TMP/$NAME" "$TARGET"

SO="$TARGET/Contents/x86_64-linux/Axell Gate.so"
if ldd "$SO" | grep -q "not found"; then
    echo "Installed to $TARGET, but these system libraries are missing:"
    ldd "$SO" | grep "not found"
    echo "Install them with your distribution's package manager (usually freetype and fontconfig)."
    exit 1
fi
echo "Installed: $TARGET"
echo "All required system libraries are present."
echo "Now restart your DAW so it scans the new plugin, then search for \"Axell Gate\" in its FX browser."
EOS

cat > "$OUT/uninstall.sh" <<'EOS'
#!/usr/bin/env bash
# Removes Axell Gate from ~/.vst3. Your own presets are kept.
set -euo pipefail
TARGET="$HOME/.vst3/Axell Gate.vst3"
PRESETS="${XDG_DATA_HOME:-$HOME/.local/share}/AxellGate/Presets"
if [ ! -e "$TARGET" ]; then
    echo "Axell Gate is not installed in ~/.vst3. Nothing to do."
    exit 0
fi
if ! grep -q '"Name": "Axell Gate"' "$TARGET/Contents/Resources/moduleinfo.json" 2>/dev/null \
   && [ ! -f "$TARGET/Contents/x86_64-linux/Axell Gate.so" ]; then
    echo "Error: '$TARGET' does not look like Axell Gate. Not touching it."
    exit 1
fi
rm -rf "$TARGET"
echo "Removed: $TARGET"
echo "Restart your DAW so it forgets the plugin."
if [ -d "$PRESETS" ]; then
    echo "Your presets were kept in: $PRESETS"
    echo "Delete that folder too if you don't need them any more."
fi
EOS
chmod +x "$OUT/install.sh" "$OUT/uninstall.sh"

cat > "$OUT/README.txt" <<EOS
Axell Gate $VERSION - Linux x86_64 VST3
=========================================

The gate of the Axell suite: a smart vocal gate that opens on the voice (not on a level), with breath
control and removal of the beat's headphone bleed via the side-chain; drum tail shaping without clicks
(decay scaling); and host-synced rhythmic gating. 21 factory presets and A/B.


REQUIREMENTS
------------
- x86_64 Linux with glibc 2.39 or newer (Bazzite, Fedora 40+, Ubuntu 24.04+)
- Any VST3 host. Tested in REAPER.
- System libraries freetype and fontconfig (present on desktop distributions)


INSTALL
-------
In this folder, run:

  ./install.sh

Then restart your DAW and search for "Axell Gate" in its FX browser.

Manual install: copy the "Axell Gate.vst3" folder into ~/.vst3 yourself.

If your DAW doesn't find the plugin, add ~/.vst3 to its VST3 path. In REAPER:
Options > Preferences > Plug-ins > VST, add the folder (on Bazzite and other Fedora atomic
systems use the full path /var/home/<your-user>/.vst3), then click Re-scan.


UNINSTALL
---------
  ./uninstall.sh

Your user presets in ~/.local/share/AxellGate/Presets are kept.


LICENCE
-------
GNU Affero General Public License v3 (LICENSE). Third-party components: THIRD_PARTY_NOTICES.md.
EOS

# release notes = this version's CHANGELOG section
awk -v v="$VERSION" '$0 ~ "^## " v {f=1; print; next} /^## / && f {exit} f' CHANGELOG.md > "$OUTROOT/release-notes.md"
[ -s "$OUTROOT/release-notes.md" ] || { echo "CHANGELOG.md has no section for $VERSION"; exit 1; }

(cd "$OUTROOT" && tar czf "$PKG.tar.gz" --owner=0 --group=0 "$PKG" && sha256sum "$PKG.tar.gz" > SHA256SUMS.txt)
echo "Release: $OUTROOT/$PKG.tar.gz"
cat "$OUTROOT/SHA256SUMS.txt"
