#!/bin/sh
# Entfernt mpc-addin-srate: Eintrag aus /etc/ld.so.preload löschen
# (bzw. Sicherung zurückspielen) und Bibliothek entfernen.
set -e
ROOT=${SRATE_TEST_ROOT:-}
TARGETDIR="$ROOT/usr/lib/mpc-addin-srate"
PRELOADFILE="$ROOT/etc/ld.so.preload"
SOPATH="/usr/lib/mpc-addin-srate/libmpc-addin-srate.so"
say() { echo "[srate] $*"; }

[ -d "$TARGETDIR" ] || { say "Keine Installation gefunden."; exit 0; }

REMOUNTED=""
for p in "$(dirname "$PRELOADFILE")" "$TARGETDIR"; do
  if ! touch "$p/.srate-w" 2>/dev/null; then
    MP=$(df "$p" | awk 'NR==2{print $6}')
    mount -o remount,rw "$MP" && REMOUNTED="$REMOUNTED $MP"
  fi
  rm -f "$p/.srate-w"
done

if [ -f "$PRELOADFILE" ]; then
  sed -i "\|$SOPATH|d" "$PRELOADFILE"
  if [ -f "$TARGETDIR/ld.so.preload.created" ] && [ ! -s "$PRELOADFILE" ]; then
    rm -f "$PRELOADFILE"
    say "$PRELOADFILE entfernt (war von srate angelegt)."
  else
    say "Eintrag aus $PRELOADFILE entfernt."
  fi
fi
sync
rm -rf "$TARGETDIR"
sync
for MP in $REMOUNTED; do mount -o remount,ro "$MP" 2>/dev/null || true; done
say "Entfernt. Bitte die Force neu starten."
