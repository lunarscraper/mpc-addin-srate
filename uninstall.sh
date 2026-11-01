#!/bin/sh
# Entfernt mpc-addin-srate wieder: stellt die gesicherte Startdatei her
# und löscht die Bibliothek. srate.conf und srate.log bleiben liegen.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
say() { echo "[srate] $*"; }

STATE=""
for d in "$HERE" $(find /media /usr /opt /etc -maxdepth 4 -name srate.installed 2>/dev/null | xargs -r -n1 dirname); do
  [ -f "$d/srate.installed" ] && { STATE="$d/srate.installed"; break; }
done
[ -n "$STATE" ] || { say "Keine Installation gefunden."; exit 0; }

TARGETDIR=$(dirname "$STATE")
LAUNCHER=$(cat "$STATE")

for p in "$TARGETDIR" "$(dirname "$LAUNCHER")"; do
  touch "$p/.srate-w" 2>/dev/null || mount -o remount,rw "$(df "$p" | awk 'NR==2{print $6}')"
  rm -f "$p/.srate-w"
done

if [ -f "$LAUNCHER.srate-bak" ]; then
  cp -p "$LAUNCHER.srate-bak" "$LAUNCHER" && rm -f "$LAUNCHER.srate-bak"
  say "Startdatei wiederhergestellt: $LAUNCHER"
else
  sed -i "s|$TARGETDIR/libmpc-addin-srate.so:||" "$LAUNCHER"
  say "Eintrag aus $LAUNCHER entfernt."
fi
rm -f "$TARGETDIR/libmpc-addin-srate.so" "$STATE"
sync
say "Entfernt. Bitte die Force neu starten."
