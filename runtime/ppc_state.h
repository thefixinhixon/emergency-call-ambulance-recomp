// Interface between recompiled PowerPC code and the hybrid CPU core.
//
// The recompiled code runs directly on Supermodel's interpreter state so the two can
// hand off at any instruction boundary. PPCRState mirrors PPC_REGS from
// third_party/supermodel/Src/CPU/PowerPC/ppc.cpp field for field; ppc_core.cpp
// static_asserts the layout, so a Supermodel update that changes the struct fails
// to compile instead of corrupting state.
#pragma once
#include <cstdint>
#include <cstring>

struct PPCRFetchRegion
{
  uint32_t start;
  uint32_t end;
  uint32_t *ptr;
};

struct PPCRState
{
  bool fatalError;
  uint32_t r[32];
  uint32_t pc;
  uint32_t npc;
  uint32_t *op;
  uint32_t lr;
  uint32_t ctr;
  uint32_t xer;
  uint32_t msr;
  uint8_t cr[8];
  uint32_t pvr;
  uint32_t srr0, srr1, srr2, srr3;
  uint32_t hid0, hid1, hid2;
  uint32_t sdr1;
  uint32_t sprg[4];
  uint32_t dsisr, dar, ear, dmiss, dcmp, hash1, hash2, imiss, icmp, rpa;
  struct { uint32_t u, l; } ibat[4], dbat[4];
  uint32_t evpr, exier, exisr, bear, besr, iocr;
  uint32_t br[8];
  uint32_t iabr, esr, iccr, dccr, pit, pit_counter, pit_int_enable, tsr, dbsr, sgr, pid;
  int reserved;
  uint32_t reserved_address;
  int interrupt_pending;
  int external_int;
  uint64_t tb;
  int (*irq_callback)(int irqline);
  PPCRFetchRegion cur_fetch;
  PPCRFetchRegion *fetch;
  uint32_t dec;
  uint32_t fpscr;
  union { uint64_t id; double fd; } fpr[32];
  uint32_t sr[16];
  int timer_ratio;
  uint32_t timer_frac;
  int tb_base_icount;
  int dec_base_icount;
  int dec_trigger_cycle;
  uint64_t total_cycles;
  int icount;
  int cur_cycles;
  int bus_freq_multiplier;
  int cycles_per_second;
};

constexpr uint32_t PPCR_XER_SO = 0x80000000u;
constexpr uint32_t PPCR_XER_OV = 0x40000000u;
constexpr uint32_t PPCR_XER_CA = 0x20000000u;

// Exit codes returned by recompiled units to the dispatcher
enum PPCRExit : int
{
  PPCR_EXIT_DISPATCH = 0,   // npc holds the next guest PC; look it up again
  PPCR_EXIT_INTERPRET = 1,  // block cannot run natively right now (timer/budget edge); interpret from npc
};

typedef int (*PPCRUnitFn)(uint32_t entry);

struct PPCRUnitEntry
{
  uint32_t addr;
  PPCRUnitFn fn;
};

// Recompiled RAM units record the guest code they were built from so the core can check
// it still matches RAM before running them. A unit covers one or more address ranges.
struct PPCRRamRange
{
  uint32_t start;           // guest address
  uint32_t words;           // instruction words
  uint32_t code_offset;     // index of the first expected word in ppcr_ram_code[]
};

struct PPCRRamUnit
{
  uint32_t first_range;     // index into ppcr_ram_ranges[]
  uint32_t num_ranges;
};

