/* Compatibility shim: lets the Linux amdgpu AtomBIOS interpreter (atom.c, MIT licence,
 * (c) 2008 Advanced Micro Devices) build as a freestanding EFI object. */
#ifndef ATOM_EFI_SHIM_H
#define ATOM_EFI_SHIM_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef uint8_t __u8; typedef uint16_t __u16; typedef uint32_t __u32; typedef uint64_t __u64;
typedef uint8_t __le8; typedef uint16_t __le16; typedef uint32_t __le32; typedef uint64_t __le64;
struct drm_device; struct mutex { int x; };
#define EINVAL 22
#define KERN_INFO ""
#define KERN_ERR ""
#define KERN_DEBUG ""
#define KERN_CONT ""
#define pr_info(...) ((void)0)
#define pr_cont(...) ((void)0)
#define printk(...) ((void)0)
#define DRM_ERROR(...) ((void)0)
#define cpu_to_le32(x) (x)
#define cpu_to_le16(x) (x)
#define le16_to_cpu(x) (x)
#define le32_to_cpu(x) (x)
#define get_unaligned_le32(p) ({ uint32_t _v; __builtin_memcpy(&_v, (p), 4); _v; })
#define get_unaligned_le16(p) ({ uint16_t _v; __builtin_memcpy(&_v, (p), 2); _v; })
#define GFP_KERNEL 0
void *atom_alloc(size_t n);          /* zeroed */
void atom_free(void *p);
void atom_stall_us(unsigned us);
unsigned long atom_now_ms(void);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t n);
size_t strlcpy(char *d, const char *s, size_t n);
void *memset(void *s, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
#define kzalloc(sz, f) atom_alloc(sz)
#define kmalloc(sz, f) atom_alloc(sz)
#define kcalloc(n, sz, f) atom_alloc((n) * (sz))
#define kfree(p) atom_free(p)
#define mutex_init(m) ((void)0)
#define mutex_lock(m) ((void)0)
#define mutex_unlock(m) ((void)0)
#define mutex_destroy(m) ((void)0)
#define jiffies atom_now_ms()
#define HZ 1000
#define time_after(a, b) ((long)((b) - (a)) < 0)
#define jiffies_to_msecs(x) (x)
#define msleep(x) atom_stall_us((x) * 1000u)
#define mdelay(x) atom_stall_us((x) * 1000u)
#define udelay(x) atom_stall_us(x)
#define drm_can_sleep() 0
#define lower_32_bits(n) ((uint32_t)((n) & 0xffffffff))
#define upper_32_bits(n) ((uint32_t)(((n) >> 16) >> 16))
#define do_div(n, base) ({ uint32_t __rem = (uint32_t)((n) % (base)); (n) /= (base); __rem; })
/* loop breaker (see atom_op_jump) */
extern int atom_break_loops; extern unsigned atom_loop_ms; extern int atom_loops_broken;
void atom_hook_loop_broken(unsigned table_start, unsigned target, unsigned ms);
#endif
