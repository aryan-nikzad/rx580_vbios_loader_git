/*
 * vbios_loader.c - UEFI app: POST an AMD Polaris GPU whose SPI vBIOS chip is dead.
 *
 * ROM sources : every <dir of this .efi>\vbioses\*.rom (sorted by name).
 *               No firmware is embedded in this source tree; the user supplies the ROM files.
 * AUTO mode   : tries ROMs one by one.  For each: publish ROM (PciIo->RomImage +
 *               ACPI VFCT), run the ROM's UEFI GOP driver, ConnectController().
 *               If the GPU init hangs (TIMEOUT_SECS) the timer callback resets the
 *               machine; progress is kept in an NVRAM variable so the next boot tries
 *               the next ROM.  The ROM that works is remembered and used first.
 * Menu keys   : UP/DOWN+ENTER = run one ROM, V = VFCT-only, R = forget state, ESC = skip.
 */
#include <efi.h>
#include <efilib.h>

#define NARGS_(_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,N,...) N
#define NARGS(...) NARGS_(__VA_ARGS__,10,9,8,7,6,5,4,3,2,1,0)
#define FW(f, ...) uefi_call_wrapper((f), NARGS(__VA_ARGS__), __VA_ARGS__)   /* MS ABI */

static void trace_write(const CHAR16 *msg);
static void post_code(UINT8 v);
#define TRACE(...) do { CHAR16 _tb[160]; SPrint(_tb, sizeof _tb, __VA_ARGS__); trace_write(_tb); } while (0)

#define AMD_VID       0x1002
#define TIMEOUT_SECS  12
#define MENU_SECS     5
#define STATE_MAGIC   0x334C4256u
#define MAX_ROM_BYTES (2u * 1024 * 1024)

/* ---------- protocols not in gnu-efi ---------- */
typedef struct _DECOMP DECOMP;
struct _DECOMP {
  EFI_STATUS (EFIAPI *GetInfo)(DECOMP *, VOID *, UINT32, UINT32 *, UINT32 *);
  EFI_STATUS (EFIAPI *Decompress)(DECOMP *, VOID *, UINT32, VOID *, UINT32, VOID *, UINT32);
};
static EFI_GUID gDecomp = {0xd8117cfe,0x94a6,0x11d4,{0x9a,0x3a,0x00,0x90,0x27,0x3f,0xc1,0x4d}};
typedef struct _ACPITBL ACPITBL;
struct _ACPITBL {
  EFI_STATUS (EFIAPI *Install)(ACPITBL *, VOID *, UINTN, UINTN *);
  EFI_STATUS (EFIAPI *Uninstall)(ACPITBL *, UINTN);
};
static EFI_GUID gAcpiTbl = {0x31ce593d,0x108a,0x485d,{0xad,0xb2,0x78,0xf2,0x1f,0x29,0x66,0xbe}};
static EFI_GUID gStateGuid = {0x5b6c2a7e,0x9d34,0x4f1a,{0x8e,0x21,0x7a,0x0c,0x3d,0x44,0xb1,0x90}};

/* ---------- persistent state (NVRAM) ---------- */
#define NRES 64
/* res: 0 untried, 1 hung/unknown, 2 unusable ROM, 3 failed, 4 OK, 6 init ran but stuck loops had to be broken */
typedef struct { UINT32 magic, next, strikes; CHAR16 working[80]; UINT8 res[NRES]; UINT32 membefore[NRES]; UINT8 loops[NRES]; } STATE;
static STATE g_st;
static void state_load(void)
{
  UINTN sz = sizeof(STATE); UINT32 at;
  if (EFI_ERROR(FW(RT->GetVariable, L"VbiosLdrState", &gStateGuid, &at, &sz, &g_st)) ||
      sz != sizeof(STATE) || g_st.magic != STATE_MAGIC) {
    ZeroMem(&g_st, sizeof g_st); g_st.magic = STATE_MAGIC;
  }
}
static void state_save(void)
{
  FW(RT->SetVariable, L"VbiosLdrState", &gStateGuid,
     EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
     sizeof(STATE), &g_st);
}

/* ---------- candidates ---------- */
typedef struct { CHAR16 name[72]; } CAND;
static CAND *cands; static UINTN ncand;
static EFI_FILE_HANDLE g_root; static CHAR16 *g_romdir; static EFI_FILE_HANDLE g_vol_root;   /* g_vol_root: volume the .efi was started from */

static UINT16 rd16(const UINT8 *p) { return p[0] | (p[1] << 8); }

static CHAR16 *own_dir(EFI_LOADED_IMAGE *li)
{
  CHAR16 *last = NULL; EFI_DEVICE_PATH *dp = li->FilePath;
  for (; dp && !IsDevicePathEnd(dp); dp = NextDevicePathNode(dp))
    if (DevicePathType(dp) == MEDIA_DEVICE_PATH && DevicePathSubType(dp) == MEDIA_FILEPATH_DP)
      last = ((FILEPATH_DEVICE_PATH *)dp)->PathName;
  if (!last) return StrDuplicate(L"");
  CHAR16 *d = StrDuplicate(last); INTN i = (INTN)StrLen(d) - 1;
  while (i >= 0 && d[i] != L'\\') i--;
  if (i < 0) d[0] = 0; else d[i] = 0;              /* drop file name */
  return d;
}

static BOOLEAN ends_rom(const CHAR16 *n)
{
  UINTN l = StrLen(n);
  return l > 4 && (n[l-4] == L'.') && (n[l-3] | 0x20) == L'r' && (n[l-2] | 0x20) == L'o' && (n[l-1] | 0x20) == L'm';
}