extern "C++" {

extern PPCRState *ppcr;
extern uint8_t *ppcr_ram;              // Supermodel RAM: 8 MB, host-order 32-bit words
extern uint8_t ppcr_code_line[];       // 256-byte lines of RAM that hold recompiled code

// Slow paths (MMIO, unaligned, CROM data) go through Supermodel's bus. They first set
// pc/npc/icount exactly as the interpreter would have them at this instruction, since a
// bus access can raise an external interrupt. They return true if that redirected npc.
bool ppcr_ld8_slow(uint32_t ea, uint8_t &out, uint32_t pc, int icount);
bool ppcr_ld16_slow(uint32_t ea, uint16_t &out, uint32_t pc, int icount);
bool ppcr_ld32_slow(uint32_t ea, uint32_t &out, uint32_t pc, int icount);
bool ppcr_ld64_slow(uint32_t ea, uint64_t &out, uint32_t pc, int icount);
bool ppcr_st8_slow(uint32_t ea, uint8_t v, uint32_t pc, int icount);
bool ppcr_st16_slow(uint32_t ea, uint16_t v, uint32_t pc, int icount);
bool ppcr_st32_slow(uint32_t ea, uint32_t v, uint32_t pc, int icount);
bool ppcr_st64_slow(uint32_t ea, uint64_t v, uint32_t pc, int icount);
void ppcr_code_write(uint32_t addr);   // a store hit a RAM line holding recompiled code
bool ppcr_ram_unit_ok(uint32_t idx);   // RAM unit idx still matches the code it was compiled from

// Runs one instruction through the interpreter's handler (pc/npc must be set)
void ppcr_interp_op(uint32_t op);

// Generated tables (generated/ppc_table.cpp)
extern const PPCRUnitEntry ppcr_units[];
extern const uint32_t ppcr_num_units;
extern const PPCRRamUnit ppcr_ram_units[];
extern const uint32_t ppcr_num_ram_units;
extern const PPCRRamRange ppcr_ram_ranges[];
extern const uint32_t ppcr_num_ram_ranges;
extern const uint32_t ppcr_ram_code[];   // expected instruction words, host order

}  // extern "C++"

// --- Inline memory access fast paths (aligned RAM; everything else goes to the bus) --
// Each returns true if an interrupt raised by a bus access redirected execution.

constexpr uint32_t PPCR_RAM_SIZE = 0x00800000;

static inline void ppcr_note_store(uint32_t ea)
{
  if (ppcr_code_line[ea >> 8]) [[unlikely]]
    ppcr_code_write(ea);
}

static inline bool ppcr_ld32(uint32_t ea, uint32_t &out, uint32_t pc, int ic)
{
  if (ea < PPCR_RAM_SIZE && !(ea & 3)) [[likely]]
  {
    memcpy(&out, &ppcr_ram[ea], 4);
    return false;
  }
  return ppcr_ld32_slow(ea, out, pc, ic);
}

static inline bool ppcr_ld16(uint32_t ea, uint16_t &out, uint32_t pc, int ic)
{
  if (ea < PPCR_RAM_SIZE && !(ea & 1)) [[likely]]
  {
    memcpy(&out, &ppcr_ram[ea ^ 2], 2);
    return false;
  }
  return ppcr_ld16_slow(ea, out, pc, ic);
}

static inline bool ppcr_ld8(uint32_t ea, uint8_t &out, uint32_t pc, int ic)
{
  if (ea < PPCR_RAM_SIZE) [[likely]]
  {
    out = ppcr_ram[ea ^ 3];
    return false;
  }
  return ppcr_ld8_slow(ea, out, pc, ic);
}

static inline bool ppcr_ld64(uint32_t ea, uint64_t &out, uint32_t pc, int ic)
{
  if (ea <= PPCR_RAM_SIZE - 8 && !(ea & 3)) [[likely]]
  {
    uint32_t hi, lo;
    memcpy(&hi, &ppcr_ram[ea], 4);
    memcpy(&lo, &ppcr_ram[ea + 4], 4);
    out = ((uint64_t)hi << 32) | lo;
    return false;
  }
  return ppcr_ld64_slow(ea, out, pc, ic);
}

static inline bool ppcr_st32(uint32_t ea, uint32_t v, uint32_t pc, int ic)
{
  if (ea < PPCR_RAM_SIZE && !(ea & 3)) [[likely]]
  {
    memcpy(&ppcr_ram[ea], &v, 4);
    ppcr_note_store(ea);
    return false;
  }
  return ppcr_st32_slow(ea, v, pc, ic);
}

static inline bool ppcr_st16(uint32_t ea, uint16_t v, uint32_t pc, int ic)
{
  if (ea < PPCR_RAM_SIZE && !(ea & 1)) [[likely]]
  {
    memcpy(&ppcr_ram[ea ^ 2], &v, 2);
    ppcr_note_store(ea);
    return false;
  }
  return ppcr_st16_slow(ea, v, pc, ic);
}

static inline bool ppcr_st8(uint32_t ea, uint8_t v, uint32_t pc, int ic)
{
  if (ea < PPCR_RAM_SIZE) [[likely]]
  {
    ppcr_ram[ea ^ 3] = v;
    ppcr_note_store(ea);
    return false;
  }
  return ppcr_st8_slow(ea, v, pc, ic);
}

