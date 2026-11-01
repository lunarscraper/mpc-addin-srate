#!/bin/bash
# Läuft im Container debian:buster (glibc 2.28), damit die Bibliothek
# auch auf der älteren glibc der Force lädt.
set -euo pipefail
cd /src

# Das Buster-Image enthält libc6 aus den Sicherheits-Updates. Ohne diese
# Quelle passt libc6-dev nicht dazu ("held broken packages").
rm -f /etc/apt/sources.list.d/*
cat > /etc/apt/sources.list <<'EOF'
deb http://archive.debian.org/debian buster main
deb http://archive.debian.org/debian buster-updates main
deb http://archive.debian.org/debian-security buster/updates main
EOF
apt-get -o Acquire::Check-Valid-Until=false update
apt-get install -y --no-install-recommends \
  gcc libc6-dev make zip libasound2 \
  gcc-arm-linux-gnueabihf libc6-dev-armhf-cross binutils-arm-linux-gnueabihf

make clean
make test
make all
make dist VERSION="${VERSION:-dev}"

echo "--- benötigte glibc-Versionen (Force braucht <= ihre eigene) ---"
arm-linux-gnueabihf-objdump -T build/arm/libmpc-addin-srate.so \
  | grep -o 'GLIBC_[0-9.]*' | sort -uV
file build/arm/libmpc-addin-srate.so 2>/dev/null || true