static EFI_GUID gSfs = {0x964e5b22,0x6459,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
static UINTN g_nvol, g_ndirs; static CHAR16 *g_owndir;

static BOOLEAN open_dir(EFI_FILE_HANDLE root, const CHAR16 *path, EFI_FILE_HANDLE *dh)
{
  return !EFI_ERROR(FW(root->Open, root, dh, (CHAR16 *)(path[0] ? path : L"\\"), EFI_FILE_MODE_READ, 0));
}

/* add every *.rom in root:path to the candidate list; remember volume+dir on first hit */
static UINTN add_dir_roms(EFI_FILE_HANDLE root, CHAR16 *path)
{
  EFI_FILE_HANDLE dh; UINTN added = 0;
  g_ndirs++;
  if (!open_dir(root, path, &dh)) return 0;
  UINT8 *buf = AllocatePool(1024);
  for (;;) {
    UINTN sz = 1024;
    if (EFI_ERROR(FW(dh->Read, dh, &sz, buf)) || !sz) break;
    EFI_FILE_INFO *fi = (EFI_FILE_INFO *)buf;
    if ((fi->Attribute & EFI_FILE_DIRECTORY) || !ends_rom(fi->FileName)) continue;
    if (StrLen(fi->FileName) >= 70 || ncand >= 256) continue;
    StrCpy(cands[ncand].name, fi->FileName); ncand++; added++;
  }
  FW(dh->Close, dh); FreePool(buf);
  if (added) { g_root = root; g_romdir = StrDuplicate(path); }
  return added;
}

/* depth-limited search for a directory named "vbioses" */
static UINTN search_tree(EFI_FILE_HANDLE root, CHAR16 *path, UINTN depth)
{
  EFI_FILE_HANDLE dh; UINTN found = 0;
  if (g_ndirs > 200 || !open_dir(root, path, &dh)) return 0;
  g_ndirs++;
  UINT8 *buf = AllocatePool(1024);
  CHAR16 (*subs)[72] = AllocateZeroPool(sizeof(CHAR16) * 72 * 40); UINTN ns = 0;
  for (;;) {
    UINTN sz = 1024;
    if (EFI_ERROR(FW(dh->Read, dh, &sz, buf)) || !sz) break;
    EFI_FILE_INFO *fi = (EFI_FILE_INFO *)buf;
    if (!(fi->Attribute & EFI_FILE_DIRECTORY) || fi->FileName[0] == L'.' || StrLen(fi->FileName) >= 70) continue;
    if (ns < 40) StrCpy(subs[ns++], fi->FileName);
  }
  FW(dh->Close, dh); FreePool(buf);
  for (UINTN i = 0; i < ns && !found; i++) {              /* a folder with the right name here? */
    if (!StriCmp(subs[i], L"vbioses")) {
      CHAR16 *sub = PoolPrint(L"%s\\%s", path, subs[i]);
      found = add_dir_roms(root, sub); FreePool(sub);
    }
  }
  for (UINTN i = 0; i < ns && !found && depth < 3; i++) {
    if (!StriCmp(subs[i], L"vbioses")) continue;
    CHAR16 *sub = PoolPrint(L"%s\\%s", path, subs[i]);
    found = search_tree(root, sub, depth + 1); FreePool(sub);
  }
  FreePool(subs);
  return found;
}

static void scan_folder(EFI_HANDLE image)
{
  EFI_LOADED_IMAGE *li = NULL; EFI_HANDLE *vols = NULL; UINTN nv = 0;
  cands = AllocateZeroPool(sizeof(CAND) * 256); ncand = 0; g_owndir = L"?";
  if (EFI_ERROR(FW(BS->HandleProtocol, image, &gEfiLoadedImageProtocolGuid, (VOID **)&li))) li = NULL;
  FW(BS->LocateHandleBuffer, ByProtocol, &gSfs, NULL, &nv, &vols);
  g_nvol = nv;

  if (li) {                                            /* 1) own volume: next to the .efi, then usual places */
    EFI_FILE_HANDLE root = LibOpenRoot(li->DeviceHandle);
    g_vol_root = root;
    if (root) {
      g_owndir = own_dir(li);
      CHAR16 *p[4] = { PoolPrint(L"%s\\vbioses", g_owndir), StrDuplicate(L"\\vbioses"),
                       StrDuplicate(L"\\EFI\\vbioses"), StrDuplicate(L"\\EFI\\BOOT\\vbioses") };
      for (UINTN t = 0; t < 4 && !ncand; t++) add_dir_roms(root, p[t]);
      if (!ncand) search_tree(root, L"", 0);
    }
  }
  for (UINTN v = 0; v < nv && !ncand; v++) {           /* 2) search every other FAT volume */
    EFI_FILE_HANDLE root = LibOpenRoot(vols[v]);
    if (root) search_tree(root, L"", 0);
  }
  for (UINTN i = 1; i < ncand; i++) {                  /* sort by name, case-insensitive */
    CAND k = cands[i]; INTN j = (INTN)i - 1;
    while (j >= 0 && StriCmp(cands[j].name, k.name) > 0) { cands[j+1] = cands[j]; j--; }
    cands[j+1] = k;
  }
}

/* ---------- ROM parsing ---------- */
/* Walk option-ROM chain. Returns UEFI image (code type 3) or NULL; *end = real ROM length. */
static const UINT8 *parse_rom(const UINT8 *rom, UINTN size, UINT16 *vid, UINT16 *did, UINTN *end)
{
  UINTN off = 0; BOOLEAN first = TRUE; const UINT8 *efi = NULL; *end = 0;
  while (off + 0x1C <= size && rom[off] == 0x55 && rom[off+1] == 0xAA) {
    UINTN pcir = off + rd16(rom + off + 0x18);
    if (pcir + 0x18 > size || CompareMem((VOID *)(rom + pcir), "PCIR", 4)) break;
    if (first) { *vid = rd16(rom + pcir + 4); *did = rd16(rom + pcir + 6); first = FALSE; }
    UINTN len = (UINTN)rd16(rom + pcir + 0x10) * 512;
    if (!len || off + len > size) break;
    if (rom[pcir + 0x14] == 0x03 && !efi) efi = rom + off;
    off += len; *end = off;
    if (rom[pcir + 0x15] & 0x80) break;
  }
  return efi;
}

/* UEFI image -> raw PE (decompress with firmware protocol if flagged) */
static EFI_STATUS extract_pe(const UINT8 *img, VOID **pe, UINTN *pesz)
{
  UINT16 mach = rd16(img + 0x0A), comp = rd16(img + 0x0C), hdr = rd16(img + 0x16);
  UINTN init = (UINTN)rd16(img + 2) * 512;
  if (rd16(img + 4) != 0x0EF1) return EFI_UNSUPPORTED;
  if (mach != 0x8664) return EFI_UNSUPPORTED;                 /* x64 only */
  if (hdr >= init) return EFI_VOLUME_CORRUPTED;
  UINT8 *src = (UINT8 *)img + hdr; UINTN srclen = init - hdr;
  if (!comp) {
    *pe = AllocatePool(srclen); if (!*pe) return EFI_OUT_OF_RESOURCES;
    CopyMem(*pe, src, srclen); *pesz = srclen; return EFI_SUCCESS;
  }
  DECOMP *d; EFI_STATUS s = FW(BS->LocateProtocol, &gDecomp, NULL, (VOID **)&d);
  if (EFI_ERROR(s)) return s;
  UINT32 dsz, ssz;
  s = FW(d->GetInfo, d, src, (UINT32)srclen, &dsz, &ssz); if (EFI_ERROR(s)) return s;
  VOID *out = AllocatePool(dsz), *scr = AllocatePool(ssz);
  if (!out || !scr) return EFI_OUT_OF_RESOURCES;
  s = FW(d->Decompress, d, src, (UINT32)srclen, out, dsz, scr, ssz);
  FreePool(scr);
  if (EFI_ERROR(s)) return s;
  *pe = out; *pesz = dsz; return EFI_SUCCESS;
}

typedef struct { UINT8 *rom; UINTN romsz; VOID *pe; UINTN pesz; UINT16 vid, did; UINT8 *full; UINTN fullsz; } ROMDATA;

static EFI_STATUS get_rom(UINTN i, ROMDATA *r)
{
  ZeroMem(r, sizeof *r);
  const UINT8 *efi; UINTN end;
  CHAR16 *path = PoolPrint(L"%s\\%s", g_romdir, cands[i].name);
  EFI_FILE_HANDLE f; EFI_STATUS s = FW(g_root->Open, g_root, &f, path, EFI_FILE_MODE_READ, 0);
  FreePool(path); if (EFI_ERROR(s)) return s;
  EFI_FILE_INFO *fi = LibFileInfo(f);
  UINTN sz = fi ? (UINTN)fi->FileSize : 0; if (fi) FreePool(fi);
  if (sz < 0x1000 || sz > MAX_ROM_BYTES) { FW(f->Close, f); return EFI_BAD_BUFFER_SIZE; }
  UINT8 *buf = AllocatePool(sz); if (!buf) { FW(f->Close, f); return EFI_OUT_OF_RESOURCES; }
  s = FW(f->Read, f, &sz, buf); FW(f->Close, f);
  if (EFI_ERROR(s)) { FreePool(buf); return s; }
  efi = parse_rom(buf, sz, &r->vid, &r->did, &end);
  if (!end) { FreePool(buf); return EFI_VOLUME_CORRUPTED; }
  r->rom = buf; r->romsz = end; r->full = buf; r->fullsz = sz;     /* full = whole chip image incl. data past the BIOS images */
  if (!efi) return EFI_NOT_FOUND;
  return extract_pe(efi, &r->pe, &r->pesz);
}

/* ---------- ACPI VFCT ---------- */
static BOOLEAN g_vfct_have; static UINTN g_vfct_key;
static EFI_STATUS install_vfct(const UINT8 *rom, UINTN romsz, UINTN bus, UINTN dev, UINTN fn,
                               UINT16 vid, UINT16 did, UINT16 ssvid, UINT16 ssid)
{
  ACPITBL *at; EFI_STATUS s = FW(BS->LocateProtocol, &gAcpiTbl, NULL, (VOID **)&at);
  if (EFI_ERROR(s)) return s;
  UINTN hdrsz = 76, imgh = 28, total = hdrsz + imgh + romsz;
  UINT8 *t = AllocateZeroPool(total); if (!t) return EFI_OUT_OF_RESOURCES;
  CopyMem(t, "VFCT", 4);
  *(UINT32 *)(t + 4) = (UINT32)total; t[8] = 1;
  CopyMem(t + 10, "AMDGPU", 6); CopyMem(t + 16, "VBIOSLDR", 8);
  *(UINT32 *)(t + 24) = 1; CopyMem(t + 28, "VLDR", 4); *(UINT32 *)(t + 32) = 1;
  *(UINT32 *)(t + 52) = (UINT32)hdrsz;
  UINT8 *h = t + hdrsz;
  *(UINT32 *)(h + 0) = (UINT32)bus; *(UINT32 *)(h + 4) = (UINT32)dev; *(UINT32 *)(h + 8) = (UINT32)fn;
  *(UINT16 *)(h + 12) = vid; *(UINT16 *)(h + 14) = did;
  *(UINT16 *)(h + 16) = ssvid; *(UINT16 *)(h + 18) = ssid;
  *(UINT32 *)(h + 24) = (UINT32)romsz;
  CopyMem(h + imgh, (VOID *)rom, romsz);
  UINT8 sum = 0; for (UINTN i = 0; i < total; i++) sum += t[i];
  t[9] = (UINT8)(0 - sum);
  if (g_vfct_have) { FW(at->Uninstall, at, g_vfct_key); g_vfct_have = FALSE; }
  UINTN key = 0; s = FW(at->Install, at, t, total, &key);
  if (!EFI_ERROR(s)) { g_vfct_key = key; g_vfct_have = TRUE; }
  FreePool(t); return s;
}

/* ---------- GPU register access + watchdog heartbeat ---------- */
static UINT64 g_mmio; static UINTN g_sec;
static UINT32 rreg(UINT32 off)           /* BAR5 = MMIO registers on Polaris; read via physical address */
{
  if (!g_mmio) return 0xDEADDEAD;
  return *(volatile UINT32 *)(UINTN)(g_mmio + off);
}
#define R_MEMSIZE 0x5428
#define R_GRBM    0x8010
#define R_SRBM    0x0E50
#define R_SCRATCH 0x1724
static void regs_line(const CHAR16 *tag)
{
  Print(L"  %s MEMSIZE=%08x GRBM=%08x SRBM=%08x SCRATCH0=%08x\n", tag,
        rreg(R_MEMSIZE), rreg(R_GRBM), rreg(R_SRBM), rreg(R_SCRATCH));
}
/* ---------- embedded AtomBIOS interpreter (atomlib/atom.c, from the Linux amdgpu driver) ---------- */
struct card_info {
  void *dev;
  void (*reg_write)(struct card_info *, UINT32, UINT32);  UINT32 (*reg_read)(struct card_info *, UINT32);
  void (*mc_write)(struct card_info *, UINT32, UINT32);   UINT32 (*mc_read)(struct card_info *, UINT32);
  void (*pll_write)(struct card_info *, UINT32, UINT32);  UINT32 (*pll_read)(struct card_info *, UINT32);
};
struct atom_context;
extern struct atom_context *amdgpu_atom_parse(struct card_info *, void *);
extern int amdgpu_atom_asic_init(struct atom_context *);
extern void amdgpu_atom_destroy(struct atom_context *);
extern int atom_break_loops, atom_loops_broken; extern unsigned atom_loop_ms;
extern unsigned long atom_now_ms(void);
#define ATOM_LOOP_MS 300
static BOOLEAN g_force = FALSE;           /* F: run the init even if the GPU already looks fully initialised */
static BOOLEAN g_postdump = FALSE;        /* P: snapshot all registers after init (a diagnostic; can itself freeze the PC) */
static BOOLEAN g_engine_atom = TRUE;      /* E: own interpreter (default) vs firmware GOP driver */

/* ---------- memory-controller microcode ----------
 * The SPI ROM chip also holds the MC sequencer firmware (polaris10_mc.bin payload) at ROM offset 0x37000:
 *   [ucode version, n_io_pairs, ucode_dwords, total_dwords] [n_io_pairs x (index,value)] [ucode dwords]
 * ASIC_Init's MC_SEQ_Control hands that offset to the GPU (SMC reg 0xC0600010) and the hardware pulls it from the
 * chip. With a dead chip nothing arrives and the MC never trains, so we do the hardware's job from the ROM file. */
static UINT32 g_smc_idx1;
static const UINT8 *g_mc_fallback; static UINTN g_mc_fallback_sz; static INTN g_mc_fallback_off;
static void mmio_w(UINT32 reg, UINT32 v) { UINT64 o = (UINT64)reg * 4; if (g_mmio && o + 4 <= 0x40000) *(volatile UINT32 *)(UINTN)(g_mmio + o) = v; }
static BOOLEAN mc_block_ok(const UINT8 *full, UINTN fullsz, UINT32 off)
{
  if (!full || (UINTN)off + 16 > fullsz) return FALSE;
  const UINT32 *h = (const UINT32 *)(full + off);
  UINT32 n_io = h[1], ucw = h[2], tot = h[3];
  if (n_io == 0 || n_io > 64 || ucw == 0 || ucw > 0x4000) return FALSE;
  if (16 + n_io * 8 + ucw * 4 != tot * 4) return FALSE;
  return (UINTN)off + tot * 4 <= fullsz;
}
/* offset of a valid MC block: the requested one first, then any 4 KB-aligned spot past the BIOS images; -1 if none */
static INTN mc_find(const UINT8 *full, UINTN fullsz, UINT32 preferred)
{
  if (mc_block_ok(full, fullsz, preferred)) return (INTN)preferred;
  for (UINTN o = 0x1D000; o + 0x1000 <= fullsz; o += 0x1000) if (mc_block_ok(full, fullsz, (UINT32)o)) return (INTN)o;
  return -1;
}
/* first ROM file in the folder that carries a valid MC block (your original dump sorts first) */
static void find_mc_fallback(void)
{
  static BOOLEAN tried;
  if (tried || !g_root) return;
  tried = TRUE;
  for (UINTN i = 0; i < ncand; i++) {
    CHAR16 *path = PoolPrint(L"%s\\%s", g_romdir, cands[i].name);
    EFI_FILE_HANDLE f; EFI_STATUS st = FW(g_root->Open, g_root, &f, path, EFI_FILE_MODE_READ, 0);
    FreePool(path);
    if (EFI_ERROR(st)) continue;
    EFI_FILE_INFO *fi = LibFileInfo(f); UINTN sz = fi ? (UINTN)fi->FileSize : 0; if (fi) FreePool(fi);
    if (sz < 0x38000 || sz > MAX_ROM_BYTES) { FW(f->Close, f); continue; }
    UINT8 *buf = AllocatePool(sz);
    if (!buf) { FW(f->Close, f); continue; }
    st = FW(f->Read, f, &sz, buf); FW(f->Close, f);
    INTN fo = EFI_ERROR(st) ? -1 : mc_find(buf, sz, 0x37000);
    if (fo >= 0) { g_mc_fallback = buf; g_mc_fallback_sz = sz; g_mc_fallback_off = fo; return; }
    FreePool(buf);
  }
}
/* ---------- emulation of the chip's SPI read port + register-copy engine ----------
 * What a normal boot does (decoded from the ROM tables, see README):
 *   1. SMC_IND_INDEX_1 = ROM_INDEX (0xC0600010); data = byte offset in the ROM  (here 0x37000)
 *   2. SMC_IND_INDEX_1 = ROM_DATA  (0xC0600014); every read of SMC_IND_DATA_1 returns the next dword (auto-increment)
 *      -> the tables read the 4-dword header and 24 (index,value) pairs and write them to MC_SEQ_IO_DEBUG
 *   3. regs 0xC064/0xC066 = source/destination register byte addresses (0x20C = SMC_IND_DATA_1, 0x28CC = MC_SEQ_SUP_PGM),
 *      reg 0xC0E8 = byte count (0x7E1C)  -> a hardware copy engine streams the microcode body ROM -> MC_SEQ_SUP_PGM
 *   4. MC_SEQ_SUP_CNTL = 8, 4, 1 starts the sequencer.
 * With a dead chip steps 2 and 3 move garbage. We serve both from the ROM file instead. */
static UINT8 *g_spi; static UINT32 g_rom_idx; static UINT32 g_cp[4]; static UINTN g_cp_n; static UINT32 g_cp_src, g_cp_dst;
#define SPI_SIZE 0x40000
static void spi_setup(const UINT8 *full, UINTN fullsz)
{
  if (g_spi) FreePool(g_spi);
  g_spi = AllocatePool(SPI_SIZE);
  if (!g_spi) return;
  SetMem(g_spi, SPI_SIZE, 0xFF);
  CopyMem(g_spi, (VOID *)full, fullsz > SPI_SIZE ? SPI_SIZE : fullsz);
  g_rom_idx = 0; g_cp_n = 0; g_smc_idx1 = 0;
}
static UINT32 spi_next(void)
{
  UINT32 v = (g_rom_idx + 4 <= SPI_SIZE) ? *(UINT32 *)(g_spi + g_rom_idx) : 0xFFFFFFFFu;
  g_rom_idx += 4; return v;
}
/* the ROM may ask for its MC block at an offset where this file has none (e.g. trimmed other-brand ROMs):
 * serve the block from another ROM file at the requested offset */
static void spi_overlay_if_needed(UINT32 off)
{
  if (off < 0x1D000 || off >= SPI_SIZE || mc_block_ok(g_spi, SPI_SIZE, off)) return;
  find_mc_fallback();
  if (!g_mc_fallback || g_mc_fallback_off < 0) { Print(L"   (ROM asks for data at 0x%x that this file lacks and no other file has the MC block)\n", off); return; }
  const UINT32 *h = (const UINT32 *)(g_mc_fallback + g_mc_fallback_off);
  UINTN bytes = (UINTN)h[3] * 4;
  if (off + bytes <= SPI_SIZE) { CopyMem(g_spi + off, (VOID *)(g_mc_fallback + g_mc_fallback_off), bytes); Print(L"   (this ROM file lacks the MC block: serving the copy from another ROM file)\n"); }
}

static UINT32 c_rr(struct card_info *c, UINT32 reg)
{
  if (g_spi && reg == 0x83) {                                                /* SMC_IND_DATA_1 */
    if (g_smc_idx1 == 0xC0600014u) return spi_next();                        /* ROM_DATA  */
    if (g_smc_idx1 == 0xC0600010u) return g_rom_idx;                         /* ROM_INDEX */
  }
  UINT64 o = (UINT64)reg * 4; return (!g_mmio || o + 4 > 0x40000) ? 0 : *(volatile UINT32 *)(UINTN)(g_mmio + o);
}
static void   c_rw(struct card_info *c, UINT32 reg, UINT32 v)
{
  if (g_spi) {
    if (reg == 0x82) g_smc_idx1 = v;                                         /* SMC_IND_INDEX_1: forward too */
    else if (reg == 0x83 && g_smc_idx1 == 0xC0600010u) {                     /* ROM_INDEX <- byte offset */
      g_rom_idx = v; spi_overlay_if_needed(v); post_code(0x21); TRACE(L"ROM_INDEX <- %08x", v); return;
    }
    else if (reg >= 0xC064 && reg <= 0xC067) { g_cp[reg - 0xC064] = v; g_cp_n |= 1u << (reg - 0xC064); if (reg == 0xC064) g_cp_src = v; if (reg == 0xC066) g_cp_dst = v; return; }
    else if (reg == 0xC0E8) {
      UINT32 cnt = v & 0x03FFFFFFu;
      if (g_cp_src == 0x20C && g_cp_dst == 0x28CC && g_smc_idx1 == 0xC0600014u) {      /* ROM_DATA -> MC_SEQ_SUP_PGM */
        post_code(0x22); TRACE(L"copy engine: %d bytes ROM@%05x -> MC_SEQ_SUP_PGM", cnt, g_rom_idx);
        for (UINT32 i = 0; i < cnt / 4; i++) mmio_w(0xA33, spi_next());
        post_code(0x23); TRACE(L"copy engine: done");
        Print(L"   MC microcode: copy engine emulated, %d bytes from ROM offset 0x%x into MC_SEQ_SUP_PGM\n", cnt, g_rom_idx - cnt);
        g_cp_n = 0; return;
      }
      for (UINTN i = 0; i < 4; i++) if (g_cp_n & (1u << i)) mmio_w(0xC064 + (UINT32)i, g_cp[i]);   /* some other use: let hardware do it */
      g_cp_n = 0;
    }
  }
  mmio_w(reg, v);
}
static UINT32 c_ir(struct card_info *c, UINT32 reg) { return 0; }                 /* MC / PLL indirect: stubs, as in amdgpu */
static void   c_iw(struct card_info *c, UINT32 reg, UINT32 v) { }


/* ---- what Linux's amdgpu looks at when it starts (vi.c / amdgpu_atombios.c) ---- */
#define SMC_IND_INDEX_0   0x80
#define SMC_IND_DATA_0    0x81
#define ixSMC_RESET_CNTL  0x80000000u
#define ixSMC_CLOCK_CNTL0 0x80000004u
#define ixSMC_PC_C        0x80000370u
#define BIOS_SCRATCH_7    0x5D0
#define S7_INIT_COMPLETE  0x200u
static UINT32 smc_rd(UINT32 a) { c_rw(NULL, SMC_IND_INDEX_0, a); return c_rr(NULL, SMC_IND_DATA_0); }
static void   smc_wr(UINT32 a, UINT32 v) { c_rw(NULL, SMC_IND_INDEX_0, a); c_rw(NULL, SMC_IND_DATA_0, v); }
static BOOLEAN smc_running(void) { return !(smc_rd(ixSMC_CLOCK_CNTL0) & 1) && smc_rd(ixSMC_PC_C) >= 0x20100; }

/* Linux resets the whole GPU at probe if the SMC looks running ("PCI CONFIG reset"), which would wipe our init.
 * Make the GPU look "initialised, SMC stopped" instead. */
static void prepare_for_os(void)
{
  post_code(0x60); TRACE(L"prepare_for_os");
  UINT32 misc0 = c_rr(NULL, 0xA80), s4 = c_rr(NULL, 0x5CD);               /* MC_SEQ_MISC0, BIOS_SCRATCH_4 */
  Print(L"   MC_SEQ_MISC0=%08x BIOS_SCRATCH_4=%08x   (reference: your Samsung SMD2 original gives 5060A1F2 / 00010000)\n", misc0, s4);
  UINT32 clk = smc_rd(ixSMC_CLOCK_CNTL0), pc = smc_rd(ixSMC_PC_C), s7 = c_rr(NULL, BIOS_SCRATCH_7);
  Print(L"   OS-visible state: SMC clock_cntl0=%08x pc=%08x  BIOS_SCRATCH_7=%08x\n", clk, pc, s7);
  if (smc_running()) {
    Print(L"   SMC looks RUNNING -> Linux would reset the GPU at probe and discard our init. Halting the SMC.\n");
    smc_wr(ixSMC_RESET_CNTL, smc_rd(ixSMC_RESET_CNTL) | 1);
    smc_wr(ixSMC_CLOCK_CNTL0, smc_rd(ixSMC_CLOCK_CNTL0) | 1);
    Print(L"   after halt: SMC clock_cntl0=%08x pc=%08x  running=%s\n", smc_rd(ixSMC_CLOCK_CNTL0), smc_rd(ixSMC_PC_C), smc_running() ? L"YES" : L"no");
  }
  s7 = c_rr(NULL, BIOS_SCRATCH_7);
  Print(L"   ASIC_INIT_COMPLETE flag (BIOS_SCRATCH_7 bit 9): %s -> Linux will %s the GPU\n",
        (s7 & S7_INIT_COMPLETE) ? L"SET" : L"NOT set", (s7 & S7_INIT_COMPLETE) ? L"SKIP POSTing" : L"POST again (and stall at SetVoltage)");
}


/* MC sequencer status, printed whenever a stuck poll is broken (see MC_SEQ_Control analysis) */
#define R_MC_SEQ_MISC9   0xAE7
#define R_MC_SEQ_SUPCNTL 0xA32
#define R_MC_SEQ_CMD     0xA31
#define R_MC_SEQ_STATUSM 0xA7D
#define R_MC_SEQ_MISC0   0xA80
#define R_MC_SEQ_MISC1   0xA81
#define R_MC_TRAIN_WAKE  0xA3A      /* good/trained value: C00000E0 */
void loader_loop_diag(void)
{
  post_code(0x30);
  TRACE(L"loop broken: MISC9=%08x SUP_CNTL=%08x CMD=%08x STATUS_M=%08x WAKE=%08x", c_rr(NULL, R_MC_SEQ_MISC9), c_rr(NULL, R_MC_SEQ_SUPCNTL), c_rr(NULL, R_MC_SEQ_CMD), c_rr(NULL, R_MC_SEQ_STATUSM), c_rr(NULL, R_MC_TRAIN_WAKE));
  Print(L"      MC: MISC9=%08x SUP_CNTL=%08x CMD=%08x STATUS_M=%08x WAKE=%08x\n"
        L"          (trained = MISC9 11000707, SUP_CNTL 27800001, CMD 00030000, STATUS_M 00010300, WAKE C00000E0)\n",
        c_rr(NULL, R_MC_SEQ_MISC9), c_rr(NULL, R_MC_SEQ_SUPCNTL), c_rr(NULL, R_MC_SEQ_CMD),
        c_rr(NULL, R_MC_SEQ_STATUSM), c_rr(NULL, R_MC_TRAIN_WAKE));
}

/* ---------- reliable reset (ResetSystem can deadlock inside a timer callback) ---------- */
static void outb(UINT16 port, UINT8 v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(port)); }
static void hard_reset(void)
{
  outb(0xCF9, 0x02); FW(BS->Stall, 100);       /* chipset reset-control register */
  outb(0xCF9, 0x0E); FW(BS->Stall, 300000);
  outb(0x64, 0xFE);  FW(BS->Stall, 300000);    /* keyboard-controller reset line */
  FW(RT->ResetSystem, EfiResetCold, EFI_SUCCESS, 0, NULL);
  FW(BS->Stall, 1000000);
  struct { UINT16 l; UINT64 b; } __attribute__((packed)) idt = { 0, 0 };   /* last resort: triple fault */
  __asm__ volatile("lidt %0; int3" :: "m"(idt));
}