static inline bool ppcr_st64(uint32_t ea, uint64_t v, uint32_t pc, int ic)
{
  if (ea <= PPCR_RAM_SIZE - 8 && !(ea & 3)) [[likely]]
  {
    uint32_t hi = (uint32_t)(v >> 32), lo = (uint32_t)v;
    memcpy(&ppcr_ram[ea], &hi, 4);
    memcpy(&ppcr_ram[ea + 4], &lo, 4);
    ppcr_note_store(ea);
    ppcr_note_store(ea + 4);
    return false;
  }
  return ppcr_st64_slow(ea, v, pc, ic);
}

static inline float ppcr_u2f(uint32_t i) { float f; memcpy(&f, &i, 4); return f; }
static inline uint32_t ppcr_f2u(float f) { uint32_t i; memcpy(&i, &f, 4); return i; }

// --- Condition register helpers matching the interpreter exactly ---------------------

static inline void ppcr_set_cr0(uint32_t v)
{
  int32_t s = (int32_t)v;
  ppcr->cr[0] = (s < 0) ? 0x8 : (s > 0) ? 0x4 : 0x2;
  if (ppcr->xer & PPCR_XER_SO)
    ppcr->cr[0] |= 0x1;
}

static inline int ppcr_crbit(unsigned bi)
{
  return (ppcr->cr[bi / 4] & (1 << (3 - (bi % 4)))) ? 1 : 0;
}

// check_condition_code() from ppc.cpp, with BO/BI known at recompile time
static inline bool ppcr_cond(unsigned bo, unsigned bi)
{
  unsigned bo0 = (bo & 0x10) ? 1 : 0;
  unsigned bo1 = (bo & 0x08) ? 1 : 0;
  unsigned bo2 = (bo & 0x04) ? 1 : 0;
  unsigned bo3 = (bo & 0x02) ? 1 : 0;
  if (bo2 == 0)
    --ppcr->ctr;
  unsigned ctr_ok = bo2 | ((ppcr->ctr != 0) ^ bo3);
  unsigned cond_ok = bo0 | (ppcr_crbit(bi) ^ (bo1 ^ 1));
  return (ctr_ok & cond_ok) != 0;
}

static inline void ppcr_cmps(unsigned d, int32_t a, int32_t b)
{
  ppcr->cr[d] = (a < b) ? 0x8 : (a > b) ? 0x4 : 0x2;
  if (ppcr->xer & PPCR_XER_SO)
    ppcr->cr[d] |= 0x1;
}

static inline void ppcr_cmpu(unsigned d, uint32_t a, uint32_t b)
{
  ppcr->cr[d] = (a < b) ? 0x8 : (a > b) ? 0x4 : 0x2;
  if (ppcr->xer & PPCR_XER_SO)
    ppcr->cr[d] |= 0x1;
}

static inline void ppcr_set_ca(bool c)
{
  if (c)
    ppcr->xer |= PPCR_XER_CA;
  else
    ppcr->xer &= ~PPCR_XER_CA;
}

// SET_ADD_OV / SET_SUB_OV from ppc.cpp
static inline void ppcr_add_ov(uint32_t r, uint32_t a, uint32_t b)
{
  if ((~(a ^ b) & (a ^ r)) & 0x80000000u)
    ppcr->xer |= PPCR_XER_SO | PPCR_XER_OV;
  else
    ppcr->xer &= ~PPCR_XER_OV;
}

static inline void ppcr_sub_ov(uint32_t r, uint32_t a, uint32_t b)
{
  if (((a ^ b) & (a ^ r)) & 0x80000000u)
    ppcr->xer |= PPCR_XER_SO | PPCR_XER_OV;
  else
    ppcr->xer &= ~PPCR_XER_OV;
}

static inline void ppcr_set_crbit(unsigned bit, int b)
{
  if (b)
    ppcr->cr[bit / 4] |= (uint8_t)(1 << (3 - (bit % 4)));
  else
    ppcr->cr[bit / 4] &= (uint8_t)~(1 << (3 - (bit % 4)));
}

static inline uint32_t ppcr_get_cr()
{
  const uint8_t *c = ppcr->cr;
  return (uint32_t)c[0] << 28 | (uint32_t)c[1] << 24 | (uint32_t)c[2] << 20 | (uint32_t)c[3] << 16 |
         (uint32_t)c[4] << 12 | (uint32_t)c[5] << 8 | (uint32_t)c[6] << 4 | (uint32_t)c[7];
}

static inline uint32_t ppcr_cntlzw(uint32_t v)
{
  uint32_t n = 0;
  for (uint32_t m = 0x80000000u; n < 32 && !(v & m); m >>= 1)
    n++;
  return n;
}
