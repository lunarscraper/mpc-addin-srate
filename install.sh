#!/bin/sh
# Installiert mpc-addin-srate neben einem bereits vorhandenen LD_PRELOAD-Addin
# (z. B. mpc-addin-surface). Das Skript sucht die Startdatei, die LD_PRELOAD
# setzt, legt eine Sicherung an und hängt diese Bibliothek vorne an.
#
# Aufruf auf der Force (per SSH, im entpackten Ordner):
#   sh install.sh            zeigt, was passieren würde, und fragt nach
#   sh install.sh --yes      ohne Rückfrage

set -e
HERE=$(cd "$(dirname "$0")" && pwd)
LIB=libmpc-addin-srate.so
YES=0
[ "$1" = "--yes" ] && YES=1

say() { echo "[srate] $*"; }
die() { echo "[srate] FEHLER: $*"; exit 1; }

ARCH=$(uname -m); [ -n "$SRATE_TEST" ] && ARCH=armv7l
case "$ARCH" in
  armv7*|armhf) ;;
  *) die "Unerwartete Architektur $(uname -m) – das ist keine Force/MPC der 1. Generation." ;;
esac
[ -f "$HERE/$LIB" ] || die "$LIB fehlt im Ordner $HERE"

# 1. Startdateien suchen, die LD_PRELOAD setzen und ein Addin laden
say "Suche Startdatei mit LD_PRELOAD ..."
CANDS=""
ROOTS=${SRATE_TEST_ROOTS:-/etc /usr/bin /usr/sbin /usr/local /opt /media}
for f in $(find $ROOTS -maxdepth 4 -type f -size -64k 2>/dev/null); do
  case "$f" in
    "$HERE"/*|*.srate-bak|*/srate-*|*.md|*.txt|*.c|*.h|*.yml|*.log) continue ;;
  esac
  if grep -qs "LD_PRELOAD" "$f" && grep -qs "\.so" "$f"; then
    CANDS="$CANDS $f"
  fi
done
N=$(echo $CANDS | wc -w)
if [ "$N" -eq 0 ]; then
  die "Keine Startdatei mit LD_PRELOAD gefunden. Wie ist mpc-addin-surface bei dir installiert? Schick mir bitte die Ausgabe von:  grep -rls LD_PRELOAD /etc /media 2>/dev/null"
fi
if [ "$N" -gt 1 ]; then
  # Bevorzugt die Datei, in der schon ein mpc-addin steht
  PREF=""
  for f in $CANDS; do grep -qs "mpc-addin\|addin" "$f" && PREF="$PREF $f"; done
  [ "$(echo $PREF | wc -w)" -eq 1 ] && CANDS=$PREF
fi
N=$(echo $CANDS | wc -w)
if [ "$N" -ne 1 ]; then
  say "Mehrere mögliche Startdateien gefunden:"
  for f in $CANDS; do echo "   $f"; grep -n "LD_PRELOAD" "$f" | head -n 3 | sed 's/^/      /'; done
  die "Nicht eindeutig – bitte schick mir diese Ausgabe, dann passe ich das Skript an."
fi
LAUNCHER=$(echo $CANDS)

LINE=$(grep -n "LD_PRELOAD" "$LAUNCHER" | head -n1)
LINENO_=${LINE%%:*}
OLDSO=$(echo "${LINE#*:}" | grep -o '/[^ "'"'"':]*\.so[^ "'"'"':]*' | head -n1)
[ -n "$OLDSO" ] || die "Konnte in $LAUNCHER keinen .so-Pfad in der LD_PRELOAD-Zeile erkennen: ${LINE#*:}"
TARGETDIR=$(dirname "$OLDSO")
NEWSO="$TARGETDIR/$LIB"

say "Startdatei:        $LAUNCHER (Zeile $LINENO_)"
say "Vorhandenes Addin: $OLDSO"
say "Neues Addin nach:  $NEWSO"
if grep -qs "$LIB" "$LAUNCHER"; then
  say "Startdatei enthält $LIB bereits – kopiere nur die Dateien neu."
fi

if [ $YES -eq 0 ]; then
  printf "[srate] Fortfahren? (j/N) "
  read ANS
  case "$ANS" in j|J|y|Y) ;; *) say "Abgebrochen, nichts verändert."; exit 0 ;; esac
fi

# 2. Schreibschutz ggf. aufheben
REMOUNTED=""
ensure_rw() {
  if ! touch "$1/.srate-w" 2>/dev/null; then
    MP=$(df "$1" | awk 'NR==2{print $6}')
    say "Mache $MP beschreibbar ..."
    mount -o remount,rw "$MP" && REMOUNTED="$REMOUNTED $MP"
    touch "$1/.srate-w" || die "$1 ist nicht beschreibbar."
  fi
  rm -f "$1/.srate-w"
}
ensure_rw "$TARGETDIR"
ensure_rw "$(dirname "$LAUNCHER")"

# 3. Dateien kopieren (vorhandene srate.conf bleibt erhalten)
cp "$HERE/$LIB" "$NEWSO"
chmod 755 "$NEWSO"
[ -f "$TARGETDIR/srate.conf" ] || cp "$HERE/srate.conf" "$TARGETDIR/srate.conf"
cp "$HERE/diag.sh" "$TARGETDIR/srate-diag.sh"
cp "$HERE/uninstall.sh" "$TARGETDIR/srate-uninstall.sh"

# 4. Startdatei sichern und anpassen
if ! grep -qs "$LIB" "$LAUNCHER"; then
  [ -f "$LAUNCHER.srate-bak" ] || cp -p "$LAUNCHER" "$LAUNCHER.srate-bak"
  sed -i "${LINENO_}s|$OLDSO|$NEWSO:$OLDSO|" "$LAUNCHER"
  grep -qs "$LIB" "$LAUNCHER" || die "Anpassung fehlgeschlagen – Sicherung liegt unter $LAUNCHER.srate-bak"
fi
echo "$LAUNCHER" > "$TARGETDIR/srate.installed"
sync

for MP in $REMOUNTED; do mount -o remount,ro "$MP" 2>/dev/null || true; done

say "Fertig. Neue Zeile:"
sed -n "${LINENO_}p" "$LAUNCHER" | sed 's/^/   /'
say "Jetzt die Force neu starten. Log danach: $TARGETDIR/srate.log"
say "Notaus: leere Datei $TARGETDIR/srate.off anlegen und neu starten."