static EFI_HANDLE g_image; static UINTN g_cur;
static BOOLEAN g_fired;

static void __attribute__((ms_abi)) heartbeat(EFI_EVENT e, VOID *c)
{
  g_sec++;
  if (!(g_sec & 1)) { CHAR16 t[16]; SPrint(t, sizeof t, L"[%ds]", g_sec); regs_line(t); }
  if (g_sec >= TIMEOUT_SECS && !g_fired) {
    g_fired = TRUE;
    Print(L"\n  TIMEOUT: firmware GOP driver did not finish in %d s -> resetting the whole machine\n", TIMEOUT_SECS);
    FW(BS->Stall, 2000000);
    hard_reset();
  }
}

/* AMD "PCI config reset": magic dword to config offset 0x7C resets the ASIC only (same as amdgpu/vi.c) */
static BOOLEAN gpu_reset(EFI_PCI_IO_PROTOCOL *pio)
{
  UINT16 cmd = 0, nobm; UINT32 key = 0x39D5E86B; BOOLEAN ok = FALSE;
  FW(pio->Pci.Read, pio, EfiPciIoWidthUint16, 4, 1, &cmd);
  nobm = cmd & ~0x4;
  FW(pio->Pci.Write, pio, EfiPciIoWidthUint16, 4, 1, &nobm);          /* bus master off */
  FW(pio->Pci.Write, pio, EfiPciIoWidthUint32, 0x7C, 1, &key);        /* reset */
  FW(BS->Stall, 2000);
  for (UINTN i = 0; i < 3000 && !ok; i++) {                           /* wait up to ~3 s for the ASIC to answer */
    if (rreg(R_MEMSIZE) != 0xFFFFFFFF) { FW(BS->Stall, 1000); ok = (rreg(R_MEMSIZE) != 0xFFFFFFFF); }
    else FW(BS->Stall, 1000);
  }
  FW(pio->Pci.Write, pio, EfiPciIoWidthUint16, 4, 1, &cmd);           /* bus master back on */
  return ok;
}

