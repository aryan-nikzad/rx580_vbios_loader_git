#!/bin/sh
# Installs the RX 580 loader on the EFI System Partition and puts it FIRST in the UEFI boot order.
# Run from the unzipped folder:   sudo sh install_linux.sh
set -e
[ "$(id -u)" = 0 ] || { echo "Run with sudo:  sudo sh install_linux.sh"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
ESP=${ESP:-/boot/efi}
[ -f "$HERE/vbios_loader.efi" ] && [ -d "$HERE/vbioses" ] || { echo "Build vbios_loader.efi and create a vbioses/ directory containing your own ROM dumps first"; exit 1; }
mountpoint -q "$ESP" || { echo "EFI partition is not mounted at $ESP (set ESP=/path if it is elsewhere)"; exit 1; }

mkdir -p "$ESP/EFI/vbios"
cp "$HERE/vbios_loader.efi" "$ESP/EFI/vbios/"
rm -rf "$ESP/EFI/vbios/vbioses"; cp -r "$HERE/vbioses" "$ESP/EFI/vbios/vbioses"
sync
echo "Copied to $ESP/EFI/vbios ($(ls "$ESP/EFI/vbios/vbioses" | wc -l) ROM files)"

SRC=$(findmnt -n -o SOURCE "$ESP")                       # e.g. /dev/nvme0n1p1 or /dev/sda1
NAME=$(basename "$SRC")
PART=$(cat "/sys/class/block/$NAME/partition")
DISK="/dev/$(basename "$(readlink -f "/sys/class/block/$NAME/..")")"
echo "EFI partition: $SRC  (disk $DISK, partition $PART)"

# remove an older entry of ours, then create a new one
for n in $(efibootmgr | awk '/RX580 loader/ {gsub(/Boot|\*/,"",$1); print $1}'); do efibootmgr -q -B -b "$n"; done
efibootmgr -q --create --disk "$DISK" --part "$PART" --label "RX580 loader" --loader '\EFI\vbios\vbios_loader.efi'
NEW=$(efibootmgr | awk '/RX580 loader/ {gsub(/Boot|\*/,"",$1); print $1; exit}')
CUR=$(efibootmgr | awk -F': ' '/^BootOrder/ {print $2}' | tr -d ' ')
REST=$(echo "$CUR" | tr ',' '\n' | grep -v "^$NEW$" | grep -v '^$' | paste -sd, -)
efibootmgr -q -o "$NEW${REST:+,$REST}"
echo; efibootmgr | head -8
echo; echo "Done. Reboot: the loader menu appears, starts by itself after 5 s, then your normal OS boots."
