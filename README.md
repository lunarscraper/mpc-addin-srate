# mpc-addin-srate

LD_PRELOAD-Addin für die Akai Force (MPC OS 3.x, 1. Generation, 32-Bit-ARM).
Macht USB-Audiogeräte nutzbar, die nur 48 kHz können – gedacht für den
**Roland SH-4d**, der an der Force mit „Fehler 22“ scheitert, weil die Force
fest mit 44,1 kHz arbeitet.

## Wie es funktioniert

Die Force öffnet Audiogeräte direkt („hw:…“). Dann muss das Gerät 44,1 kHz
selbst können – der SH-4d kann das nicht, daher Fehler 22 (= „ungültiger Wert“).

Das Addin fängt diesen Aufruf ab und öffnet **nur den SH-4d** stattdessen über
„plughw:…“. Dann rechnet ALSA zwischen 48 kHz und 44,1 kHz um. Alle anderen
Geräte bleiben unberührt. Zusätzlich schreibt das Addin mit, was die Force
anfordert und was schiefgeht (`srate.log`).

**Bekannte Grenzen**
- Die Umrechnung ist einfach (linear) – kleiner Qualitätsverlust in den Höhen.
- Etwas mehr Latenz und CPU-Last; je mehr Kanäle, desto mehr Last.
- Ob die Force-Software das umgeleitete Gerät akzeptiert, zeigt erst der Test.

## 1. Bauen (GitHub)

1. Neues Repo `mpc-addin-srate` anlegen und den Inhalt dieses Ordners hochladen.
2. Unter **Releases → Draft a new release** einen Tag `v0.1.0` anlegen und veröffentlichen.
3. Actions baut und hängt `mpc-addin-srate-0.1.0.zip` an das Release.

## 2. Auf die Force bringen

ZIP auf die SD-Karte kopieren, dann per SSH auf der Force:

```
cd /media/<deine-SD-Karte>
unzip mpc-addin-srate-0.1.0.zip
cd mpc-addin-srate
```

## 3. Diagnose: Vendor und Class Compliant (vor der Installation)

SH-4d per USB an die Force, eingeschaltet. Dann:

```
sh diag.sh vendor
```

Am SH-4d den USB-Treibermodus umstellen (Vendor ↔ Generic/Class Compliant),
SH-4d neu starten, wieder anstecken, dann:

```
sh diag.sh class
```

Ergebnis: `srate-diag-vendor.txt` und `srate-diag-class.txt`. Wichtig darin:
die Zeilen **Rates:** (welche Abtastraten der SH-4d anbietet) und **Channels:**.
Bietet einer der Modi schon 44100 an, brauchst du das Addin für diesen Modus nicht.

## 4. Installieren

```
sh install.sh
```

Das Skript kopiert die Bibliothek nach `/usr/lib/mpc-addin-srate/` und trägt sie
in `/etc/ld.so.preload` ein (vorhandene Einträge, z. B. andere Addins, bleiben
erhalten). Das Log landet im Ordner auf der SD-Karte, von dem aus installiert wurde.

Danach **Force neu starten**, SH-4d anstecken und in den Einstellungen als
Audio-Interface wählen.

## 5. Testen und Log

Log liegt im Installationsordner auf der SD-Karte:

```
cat /media/<SD-Karte>/mpc-addin-srate/srate.log
```

Erfolg sieht so aus:

```
open 'hw:2,0' => plughw:2,0 CAPTURE ... -> 0 ok
set_rate plughw:2,0 44100 -> 0 ok
hw_params plughw:2,0 -> 0 ok
```

Bitte für beide Modi (vendor/class) testen und die Logs schicken.

## Einstellungen (`srate.conf`)

| Schlüssel | Bedeutung |
|---|---|
| `mode = plug` | Umleiten und umrechnen (Standard) |
| `mode = log` | Nichts ändern, nur mitschreiben |
| `match = SH4d` | Gerätename (Teil davon) aus `/proc/asound/cards` |
| `verbose = 1` | Alle Audiogeräte mitloggen |

## Notaus und Entfernen

- Sofort abschalten: `touch /usr/lib/mpc-addin-srate/srate.off`, Force neu starten.
- Ganz entfernen: `sh /usr/lib/mpc-addin-srate/srate-uninstall.sh`, Force neu starten.