static EFI_STATUS dump_to(const CHAR16 *name, BOOLEAN progress);
static BOOLEAN reg_is_hazard(UINTN i);
/* ---------- running one candidate ---------- */
enum { RES_OK, RES_SKIP, RES_SYSTEMIC, RES_FAIL, RES_VFCT, RES_HUNG, RES_DEAD };

static UINTN run_cand(UINTN idx, BOOLEAN vfct_only, EFI_HANDLE image)
{
  EFI_STATUS s; ROMDATA r;
  Print(L"\n>> ROM %d/%d: %s\n", idx + 1, ncand, cands[idx].name);
  TRACE(L"ROM %d/%d %s", idx + 1, ncand, cands[idx].name); post_code(0x10);
  s = get_rom(idx, &r);
  if (EFI_ERROR(s)) { Print(L"   cannot use this ROM: %r\n", s); if (idx < NRES) { g_st.res[idx] = 2; state_save(); } return RES_SKIP; }
  if (r.vid != AMD_VID) { Print(L"   not an AMD ROM\n"); return RES_SKIP; }

  UINTN n = 0; EFI_HANDLE *hs = NULL;
  s = FW(BS->LocateHandleBuffer, ByProtocol, &gEfiPciIoProtocolGuid, NULL, &n, &hs);
  if (EFI_ERROR(s)) { Print(L"   no PCI IO handles\n"); return RES_SYSTEMIC; }
  EFI_HANDLE gpu = NULL; EFI_PCI_IO_PROTOCOL *pio = NULL;
  for (UINTN i = 0; i < n; i++) {
    EFI_PCI_IO_PROTOCOL *p; UINT16 v, d; UINT8 cls;
    if (EFI_ERROR(FW(BS->HandleProtocol, hs[i], &gEfiPciIoProtocolGuid, (VOID **)&p))) continue;
    FW(p->Pci.Read, p, EfiPciIoWidthUint16, 0, 1, &v);
    FW(p->Pci.Read, p, EfiPciIoWidthUint16, 2, 1, &d);
    FW(p->Pci.Read, p, EfiPciIoWidthUint8, 0x0B, 1, &cls);
    if (v == AMD_VID && cls == 0x03 && d == r.did) { gpu = hs[i]; pio = p; break; }
  }
  if (!gpu) { Print(L"   no AMD GPU with device id %04x found\n", r.did); return RES_SYSTEMIC; }

  UINTN seg, bus, dev, fn; UINT16 ssv, ssi; UINT32 bar5 = 0;
  FW(pio->GetLocation, pio, &seg, &bus, &dev, &fn);
  FW(pio->Pci.Read, pio, EfiPciIoWidthUint16, 0x2C, 1, &ssv);
  FW(pio->Pci.Read, pio, EfiPciIoWidthUint16, 0x2E, 1, &ssi);
  FW(pio->Attributes, pio, EfiPciIoAttributeOperationEnable,
     EFI_PCI_IO_ATTRIBUTE_MEMORY | EFI_PCI_IO_ATTRIBUTE_IO | EFI_PCI_IO_ATTRIBUTE_BUS_MASTER, NULL);
  FW(pio->Pci.Read, pio, EfiPciIoWidthUint32, 0x24, 1, &bar5);
  g_mmio = (bar5 & 1) ? 0 : (bar5 & ~0xFu);
  Print(L"   GPU %02x:%02x.%x  %04x:%04x  subsys %04x:%04x  MMIO BAR5=%08x\n",
        bus, dev, fn, AMD_VID, r.did, ssv, ssi, (UINT32)g_mmio);

  TRACE(L"GPU %02x:%02x.%x mmio=%08x", bus, dev, fn, (UINT32)g_mmio); post_code(0x11);
  pio->RomImage = r.rom; pio->RomSize = r.romsz;          /* what PciBus would set from the ROM BAR */
  s = install_vfct(r.rom, r.romsz, bus, dev, fn, AMD_VID, r.did, ssv, ssi);
  Print(L"   VFCT table: %r\n", s); post_code(0x12);
  if (vfct_only) { Print(L"   VFCT-only mode: GOP driver skipped\n"); return RES_VFCT; }

  if (g_engine_atom) {
    UINT8 *bios = AllocateZeroPool(r.romsz + 0x4000);           /* padded: tables may read past the image end */
    if (!bios) return RES_SKIP;
    CopyMem(bios, r.rom, r.romsz);
    struct card_info card = { NULL, c_rw, c_rr, c_iw, c_ir, c_iw, c_ir };
    struct atom_context *actx = amdgpu_atom_parse(&card, bios);
    if (!actx) { Print(L"   AtomBIOS parse failed (not a usable ROM)\n"); FreePool(bios); if (idx < NRES) { g_st.res[idx] = 2; state_save(); } return RES_SKIP; }
    regs_line(L"before:");
    Print(L"   BIOS_SCRATCH_7 before init: %08x\n", c_rr(NULL, BIOS_SCRATCH_7));
    {   /* a GPU that a working ROM chip (or an earlier run) already brought up must not be re-initialised */
      UINT32 m9 = c_rr(NULL, R_MC_SEQ_MISC9), s7b = c_rr(NULL, BIOS_SCRATCH_7);
      BOOLEAN trained = (m9 & 0xFF) != 0 && (m9 & 0xFF) != 0xFF && (((m9 >> 8) & 0xFF) == (m9 & 0xFF));
      if (trained && (s7b & S7_INIT_COMPLETE) && !g_force) {
        Print(L"   GPU is already initialised and trained (MC_SEQ_MISC9=%08x) - leaving it untouched.\n", m9);
        TRACE(L"already initialised, skipped (MISC9=%08x)", m9);
        FreePool(bios);
        if (idx < NRES) { g_st.res[idx] = 4; state_save(); }
        return RES_OK;
      }
    }
    if (idx < NRES) { g_st.res[idx] = 1; g_st.membefore[idx] = rreg(R_MEMSIZE); state_save(); }
    spi_setup(r.full, r.fullsz);
    atom_break_loops = 1; atom_loop_ms = ATOM_LOOP_MS; atom_loops_broken = 0;
    Print(L"   running ASIC_Init in the built-in interpreter (a poll stuck > %d ms is skipped)...\n", ATOM_LOOP_MS);
    TRACE(L"ASIC_Init start"); post_code(0x20);
    unsigned long t0 = atom_now_ms();
    int rc = amdgpu_atom_asic_init(actx);
    post_code(0x40); TRACE(L"ASIC_Init returned %d, %d loops skipped", rc, atom_loops_broken);
    if (g_spi) { FreePool(g_spi); g_spi = NULL; }                                  /* back to the real hardware */
    unsigned long dt = atom_now_ms() - t0;
    BOOLEAN s7_ok = (c_rr(NULL, BIOS_SCRATCH_7) & S7_INIT_COMPLETE) != 0;
    Print(L"   ASIC_Init returned %d after %d ms, %d stuck loop(s) skipped\n", rc, (UINTN)dt, atom_loops_broken);
    regs_line(L"after: ");
    amdgpu_atom_destroy(actx);
    if (g_postdump) { post_code(0x50); TRACE(L"post-init dump start"); EFI_STATUS ds = dump_to(L"\\regdump_postinit.bin", FALSE); TRACE(L"post-init dump done"); Print(L"   register snapshot after init -> \\regdump_postinit.bin: %r\n", ds); }
    BOOLEAN good = (rc == 0) && s7_ok;
    if (idx < NRES) { g_st.res[idx] = good ? (atom_loops_broken ? 6 : 4) : 3; g_st.loops[idx] = (UINT8)(atom_loops_broken > 255 ? 255 : atom_loops_broken); state_save(); }
    if (good) {
      if (atom_loops_broken) Print(L"   NOTE: %d polling loop(s) never completed and were skipped (see SetVoltage analysis);\n"
                                   L"         the GPU may be running at its regulator's default voltage.\n", atom_loops_broken);
      prepare_for_os();
      return RES_OK;
    }
    prepare_for_os();
    Print(L"   init did not complete -> resetting only the GPU\n");
    BOOLEAN up = gpu_reset(pio);
    regs_line(L"post-reset:");
    if (!up) { Print(L"   GPU does not answer after reset\n"); return RES_DEAD; }
    return RES_HUNG;                                              /* next ROM in the same boot, no reboot */
  }


  EFI_HANDLE drv;
  s = FW(BS->LoadImage, FALSE, image, NULL, r.pe, r.pesz, &drv);
  Print(L"   LoadImage: %r\n", s);
  if (EFI_ERROR(s)) return RES_FAIL;
  s = FW(BS->StartImage, drv, NULL, NULL);
  Print(L"   StartImage: %r\n", s);
  if (EFI_ERROR(s)) return RES_FAIL;

  regs_line(L"before:");
  g_cur = idx; g_fired = FALSE; g_image = image;
  if (idx < NRES) { g_st.res[idx] = 1; g_st.membefore[idx] = rreg(R_MEMSIZE); state_save(); }
  Print(L"   ConnectController (GPU init, timeout %d s)...\n", TIMEOUT_SECS);
  EFI_EVENT hb; g_sec = 0;
  FW(BS->CreateEvent, EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, (EFI_EVENT_NOTIFY)heartbeat, NULL, &hb);
  FW(BS->SetTimer, hb, TimerPeriodic, 10000000);
  s = FW(BS->ConnectController, gpu, NULL, NULL, TRUE);
  FW(BS->SetTimer, hb, TimerCancel, 0); FW(BS->CloseEvent, hb);
  Print(L"   ConnectController: %r\n", s);
  regs_line(L"after: ");

  VOID *gop = NULL; UINT32 mem = rreg(R_MEMSIZE);
  BOOLEAN has_gop = !EFI_ERROR(FW(BS->HandleProtocol, gpu, &GraphicsOutputProtocol, &gop));
  BOOLEAN mem_ok = (mem >= 256 && mem <= 32768);
  Print(L"   GOP on GPU: %s   VRAM size register: %s\n", has_gop ? L"yes" : L"no", mem_ok ? L"valid" : L"invalid");
  BOOLEAN good = !EFI_ERROR(s) && (has_gop || mem_ok);
  if (idx < NRES) { g_st.res[idx] = good ? 4 : 3; state_save(); }
  return good ? RES_OK : RES_FAIL;
}


