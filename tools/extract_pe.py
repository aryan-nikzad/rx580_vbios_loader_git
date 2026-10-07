#!/usr/bin/env python3
"""Local development helper: extract the UEFI driver from a user-supplied AMD vBIOS ROM.
Walks the option-ROM chain, finds the code-type-3 image, decompresses it
(UEFI compression) if flagged, and writes the raw PE image."""
import struct, sys
from uefi_firmware import efi_compressor as ec
d = open(sys.argv[1], 'rb').read(); off = 0
while d[off:off+2] == b'\x55\xaa':
    pcir = off + struct.unpack('<H', d[off+0x18:off+0x1a])[0]
    assert d[pcir:pcir+4] == b'PCIR'
    ln = struct.unpack('<H', d[pcir+0x10:pcir+0x12])[0] * 512
    if d[pcir+0x14] == 3: break
    if d[pcir+0x15] & 0x80: sys.exit('no UEFI image in ROM')
    off += ln
else: sys.exit('bad ROM')
init = struct.unpack('<H', d[off+2:off+4])[0] * 512
sub, mach, comp = struct.unpack('<HHH', d[off+8:off+14])      # subsystem, machine, compression
hdr = struct.unpack('<H', d[off+0x16:off+0x18])[0]
pay = d[off+hdr:off+init]
if comp == 1:
    pay = ec.EfiDecompress(pay, len(pay))
assert pay[:2] == b'MZ', 'not a PE'
pe = struct.unpack('<I', pay[0x3c:0x40])[0]
pm = struct.unpack('<H', pay[pe+4:pe+6])[0]
open(sys.argv[2], 'wb').write(pay)
print('%s: rom machine=%04x pe machine=%04x subsystem=%d compressed=%d -> %d bytes' % (sys.argv[1], mach, pm, sub, comp, len(pay)))
