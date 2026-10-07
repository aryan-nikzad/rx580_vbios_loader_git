# RX 580 vBIOS Loader

A UEFI application for initializing an AMD Polaris / RX 580 GPU when its SPI
vBIOS flash chip (or the electrical path to it) is unavailable.

The loader reads a **user-supplied full ROM dump** from a FAT filesystem,
emulates the ROM reads required during initialization, and can expose the ROM
to the operating system through PCI/ACPI mechanisms.

## Quick Setup

```bash
git clone https://github.com/aryan-nikzad/rx580_vbios_loader_git.git
cd rx580_vbios_loader_git
sudo apt install gnu-efi build-essential
make
```

### Add your own vBIOS

```bash
mkdir vbioses
```

Copy one or more `.rom` vBIOS files into the `vbioses/` folder:

```text
rx580_vbios_loader_git/
├── vbioses/
│   ├── my_rx580.rom
│   ├── backup.rom
│   └── another_vbios.rom
└── ...
```

You can add **multiple ROMs**. The loader will try them until one successfully initializes the GPU.

You must provide your **own vBIOS dump** or a legally obtained compatible ROM. Do not add proprietary ROMs to this repository.

### Install

```bash
chmod +x ./install_linux.sh
sudo ./install_linux.sh
```

> **Requirements:** The system must be booted in UEFI mode and GRUB must be installed. The installation script adds the vBIOS loader to the EFI/GRUB boot process.

Reboot the system after installation.

**Done.** The loader will attempt to load the supplied RX 580 vBIOS before the operating system initializes the GPU.


> **Important:** This repository intentionally contains **no GPU vBIOS dumps,
> GOP images, extracted firmware, or prebuilt `.efi` loader**. Firmware is
> hardware/vendor material and should be obtained and used by the end user.

## How it works

The loader:

- finds `*.rom` files in a `vbioses/` directory;
- sorts them by filename, so names such as `00_...rom` can be tried first;
- parses the PCI option-ROM structure and UEFI image;
- can run the included AtomBIOS interpreter;
- serves ROM reads from the supplied file during initialization;
- can publish the ROM through the interfaces expected by the OS;
- remembers test results in UEFI NVRAM.

For the important initialization path, a **full SPI-chip dump** is preferable.
The original project testing found that the Polaris memory-controller data may
live beyond the normal BIOS image. Do not trim a hardware dump just because
the visible BIOS image is smaller.

## 1. Dump your own ROM

If your card's SPI flash is still readable, make a complete dump before doing
anything else. Use a programmer such as a CH341A with the appropriate voltage
adapter, or another reliable SPI programmer.

**Keep the original dump private. Do not commit it to Git.**

The loader does not require a particular filename. Rename your dump to any
simple `.rom` name, for example:

```text
my_rx580_original.rom
```

Then place it here:

```text
vbioses/my_rx580_original.rom
```

If you have multiple dumps, put all of them in `vbioses/`. The loader sorts them
alphabetically. If you want one attempted first, give it a prefix such as:

```text
00_original_full_dump.rom
01_matching_stock_rom.rom
02_fallback_rom.rom
```

A `00_` filename is only an ordering convention; it does not make the ROM more
compatible.

### Where to obtain a stock ROM

If your original dump is unavailable, you can research a matching stock image
from the [TechPowerUp VGA BIOS Collection](https://www.techpowerup.com/vgabios/?model=RX+580).
It contains many RX 580 vendor/model/memory variants. Match the exact board,
memory size, memory vendor, subsystem ID, and preferably the BIOS version.

A downloaded stock ROM is **not equivalent to your own full SPI dump**. In
particular, this loader may need data located outside the normal BIOS image.
Prefer a full dump from your own card whenever possible.

You can also use a ROM dumped by another owner as a diagnostic/compatibility
candidate, but verify that the board and memory configuration are actually
compatible. The project does not endorse arbitrary ROM flashing.

## 2. Build the loader

On Debian/Ubuntu:

```bash
sudo apt install build-essential gnu-efi efibootmgr
make
```

This produces:

```text
vbios_loader.efi
```

No ROM is embedded during the build.

The source tree deliberately does not ship prebuilt firmware-derived `.efi`
GOP files. If you need to extract a UEFI PE image for development/testing, the
optional `tools/extract_pe.py` helper can process a ROM you supply locally.

## 3. Prepare the EFI partition

Create this layout on a FAT EFI System Partition:

```text
EFI/
└── vbios/
    ├── vbios_loader.efi
    └── vbioses/
        ├── 00_original_full_dump.rom
        └── 01_matching_rom.rom
```

The loader can also find a `vbioses` directory next to the `.efi`, or in common
EFI locations, but the layout above is the recommended one.

From Linux, the included installer can copy the loader and your local ROMs:

```bash
sudo sh install_linux.sh
```

It expects `/boot/efi` by default. Use `ESP=/your/mountpoint` if needed.

## 4. Using the loader

On boot, start `vbios_loader.efi` through:

- a UEFI firmware boot entry;
- a UEFI shell;
- GRUB `chainloader`.

The loader searches FAT volumes for `vbioses/`, presents the ROM candidates, and
can automatically try them.

Useful keys include:

- **UP/DOWN + ENTER** — select a ROM;
- **ENTER / AUTO** — try ROMs automatically;
- **R** — forget the remembered working ROM;
- **ESC** — skip;
- **V** — VFCT-only mode;
- **F** — force initialization;
- **P** — post-init register snapshot;
- **D** — register dump;
- **E** — switch between the AtomBIOS interpreter and firmware GOP path.

The exact behavior depends on the GPU, ROM, firmware environment, and the
state of the card. A failed initialization can hang or reset the machine.

## 5. Full-dump requirement

The normal BIOS image inside an RX 580 ROM may be around 118–120 KiB while a
physical SPI chip can contain a 256 KiB image.

For this project, do **not** automatically cut a 256 KiB programmer dump down to
the visible BIOS image. Keep the complete dump when available.

If the selected ROM lacks the Polaris memory-controller block, the loader can
look for a usable MC block in another supplied full dump. This is one reason
keeping your own original full dump as the first `00_...rom` candidate is useful.

## Firmware safety

This is low-level firmware/GPU initialization software. Test with a recovery
path available. Do not assume that an RX 580 ROM is interchangeable merely
because both cards are called "RX 580".

The project is intended to **load/use** a ROM for recovery or initialization.
It is not a recommendation to flash an incompatible ROM onto the card.

## Repository hygiene

Do not commit:

- your personal SPI dumps;
- downloaded vendor vBIOS files;
- modified/mining ROMs;
- extracted GOP/PE firmware;
- generated `.efi` binaries;
- diagnostic register dumps.

These patterns are covered by `.gitignore`, but check `git status` before
pushing.

## Licensing

Original project code is released under the MIT License in `LICENSE`.

The `atomlib/` directory contains AMD AtomBIOS-derived code with its own AMD
MIT-style copyright and permission notices. Those notices remain in the source;
see `LICENSES-AMD-ATOMBIOS.txt`.

ROM dumps, vBIOS images, GOP images, and other vendor firmware are intentionally
excluded from this repository and are **not covered by this project's MIT
license**.

## Development

See [`AGENTS.md`](AGENTS.md) for the project-specific development workflow,
architecture notes, build rules, and firmware-handling requirements.