/* ---------- register dump for offline comparison (dead ROM chip vs working ROM chip) ---------- */
static EFI_PCI_IO_PROTOCOL *g_dump_pio;
static BOOLEAN find_gpu_basic(void)       /* locate the AMD GPU, enable MMIO, set g_mmio (no ROM needed) */
{
  UINTN n = 0; EFI_HANDLE *hs = NULL;
  if (EFI_ERROR(FW(BS->LocateHandleBuffer, ByProtocol, &gEfiPciIoProtocolGuid, NULL, &n, &hs))) return FALSE;
  for (UINTN i = 0; i < n; i++) {
    EFI_PCI_IO_PROTOCOL *p; UINT16 v, d; UINT8 cls; UINT32 bar5 = 0;
    if (EFI_ERROR(FW(BS->HandleProtocol, hs[i], &gEfiPciIoProtocolGuid, (VOID **)&p))) continue;
    FW(p->Pci.Read, p, EfiPciIoWidthUint16, 0, 1, &v); FW(p->Pci.Read, p, EfiPciIoWidthUint16, 2, 1, &d);
    FW(p->Pci.Read, p, EfiPciIoWidthUint8, 0x0B, 1, &cls);
    if (v != AMD_VID || cls != 0x03 || (d != 0x67DF && d != 0x6FDF)) continue;
    FW(p->Attributes, p, EfiPciIoAttributeOperationEnable, EFI_PCI_IO_ATTRIBUTE_MEMORY | EFI_PCI_IO_ATTRIBUTE_IO | EFI_PCI_IO_ATTRIBUTE_BUS_MASTER, NULL);
    FW(p->Pci.Read, p, EfiPciIoWidthUint32, 0x24, 1, &bar5);
    g_mmio = (bar5 & 1) ? 0 : (bar5 & ~0xFu); g_dump_pio = p;
    return g_mmio != 0;
  }
  return FALSE;
}

