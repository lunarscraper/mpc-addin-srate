#!/bin/bash
# Läuft im Container debian:buster (glibc 2.28), damit die Bibliothek
# auch auf der älteren glibc der Force lädt.
set -euo pipefail
cd /src

cat > /etc/apt/sources.list <<'EOF'
deb http://archive.debian.org/debian buster main
EOF
apt-get -o Acquire::Check-Valid-Until=false update -qq
apt-get install -y -qq --no-install-recommends \
  gcc libc6-dev make zip libasound2 \
  gcc-arm-linux-gnueabihf libc6-dev-armhf-cross binutils-arm-linux-gnueabihf >/dev/null

make clean
make test
make all
make dist VERSION="${VERSION:-dev}"

echo "--- benötigte glibc-Versionen (Force braucht <= ihre eigene) ---"
arm-linux-gnueabihf-objdump -T build/arm/libmpc-addin-srate.so \
  | grep -o 'GLIBC_[0-9.]*' | sort -uV
file build/arm/libmpc-addin-srate.so 2>/dev/null || true
