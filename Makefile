# Build the UEFI loader from source.
#
# No vBIOS, GOP/PE image, or firmware dump is embedded in the repository.
# The finished loader discovers user-supplied *.rom files at runtime.
#
# Prerequisites on Debian/Ubuntu:
#   sudo apt install build-essential gnu-efi efibootmgr
#
# Build:
#   make
#
# The output is vbios_loader.efi. It is intentionally ignored by git.

CFLAGS = -I/usr/include/efi -I/usr/include/efi/x86_64 -fpic -fshort-wchar \
 -fno-stack-protector -fno-stack-check -mno-red-zone -maccumulate-outgoing-args \
 -ffreestanding -Wall -DEFI_FUNCTION_WRAPPER

vbios_loader.efi: vbios_loader.so
	objcopy -j .text -j .sdata -j .data -j .rodata -j .dynamic -j .dynsym \
		-j .rel -j .rela -j '.rel*' -j '.rela*' -j .reloc \
		-O pei-x86-64 $< $@

vbios_loader.so: vbios_loader.o atomlib/atom.o atom_support.o
	ld -shared -Bsymbolic -L/usr/lib -T/usr/lib/elf_x86_64_efi.lds /usr/lib/crt0-efi-x86_64.o \
		vbios_loader.o atomlib/atom.o atom_support.o -o $@ -lefi -lgnuefi

vbios_loader.o: vbios_loader.c
	gcc $(CFLAGS) -c $< -o $@

atomlib/atom.o: atomlib/atom.c atomlib/amdgpu.h
	gcc -O2 -fpic -fshort-wchar -fno-stack-protector -mno-red-zone -ffreestanding -w \
		-Iatomlib/inc -Iatomlib -include atomlib/amdgpu.h -c $< -o $@

atom_support.o: atom_support.c
	gcc $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o *.so vbios_loader.efi atomlib/*.o

.PHONY: clean
