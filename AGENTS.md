# AGENTS.md

## Project goal

`rx580_vbios_loader` is a low-level UEFI application for initializing AMD
Polaris/RX 580 GPUs when the card's SPI vBIOS cannot be read normally.

The project is intentionally source-only. Firmware payloads are supplied by
the person testing the hardware.

## Non-negotiable repository rules

1. **Never commit ROM/vBIOS dumps.**
   - `*.rom`, `*.bin`, extracted firmware, and vendor GOP/PE images are local
     test assets.
   - Keep them outside Git or under ignored paths.
2. **Never re-add embedded ROMs to `vbios_loader.c`.**
   - The loader must discover user-supplied ROMs at runtime.
3. **Do not commit `vbios_loader.efi`.**
   - Build artifacts are ignored.
4. Preserve third-party copyright/license notices.
5. Do not claim that vendor firmware is covered by this project's MIT license.
6. Keep user instructions explicit about using a **full SPI dump** when possible.
7. Avoid destructive firmware operations in scripts. This project loads ROM data;
   it should not silently flash a card.

## Architecture

- `vbios_loader.c`
  - Main UEFI application.
  - Finds `vbioses/*.rom`.
  - Parses PCI option ROMs.
  - Extracts/uses the UEFI image.
  - Implements ROM emulation and GPU initialization flow.
  - Handles the menu, persistent NVRAM state, diagnostics, and ACPI/VFCT.
- `atomlib/`
  - AMD AtomBIOS-derived interpreter and headers.
  - Retain the original AMD MIT-style notices.
- `atom_support.c`
  - Adapter/support layer for the AtomBIOS interpreter.
- `install_linux.sh`
  - Linux helper for copying the built loader and local `vbioses/` to the ESP.
- `tools/extract_pe.py`
  - Optional local-development helper for extracting a UEFI PE image from a
    user-supplied ROM.
  - It depends on the external `uefi_firmware` Python package.
- `Makefile`
  - Source-only build. No firmware is required at compile time.

## Runtime ROM contract

The loader expects user-supplied files ending in `.rom`.

Recommended layout:

```text
EFI/vbios/
├── vbios_loader.efi
└── vbioses/
    ├── 00_original_full_dump.rom
    ├── 01_matching_stock.rom
    └── 02_fallback.rom
```

The filename does not need a particular model string. Alphabetical ordering is
used for candidate order.

A complete programmer dump is preferable to a trimmed BIOS image because
Polaris initialization may require data beyond the visible BIOS image, including
the memory-controller block.

## How a developer should test

Keep private test firmware outside the repository, for example:

```text
~/rx580-test-firmware/
├── original_full_dump.rom
├── stock_candidate.rom
└── modified_candidate.rom
```

Copy or symlink test ROMs into a local ignored `vbioses/` directory only when
needed.

Before committing:

```bash
git status --short
git diff --check
git ls-files | grep -Ei '\.(rom|bin|efi)$'
```

The final command should return no tracked firmware/build artifacts.

Build:

```bash
make clean && make
```

For installer testing, create a local `vbioses/` directory and place only
private test dumps there.

## Firmware handling

### User dump

If the user can still read the SPI flash:

1. Make a complete backup/dump first.
2. Verify the dump size against the physical flash capacity.
3. Keep the original untouched.
4. Rename a working copy if desired.
5. Place it in `vbioses/`.
6. Give the preferred candidate a `00_` prefix so it is tried first.

Never ask users to overwrite their only backup.

### Stock/downloaded ROM

A stock ROM can be used as a candidate when an original dump is unavailable,
but compatibility must be checked carefully. Useful sources include the
TechPowerUp VGA BIOS Collection. Do not mirror those firmware files into this
repository.

### MC fallback

The loader can search other supplied ROM files for a valid Polaris memory
controller block. Therefore multiple candidate ROMs can be useful, but this
does not make arbitrary ROMs compatible.

## Licensing

The root `LICENSE` applies to original project code.

`atomlib/` contains AMD AtomBIOS-derived code whose source files include their
own permissive AMD copyright/license text. Do not remove those notices.

Firmware is a separate category. ROM/GOP/vendor firmware is not automatically
MIT merely because this repository is MIT. Keep firmware out of the repository.

## Code-change guidance

When changing ROM discovery:

- preserve FAT-only assumptions;
- preserve case-insensitive `.rom` matching;
- preserve deterministic filename ordering;
- keep the search bounded so malformed FAT trees cannot cause an unbounded scan;
- do not silently select a random ROM from another removable drive if that
  behavior is changed;
- update README instructions whenever the expected directory layout changes.

When changing initialization:

- prefer bounded waits/timeouts;
- keep trace logging useful after a hard freeze;
- avoid changes that turn a recoverable initialization failure into an
  unconditional machine hang/reset;
- document hardware-specific assumptions near the code that uses them.

## Build outputs

Expected local output:

```text
vbios_loader.efi
```

Do not add it to Git. GitHub users should build it from source.

## Before a release

Check all of the following:

- [ ] No `.rom`, `.bin`, vendor firmware, or extracted GOP files are tracked.
- [ ] No prebuilt `vbios_loader.efi` is tracked.
- [ ] `README.md` explains how to obtain/dump a user's own ROM.
- [ ] `README.md` explains full-dump vs trimmed-image behavior.
- [ ] `LICENSE` is present.
- [ ] AMD AtomBIOS notices remain intact.
- [ ] `git diff --check` passes.
- [ ] `make clean && make` succeeds on a supported Ubuntu/Debian environment.
- [ ] Installer paths match the README.
- [ ] No personal diagnostic logs or hardware-specific secrets are present.
