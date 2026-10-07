/* Runtime support for the embedded AtomBIOS interpreter (atomlib/atom.c): memory, timing, libc bits. */
#include <efi.h>
#include <efilib.h>
#include <stddef.h>

extern void loader_loop_diag(void);
void *atom_alloc(size_t n) { return AllocateZeroPool(n ? n : 1); }
void atom_free(void *p) { if (p) FreePool(p); }
void atom_stall_us(unsigned us) { uefi_call_wrapper(BS->Stall, 1, (UINTN)us); }

static inline UINT64 rdtsc_(void) { UINT32 lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((UINT64)hi << 32) | lo; }
unsigned long atom_now_ms(void)                      /* monotonic milliseconds from the TSC, calibrated against Stall() */
{
  static UINT64 per_ms;
  if (!per_ms) {
    UINT64 a = rdtsc_(); uefi_call_wrapper(BS->Stall, 1, 20000); UINT64 b = rdtsc_();
    per_ms = (b - a) / 20; if (per_ms < 100000) per_ms = 2000000;     /* sanity: >= 100 MHz */
  }
  return (unsigned long)(rdtsc_() / per_ms);
}

void atom_hook_loop_broken(unsigned table_start, unsigned target, unsigned ms)
{
  Print(L"   [stuck poll broken after %d ms: table code @0x%04x, loop target 0x%04x]\n", ms, table_start, target);
  loader_loop_diag();
}

size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
size_t strnlen(const char *s, size_t m) { size_t n = 0; while (n < m && s[n]) n++; return n; }
size_t strlcpy(char *d, const char *s, size_t n) { size_t l = strlen(s); if (n) { size_t c = l >= n ? n - 1 : l; for (size_t i = 0; i < c; i++) d[i] = s[i]; d[c] = 0; } return l; }
int strncmp(const char *a, const char *b, size_t n)
{
  for (size_t i = 0; i < n; i++) { unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i]; if (x != y) return x < y ? -1 : 1; if (!x) return 0; }
  return 0;
}
