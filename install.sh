#!/bin/sh
# Installiert mpc-addin-srate über /etc/ld.so.preload.
# Das funktioniert unabhängig davon, wie andere Addins (z. B. mpc-addin-surface)
# eingebunden sind – keine Startdatei muss gesucht oder verändert werden.
#
# Aufruf auf der Force (per SSH, im entpackten Ordner auf der SD-Karte):
#   sh install.sh            zeigt, was passiert, und fragt nach
#   sh install.sh --yes      ohne Rückfrage

set -e
HERE=$(cd "$(dirname "$0")" && pwd)
LIB=libmpc-addin-srate.so
YES=0
[ "$1" = "--yes" ] && YES=1

ROOT=${SRATE_TEST_ROOT:-}                 # nur für Tests
PRELOADFILE="$ROOT/etc/ld.so.preload"
TARGETDIR="$ROOT/usr/lib/mpc-addin-srate"
NEWSO="$TARGETDIR/$LIB"
# Pfad, wie er in ld.so.preload steht (ohne Test-Präfix)
SOPATH="/usr/lib/mpc-addin-srate/$LIB"

say() { echo "[srate] $*"; }
die() { echo "[srate] FEHLER: $*"; exit 1; }

ARCH=$(uname -m); [ -n "$SRATE_TEST" ] && ARCH=armv7l
case "$ARCH" in
  armv7*|armhf) ;;
  *) die "Unerwartete Architektur $(uname -m) – das ist keine Force/MPC der 1. Generation." ;;
esac
[ -f "$HERE/$LIB" ] || die "$LIB fehlt im Ordner $HERE"

# Log auf die SD-Karte legen, wenn wir von dort installiert werden
LOGPATH=""
case "$HERE" in /media/*) LOGPATH="$HERE/srate.log" ;; esac

say "Bibliothek nach:   $NEWSO"
say "Eintrag in:        $PRELOADFILE"
if [ -s "$PRELOADFILE" ]; then
  say "Vorhandener Inhalt von $PRELOADFILE:"
  sed 's/^/     /' "$PRELOADFILE"
else
  say "$PRELOADFILE gibt es noch nicht – wird angelegt."
fi
[ -n "$LOGPATH" ] && say "Log nach:          $LOGPATH"

if [ $YES -eq 0 ]; then
  printf "[srate] Fortfahren? (j/N) "
  read ANS
  case "$ANS" in j|J|y|Y) ;; *) say "Abgebrochen, nichts verändert."; exit 0 ;; esac
fi

# Schreibschutz des Systems ggf. aufheben
REMOUNTED=""
ensure_rw() {
  mkdir -p "$1" 2>/dev/null || true
  if ! touch "$1/.srate-w" 2>/dev/null; then
    # nächsten vorhandenen Ordner suchen, um den Einhängepunkt zu bestimmen
    P="$1"
    while [ ! -d "$P" ] && [ "$P" != "/" ]; do P=$(dirname "$P"); done
    MP=$(df -P "$P" 2>/dev/null | awk 'NR==2{print $NF}')
    [ -n "$MP" ] || MP=/
    say "Mache $MP beschreibbar ..."
    mount -o remount,rw "$MP" && REMOUNTED="$REMOUNTED $MP"
    mkdir -p "$1"
    touch "$1/.srate-w" || die "$1 ist nicht beschreibbar."
  fi
  rm -f "$1/.srate-w"
}
ensure_rw "$(dirname "$PRELOADFILE")"
ensure_rw "$TARGETDIR"

# Dateien kopieren
cp "$HERE/$LIB" "$NEWSO.new" && mv "$NEWSO.new" "$NEWSO"
chmod 755 "$NEWSO"
cp "$HERE/srate.conf" "$TARGETDIR/srate.conf"
[ -n "$LOGPATH" ] && printf '\nlog = %s\n' "$LOGPATH" >> "$TARGETDIR/srate.conf"
cp "$HERE/diag.sh" "$TARGETDIR/srate-diag.sh"
cp "$HERE/uninstall.sh" "$TARGETDIR/srate-uninstall.sh"
rm -f "$TARGETDIR/srate.off"

# ld.so.preload ergänzen (Sicherung, falls schon vorhanden)
if [ -f "$PRELOADFILE" ]; then
  [ -f "$TARGETDIR/ld.so.preload.bak" ] || cp -p "$PRELOADFILE" "$TARGETDIR/ld.so.preload.bak"
else
  echo "neu" > "$TARGETDIR/ld.so.preload.created"
fi
if ! grep -qsF "$SOPATH" "$PRELOADFILE"; then
  echo "$SOPATH" >> "$PRELOADFILE"
fi
echo "$PRELOADFILE" > "$TARGETDIR/srate.installed"
sync

# Kurztest: lädt die Bibliothek? (ein neuer Befehl würde sonst meckern)
if [ -z "$SRATE_TEST" ]; then
  if /bin/true 2>&1 | grep -q "cannot be preloaded"; then
    sed -i "\|$SOPATH|d" "$PRELOADFILE"
    die "Die Bibliothek lässt sich nicht laden – Eintrag wieder entfernt. Bitte Ausgabe von  LD_PRELOAD=$NEWSO /bin/true  schicken."
  fi
fi

for MP in $REMOUNTED; do mount -o remount,ro "$MP" 2>/dev/null || true; done

say "Fertig. Inhalt von $PRELOADFILE:"
sed 's/^/     /' "$PRELOADFILE"
# Bleibt /etc nach dem Neustart erhalten? (tmpfs wäre flüchtig)
ETCFS=$(df -P "$(dirname "$PRELOADFILE")" 2>/dev/null | awk 'NR==2{print $1" auf "$NF}')
say "/etc liegt auf: $ETCFS"
say "Jetzt die Force neu starten und danach prüfen:  cat /etc/ld.so.preload"
[ -n "$LOGPATH" ] && say "Log danach: $LOGPATH"
say "Notaus: touch $TARGETDIR/srate.off  und neu starten."
say "Entfernen: sh $TARGETDIR/srate-uninstall.sh"