/* Registers that hang the CPU when read on a cold GPU (block is clock/power gated and never answers).
 * VCE (video encode): 0x8000..0x8FFF - found the hard way (reading 0x8001 froze the PC). */
static BOOLEAN reg_is_hazard(UINTN i) { return i >= 0x8000 && i < 0x9000; }

/* write all registers (minus hazards) to <name> on the loader's volume, chunk by chunk */
static EFI_STATUS dump_to(const CHAR16 *name, BOOLEAN progress)
{
  EFI_FILE_HANDLE root = g_vol_root ? g_vol_root : g_root;
  if (!root) return EFI_NOT_FOUND;
  EFI_FILE_HANDLE f; EFI_STATUS s = FW(root->Open, root, &f, (CHAR16 *)name, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
  if (EFI_ERROR(s)) return s;
  UINT32 *buf = AllocatePool(0x4000);
  if (!buf) { FW(f->Close, f); return EFI_OUT_OF_RESOURCES; }
  for (UINTN base = 0; base < 0x10000; base += 0x1000) {
    for (UINTN k = 0; k < 0x1000; k++) {
      UINTN i = base + k;
      if (progress && !(i & 0xFF)) Print(L"    reg 0x%04x   \r", (UINT32)i);
      if (!k) { post_code((UINT8)(0xD0 | (base >> 12))); TRACE(L"dump chunk 0x%04x", (UINT32)base); }   /* last number on screen = where it froze */
      buf[k] = reg_is_hazard(i) ? 0xBAD0BAD0u : c_rr(NULL, (UINT32)i);
    }
    UINTN sz = 0x4000; FW(f->Write, f, &sz, buf); FW(f->Flush, f);
  }
  /* indirect MC_SEQ_IO_DEBUG space (memory-PHY settings and training results live here, not in the 256 KB window):
   * index register 0xA91, data register 0xA92. Appended after the MMIO block: 0x400 dwords. */
  post_code(0xE0); TRACE(L"dump: MC IO_DEBUG indirect space");
  UINT32 saved_idx = c_rr(NULL, 0xA91);
  for (UINTN i = 0; i < 0x400; i++) { mmio_w(0xA91, (UINT32)i); buf[i & 0xFFF] = c_rr(NULL, 0xA92); }
  mmio_w(0xA91, saved_idx);
  { UINTN sz = 0x1000; FW(f->Write, f, &sz, buf); FW(f->Flush, f); }
  TRACE(L"dump: done");
  FW(f->Close, f); FreePool(buf);
  return EFI_SUCCESS;
}

static void dump_registers(void)
{
  if (!find_gpu_basic()) { Print(L"  No Polaris GPU found (or BAR5 unusable).\n"); return; }
  UINT32 memsz = c_rr(NULL, 0x150A);
  CHAR16 name[48]; SPrint(name, sizeof name, L"\\regdump_mem%08x.bin", memsz);
  Print(L"\n  Dumping GPU registers to %s (state: MEMSIZE=%08x).\n"
        L"  Written chunk by chunk, so a freeze keeps what was read. If the PC freezes, note the last number.\n", name, memsz);
  EFI_STATUS s = dump_to(name, TRUE);
  Print(L"\n  Saved %s : %r  (copy it off the EFI partition and send it)\n", name, s);
}


/* ---------- breadcrumbs for hard freezes ----------
 * \loader_trace.txt on the EFI partition (flushed after every line) and the motherboard POST-code port 0x80
 * (shows on boards with a 2-digit Q-Code display). After a freeze the last line / last code is the last phase reached. */
static void post_code(UINT8 v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"((UINT16)0x80)); }
static EFI_FILE_HANDLE g_trace; static BOOLEAN g_trace_tried;
static void trace_write(const CHAR16 *msg)
{
  if (!g_trace && !g_trace_tried) {
    g_trace_tried = TRUE;
    EFI_FILE_HANDLE root = g_vol_root ? g_vol_root : g_root, f;
    if (root) {
      if (!EFI_ERROR(FW(root->Open, root, &f, L"\\loader_trace.txt", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) FW(f->Delete, f);   /* start a fresh file */
      if (!EFI_ERROR(FW(root->Open, root, &f, L"\\loader_trace.txt", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0))) g_trace = f;
    }
  }
  if (!g_trace) return;
  CHAR8 line[200]; UINTN n = 0;
  for (; msg[n] && n < 196; n++) line[n] = (CHAR8)(msg[n] < 0x80 ? msg[n] : '?');
  line[n++] = '\r'; line[n++] = '\n';
  UINTN sz = n; FW(g_trace->Write, g_trace, &sz, line); FW(g_trace->Flush, g_trace);
}

/* ---------- UI helpers ---------- */
static BOOLEAN wait_key(UINTN secs)      /* secs==0: wait forever. TRUE if a key was pressed */
{
  EFI_EVENT ev[2]; UINTN idx; EFI_INPUT_KEY k; BOOLEAN pressed = FALSE; UINTN n = 1;
  ev[0] = ST->ConIn->WaitForKey;
  if (secs) { FW(BS->CreateEvent, EVT_TIMER, 0, NULL, NULL, &ev[1]); FW(BS->SetTimer, ev[1], TimerRelative, (UINT64)secs * 10000000); n = 2; }
  FW(ST->ConIn->Reset, ST->ConIn, FALSE);
  FW(BS->WaitForEvent, n, ev, &idx);
  if (idx == 0) { FW(ST->ConIn->ReadKeyStroke, ST->ConIn, &k); pressed = TRUE; }
  if (secs) { FW(BS->SetTimer, ev[1], TimerCancel, 0); FW(BS->CloseEvent, ev[1]); }
  return pressed;
}

static CHAR16 *res_tag(UINTN i)
{
  static CHAR16 b[2][64]; static UINTN k; CHAR16 *o = b[k ^= 1];
  UINT8 r = i < NRES ? g_st.res[i] : 0;
  switch (r) {
    case 1: SPrint(o, 128, L"   [hung, VRAM before=%x]", g_st.membefore[i]); break;
    case 2: SPrint(o, 128, L"   [unusable]"); break;
    case 3: SPrint(o, 128, L"   [driver error]"); break;
    case 4: SPrint(o, 128, L"   [OK]"); break;
    case 6: SPrint(o, 128, L"   [init ran, %d stuck loop(s) skipped]", (int)g_st.loops[i]); break;
    case 5: SPrint(o, 128, L"   [hung, VRAM valid - kept, before=%x]", g_st.membefore[i]); break;
    default: o[0] = 0;
  }
  return o;
}

static UINTN find_by_name(const CHAR16 *n)
{
  for (UINTN i = 0; i < ncand; i++) if (!StrCmp(cands[i].name, (CHAR16 *)n)) return i;
  return (UINTN)-1;
}

/* returns: (UINTN)-2 = AUTO, (UINTN)-1 = skip, else candidate index */
static BOOLEAN g_vfct_only;
static UINTN menu(void)
{
  UINTN sel = 0, top = 0, secs = MENU_SECS, idx; BOOLEAN timed = TRUE; EFI_EVENT tmr, ev[2]; EFI_INPUT_KEY k;
  const UINTN rows = 6;
  FW(BS->CreateEvent, EVT_TIMER, 0, NULL, NULL, &tmr);
  FW(BS->SetTimer, tmr, TimerPeriodic, 10000000);
  ev[0] = ST->ConIn->WaitForKey; ev[1] = tmr;
  FW(ST->ConIn->Reset, ST->ConIn, FALSE);
  for (;;) {
    FW(ST->ConOut->ClearScreen, ST->ConOut);
    Print(L"== RX 580 vBIOS loader ==   %d ROM(s) from %s\n", ncand, g_romdir);
    UINTN w = find_by_name(g_st.working);
    if (g_st.working[0]) Print(L"Remembered working ROM: %s%s\n", g_st.working, w == (UINTN)-1 ? L"  (file missing)" : L"");
    else Print(L"Next ROM to test in AUTO: #%d of %d%s\n", g_st.next + 1, ncand, g_st.next >= ncand ? L"  (all tried - press R)" : L"");
    Print(L"\n");
    UINTN total = ncand + 1;
    if (sel < top) top = sel;
    if (sel >= top + rows) top = sel - rows + 1;
    for (UINTN i = top; i < top + rows && i < total; i++) {
      if (i == 0) Print(L"  %s AUTO: test ROMs one by one until the GPU initialises\n", sel == 0 ? L">" : L" ");
      else Print(L"  %s %d) %s%s%s\n", sel == i ? L">" : L" ", i, cands[i-1].name,
                 (!StrCmp(cands[i-1].name, g_st.working)) ? L"  *current*" : L"", res_tag(i-1));
    }
    Print(L"\n  UP/DOWN+ENTER = run   V = VFCT-only: %s   R = forget state   ESC = skip\n", g_vfct_only ? L"ON " : L"off");
    Print(L"  D = dump all GPU registers to a file on the EFI partition (diagnosis; do it BEFORE any init)\n");
    Print(L"  F = init even if the GPU already looks initialised: %s    P = register snapshot after init: %s\n", g_force ? L"ON " : L"off", g_postdump ? L"ON " : L"off");
    Print(L"  E = init engine: %s\n", g_engine_atom ? L"BUILT-IN INTERPRETER (never hangs; skips stuck polls)" : L"FIRMWARE GOP DRIVER (hangs -> whole-PC reset)");
    if (timed) Print(L"  Starting selection in %d s... (any key stops the timer)\n", secs);
    FW(BS->WaitForEvent, 2, ev, &idx);
    if (idx == 1) { if (timed && --secs == 0) break; continue; }
    if (EFI_ERROR(FW(ST->ConIn->ReadKeyStroke, ST->ConIn, &k))) continue;
    timed = FALSE;
    if (k.UnicodeChar == L'f' || k.UnicodeChar == L'F') { g_force = !g_force; continue; }
    if (k.UnicodeChar == L'p' || k.UnicodeChar == L'P') { g_postdump = !g_postdump; continue; }
    if (k.UnicodeChar == L'd' || k.UnicodeChar == L'D') { dump_registers(); FW(BS->Stall, 8000000); continue; }
    if (k.UnicodeChar == L'e' || k.UnicodeChar == L'E') { g_engine_atom = !g_engine_atom; continue; }
    if (k.UnicodeChar == L'v' || k.UnicodeChar == L'V') { g_vfct_only = !g_vfct_only; continue; }
    if (k.UnicodeChar == L'r' || k.UnicodeChar == L'R') { ZeroMem(&g_st, sizeof g_st); g_st.magic = STATE_MAGIC; state_save(); continue; }
    if (k.UnicodeChar == L'\r') break;
    if (k.ScanCode == 0x17) { sel = (UINTN)-1; goto out; }
    if (k.ScanCode == 0x01 && sel > 0) sel--;
    if (k.ScanCode == 0x02 && sel + 1 < total) sel++;
    if (k.ScanCode == 0x09) sel = (sel > rows) ? sel - rows : 0;           /* PgUp */
    if (k.ScanCode == 0x0A) sel = (sel + rows < total) ? sel + rows : total - 1;   /* PgDn */
  }
out:
  FW(BS->SetTimer, tmr, TimerCancel, 0); FW(BS->CloseEvent, tmr);
  FW(ST->ConOut->ClearScreen, ST->ConOut);
  if (sel == (UINTN)-1) return (UINTN)-1;
  return sel == 0 ? (UINTN)-2 : sel - 1;
}

static void remember_working(UINTN idx)
{
  post_code(0x70); TRACE(L"SUCCESS %s", cands[idx].name);
  StrCpy(g_st.working, cands[idx].name); g_st.strikes = 0; state_save();
  Print(L"\n  SUCCESS - remembered working ROM: %s\n", cands[idx].name);
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab)
{
  InitializeLib(image, systab);
  state_load();
  scan_folder(image);
  UINTN choice = menu();
  BOOLEAN ok = FALSE;

  if (choice == (UINTN)-1) { Print(L"Skipped, nothing loaded.\n"); goto done; }

  if (choice != (UINTN)-2) {                         /* manual: run one ROM */
    UINTN r = run_cand(choice, g_vfct_only, image);
    if (r == RES_OK) { remember_working(choice); ok = TRUE; }
    else if (r == RES_VFCT) ok = TRUE;
    else if (r == RES_HUNG) Print(L"\n  ROM hung the GPU init; GPU was reset.\n");
    else Print(L"\n  ROM did not bring the GPU up.\n");
    goto done;
  }

  /* AUTO */
  for (;;) {
    UINTN idx; BOOLEAN was_working = FALSE;
    UINTN w = g_st.working[0] ? find_by_name(g_st.working) : (UINTN)-1;
    if (w != (UINTN)-1 && g_st.strikes < 2) { idx = w; was_working = TRUE; g_st.strikes++; state_save(); }
    else {
      if (g_st.working[0]) { g_st.working[0] = 0; g_st.strikes = 0; g_st.next = 0; state_save(); }
      idx = g_st.next;
      if (idx >= ncand) { Print(L"\n  All %d ROMs were tried without success. Menu: R = start over.\n", ncand); goto done; }
      g_st.next = (UINT32)idx + 1; state_save();      /* assume failure; cleared on success */
    }
    UINTN r = run_cand(idx, g_vfct_only, image);
    if (r == RES_OK) { remember_working(idx); ok = TRUE; goto done; }
    if (r == RES_VFCT) { ok = TRUE; goto done; }
    if (r == RES_SYSTEMIC) { if (!was_working) g_st.next = (UINT32)idx; state_save(); goto done; }
    if (r == RES_HUNG) {                                 /* GPU was reset in place -> next ROM, same boot */
      if (was_working) { g_st.working[0] = 0; g_st.strikes = 0; g_st.next = 0; state_save(); }
      continue;
    }
    if (r == RES_DEAD) { Print(L"\n  Stopping. Power-cycle the PC; AUTO resumes with the next ROM.\n"); goto done; }
    if (r == RES_SKIP) { if (was_working) { g_st.working[0] = 0; state_save(); } continue; }
    /* RES_FAIL: GPU was touched -> reset for a clean state, next boot tries the next ROM */
    if (was_working) { g_st.working[0] = 0; g_st.strikes = 0; g_st.next = 0; state_save(); }
    Print(L"\n  ROM failed -> resetting to try the next one...\n");
    FW(BS->Stall, 2500000);
    FW(RT->ResetSystem, EfiResetCold, EFI_SUCCESS, 0, NULL);
    hard_reset();
  }

done:
  if (ok) { Print(L"\n  Continuing boot in 8 s (press a key to hold this screen)...\n"); if (!wait_key(8)) return EFI_SUCCESS; }
  Print(L"\nPress any key to exit and continue boot...\n");
  wait_key(0);
  return EFI_SUCCESS;
}
