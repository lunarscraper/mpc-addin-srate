#!/bin/sh
# Diagnose: Was meldet das angeschlossene Audiogerät?
# Aufruf:  sh diag.sh vendor     (SH-4d im Vendor-Modus)
#          sh diag.sh class      (SH-4d im Class-Compliant-/Generic-Modus)
# Ergebnis: srate-diag-<name>.txt im selben Ordner.

LABEL=${1:-test}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT="$HERE/srate-diag-$LABEL.txt"
MATCH=$(sed -n 's/^[[:space:]]*match[[:space:]]*=[[:space:]]*//p' "$HERE/srate.conf" 2>/dev/null | head -n1)
MATCH=${MATCH:-SH4d}

{
  echo "=== srate-diag ($LABEL) $(date) ==="
  echo "--- System ---"
  uname -a
  (ldd --version 2>&1 | head -n1) || true
  echo
  echo "--- /proc/asound/cards ---"
  cat /proc/asound/cards
  echo
  for f in /proc/asound/card*/stream*; do
    [ -r "$f" ] || continue
    echo "--- $f ---"
    cat "$f"
    echo
  done
  echo "--- USB-Geräte ---"
  for d in /sys/bus/usb/devices/*; do
    [ -r "$d/idVendor" ] || continue
    echo "$(cat $d/idVendor):$(cat $d/idProduct) $(cat $d/manufacturer 2>/dev/null) $(cat $d/product 2>/dev/null)"
  done
  echo
  # Kartennummer des Zielgeräts suchen (Bindestriche/Groß-Klein egal)
  NORM=$(echo "$MATCH" | tr -cd '[:alnum:]' | tr '[:upper:]' '[:lower:]')
  CARD=$(awk -v m="$NORM" '
    /^ *[0-9]+ \[/ { n=$1 }
    { l=tolower($0); gsub(/[^a-z0-9]/,"",l); if (index(l,m) && n!="") { print n; exit } }
  ' /proc/asound/cards)
  if [ -n "$CARD" ]; then
    echo "Zielgerät '$MATCH' = Karte $CARD"
    if command -v arecord >/dev/null 2>&1; then
      echo "--- Test: Aufnahme direkt (hw) mit 44100 Hz ---"
      arecord -D hw:$CARD,0 -r 44100 -f S24_3LE -c 2 -d 1 /dev/null 2>&1 | tail -n 3
      echo "--- Test: Aufnahme über ALSA-Umrechnung (plughw) mit 44100 Hz ---"
      arecord -D plughw:$CARD,0 -r 44100 -f S16_LE -c 2 -d 1 /dev/null 2>&1 | tail -n 3
    else
      echo "(arecord nicht vorhanden – Aufnahmetest übersprungen)"
    fi
  else
    echo "Zielgerät '$MATCH' NICHT gefunden – angeschlossen und eingeschaltet?"
  fi
} > "$OUT" 2>&1

echo "Fertig. Ergebnis: $OUT"
