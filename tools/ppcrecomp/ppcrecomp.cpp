// ppcrecomp: static recompiler from Model 3 PowerPC 603e code to C++.
//
//   ppcrecomp --config <game.cfg> --crom <crom.bin> [--seeds <file>]... --shards <n> --out <dir>
//
// Input
//   crom.bin   8 MB big-endian fixed CROM image, mapped at 0xFF800000 (from romtool)
//   game.cfg   code regions and entry points (see config/eca.cfg)
//   seeds      extra entry points, one hex address per line (e.g. the hybrid core's
//              ppc_misses.txt, or functions exported from Ghidra by tools/ghidra)
//
// Output
//   ppc_recomp_NN.cpp   recompiled units, spread over <n> files
//   ppc_table.cpp       entry point table and RAM code checksums for runtime/ppc_core.cpp
//
// Model
//   A unit is the code reachable from one entry point through non-call branches. Each
//   basic block in it becomes a labelled C++ block. Calls, returns, indirect jumps and
//   exceptions leave the unit and go back to the runtime dispatcher. Every block first
//   checks that it can run to completion without crossing the cycle budget or a
//   decrementer tick; if not, it hands control to the interpreter, which keeps the
//   recompiled code instruction-exact with Supermodel's interpreter.
//
//   Common integer, branch and load/store instructions are emitted inline, with
//   semantics copied from Supermodel's ppc_ops.c. Everything else calls the
//   interpreter's handler for that one instruction.
#include <algorithm>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "CPU/PowerPC/PPCDisasm.h"

namespace
{
  constexpr uint32_t CROM_BASE = 0xFF800000;
  constexpr uint32_t CROM_SIZE = 0x00800000;
  constexpr uint32_t RAM_SIZE = 0x00800000;
  constexpr size_t MAX_UNIT_INSTRS = 16384;

  [[noreturn]] void Fatal(const char *fmt, ...)
  {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "ppcrecomp: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
  }

  std::string Fmt(const char *fmt, ...)
  {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
  }

  // ---------------------------------------------------------------------------------
  // Guest code image
  // ---------------------------------------------------------------------------------

  struct Image
  {
    std::vector<uint8_t> crom;               // big-endian bytes
    std::vector<uint8_t> ram;                // big-endian bytes, only code copied at boot
    std::vector<bool> ramValid;              // per word: RAM image holds known code here

    bool Fetch(uint32_t addr, uint32_t &w) const
    {
      if (addr & 3)
        return false;
      const uint8_t *p;
      if (addr >= CROM_BASE)
        p = &crom[addr - CROM_BASE];
      else if (addr < RAM_SIZE && ramValid[addr / 4])
        p = &ram[addr];
      else
        return false;
      w = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
      return true;
    }
  };

  struct Config
  {
    std::vector<std::pair<uint32_t, std::string>> entries;
    struct RamCopy { uint32_t dst, src, size; };
    std::vector<RamCopy> ramCopies;
    std::set<uint32_t> excludes;             // never treat as an entry point
  };

  uint32_t ParseNum(const std::string &s)
  {
    return (uint32_t)strtoul(s.c_str(), nullptr, 0);
  }

  Config LoadConfig(const std::string &path)
  {
    std::ifstream in(path);
    if (!in)
      Fatal("cannot open config %s", path.c_str());
    Config cfg;
    std::string line;
    int lineno = 0;
    while (std::getline(in, line))
    {
      lineno++;
      if (auto hash = line.find('#'); hash != std::string::npos)
        line.resize(hash);
      std::istringstream ss(line);
      std::string kw;
      if (!(ss >> kw))
        continue;
      if (kw == "entry")
      {
        std::string addr, name;
        ss >> addr >> name;
        cfg.entries.push_back({ParseNum(addr), name});
      }
      else if (kw == "ram_copy")
      {
        std::string dst, src, size;
        ss >> dst >> src >> size;
        cfg.ramCopies.push_back({ParseNum(dst), ParseNum(src), ParseNum(size)});
      }
      else if (kw == "exclude")
      {
        std::string addr;
        ss >> addr;
        cfg.excludes.insert(ParseNum(addr));
      }
      else
        Fatal("%s:%d: unknown keyword '%s'", path.c_str(), lineno, kw.c_str());
    }
    return cfg;
  }

  std::vector<uint32_t> LoadSeeds(const std::string &path)
  {
    std::vector<uint32_t> seeds;
    std::ifstream in(path);
    if (!in)
      return seeds;   // a missing seed file just means no extra seeds yet
    std::string line;
    while (std::getline(in, line))
    {
      if (auto hash = line.find('#'); hash != std::string::npos)
        line.resize(hash);
      std::istringstream ss(line);
      std::string addr;
      if (ss >> addr)
        seeds.push_back((uint32_t)strtoul(addr.c_str(), nullptr, 16));
    }
    return seeds;
  }

  // ---------------------------------------------------------------------------------
  // Instruction classification
  // ---------------------------------------------------------------------------------

  inline uint32_t OPCD(uint32_t w) { return w >> 26; }
  inline uint32_t RT(uint32_t w) { return (w >> 21) & 31; }
  inline uint32_t RA(uint32_t w) { return (w >> 16) & 31; }
  inline uint32_t RB(uint32_t w) { return (w >> 11) & 31; }
  inline uint32_t XO(uint32_t w) { return (w >> 1) & 0x3FF; }
  inline int32_t SIMM(uint32_t w) { return (int32_t)(int16_t)(w & 0xFFFF); }
  inline uint32_t UIMM(uint32_t w) { return w & 0xFFFF; }
  inline bool RC(uint32_t w) { return w & 1; }
  inline bool OE(uint32_t w) { return w & 0x400; }
  inline uint32_t SPRN(uint32_t w) { return ((w >> 16) & 0x1F) | ((w >> 6) & 0x3E0); }

  enum class Flow
  {
    Normal,      // execution continues at pc+4
    Branch,      // b/bc/bclr/bcctr (any form)
    Terminator,  // handler may redirect npc or change timing (mtmsr, mtspr, rfi, sc, trap, unknown)
  };

  bool IsInlineSpr(uint32_t spr) { return spr == 1 || spr == 8 || spr == 9; }

  // Opcodes whose handler cannot change control flow or timing and that do not read
  // pc/npc/icount. Everything else either is inlined or gets full handler treatment.
  bool IsPureHandler(uint32_t w)
  {
    uint32_t op = OPCD(w);
    if (op == 59 || op == 63)
      return true;   // floating point arithmetic: no exceptions in Supermodel's FPU
    return false;
  }

  // 31-extended opcodes that access memory through the bus but are not inlined
  bool IsMemoryHandler31(uint32_t xo)
  {
    switch (xo)
    {
    case 375:  // lhaux
    case 790:  // lhbrx
    case 534:  // lwbrx
    case 918:  // sthbrx
    case 662:  // stwbrx
    case 597: case 533:  // lswi, lswx
    case 725: case 661:  // stswi, stswx
    case 20:   // lwarx
    case 150:  // stwcx.
    case 247:  // stbux
    case 439:  // sthux
    case 1014: // dcbz
    case 758:  // dcba
    case 567: case 631: case 695: case 759:  // lfsux, lfdux, stfsux, stfdux
    case 983:  // stfiwx
    case 310: case 438:  // eciwx, ecowx
      return true;
    }
    return false;
  }

  Flow Classify(uint32_t w)
  {
    uint32_t op = OPCD(w);
    switch (op)
    {
    case 16: case 18:
      return Flow::Branch;
    case 19:
      switch (XO(w))
      {
      case 16: case 528:
        return Flow::Branch;
      case 0: case 257: case 129: case 289: case 225: case 33: case 449: case 417: case 193: case 150:
        return Flow::Normal;  // mcrf, cr ops, isync
      }
      return Flow::Terminator; // rfi and anything unknown
    case 17: case 3:
      return Flow::Terminator; // sc, twi
    case 7: case 8: case 10: case 11: case 12: case 13: case 14: case 15:
    case 20: case 21: case 23: case 24: case 25: case 26: case 27: case 28: case 29:
    case 32: case 33: case 34: case 35: case 36: case 37: case 38: case 39:
    case 40: case 41: case 42: case 43: case 44: case 45: case 46: case 47:
    case 48: case 49: case 50: case 51: case 52: case 53: case 54: case 55:
    case 59: case 63:
      return Flow::Normal;
    case 31:
    {
      uint32_t xo = XO(w);
      switch (xo & 0x1FF)  // OE variants share the low 9 bits for the arithmetic group
      {
      case 266: case 10: case 138: case 202: case 234: case 40: case 8: case 136: case 200: case 232:
      case 104: case 235: case 491: case 459:
        return Flow::Normal;
      }
      switch (xo)
      {
      case 75: case 11:                                  // mulhw, mulhwu
      case 28: case 60: case 444: case 412: case 124: case 476: case 316: case 284:
      case 954: case 922: case 26: case 24: case 536: case 792: case 824: case 0: case 32:
      case 23: case 55: case 87: case 119: case 279: case 311: case 343:
      case 151: case 183: case 215: case 407:
      case 535: case 599: case 663: case 727:            // lfsx, lfdx, stfsx, stfdx
      case 19: case 144: case 83:                        // mfcr, mtcrf, mfmsr
      case 598: case 854: case 86: case 470: case 54: case 278: case 246: case 982:
      case 371: case 595: case 659: case 512:            // mftb, mfsr, mfsrin, mcrxr
        return Flow::Normal;
      case 339:
        return Flow::Normal;                             // mfspr
      case 467:
        return IsInlineSpr(SPRN(w)) ? Flow::Normal : Flow::Terminator;  // mtspr
      }
      if (IsMemoryHandler31(xo))
        return Flow::Normal;
      return Flow::Terminator;                           // mtmsr, mtsr(in), tw, tlb*, unknown
    }
    }
    return Flow::Terminator;
  }

  uint32_t BranchTarget(uint32_t addr, uint32_t w)
  {
    if (OPCD(w) == 18)
    {
      int32_t li = (int32_t)(w & 0x03FFFFFC);
      if (li & 0x02000000)
        li |= (int32_t)0xFC000000;
      return (w & 2) ? (uint32_t)li : addr + (uint32_t)li;
    }
    uint32_t bd = (uint32_t)(int32_t)(int16_t)(w & 0xFFFC);
    return (w & 2) ? bd : addr + bd;
  }

  bool IsUnconditional(uint32_t w)
  {
    uint32_t bo = RT(w);
    return (bo & 0x14) == 0x14;
  }

  // ---------------------------------------------------------------------------------
  // Unit discovery
  // ---------------------------------------------------------------------------------

  struct Unit
  {
    uint32_t entry;
    std::set<uint32_t> leaders;               // block start addresses
    std::set<uint32_t> instrs;                // every instruction address in the unit
    bool inRam() const { return entry < RAM_SIZE; }
  };

  class Analyzer
  {
  public:
    Analyzer(const Image &img, const std::set<uint32_t> &excludes) : m_img(img), m_excludes(excludes) {}

    // Explore the unit starting at entry. Direct call targets are added to newCalls.
    Unit Explore(uint32_t entry, const std::set<uint32_t> &functionEntries, std::set<uint32_t> &newCalls) const
    {
      Unit u;
      u.entry = entry;
      std::vector<uint32_t> work{entry};
      u.leaders.insert(entry);
      auto addLeader = [&](uint32_t a) {
        uint32_t w;
        if (!m_img.Fetch(a, w))
          return;
        if (a != entry && functionEntries.count(a))
          return;                                  // tail call into another unit: leave via the dispatcher
        if (u.instrs.size() >= MAX_UNIT_INSTRS && !u.leaders.count(a))
          return;
        if (u.leaders.insert(a).second)
          work.push_back(a);
      };
      auto addCall = [&](uint32_t a) {
        uint32_t w;
        if (m_img.Fetch(a, w) && !m_excludes.count(a))
          newCalls.insert(a);
      };

      while (!work.empty())
      {
        uint32_t a = work.back();
        work.pop_back();
        while (true)
        {
          if (u.instrs.count(a))
          {
            u.leaders.insert(a);                    // flow merges into explored code
            break;
          }
          uint32_t w;
          if (!m_img.Fetch(a, w))
            break;
          u.instrs.insert(a);
          Flow f = Classify(w);
          if (f == Flow::Normal)
          {
            a += 4;
            continue;
          }
          if (f == Flow::Branch)
          {
            uint32_t op = OPCD(w);
            bool lk = w & 1;
            if (op == 18 || op == 16)
            {
              uint32_t t = BranchTarget(a, w);
              if (lk)
                addCall(t);
              else
                addLeader(t);
              if (op == 16 && !IsUnconditional(w))
                addLeader(a + 4);
              else if (lk)
                addLeader(a + 4);                   // return point
            }
            else
            {
              // bclr / bcctr: indirect. Conditional forms and linking forms continue at pc+4.
              if (!IsUnconditional(w) || lk)
                addLeader(a + 4);
            }
            break;
          }
          // Terminator: the handler may leave, but when it does not, execution resumes at
          // pc+4 (also the return point of sc). rfi never falls through.
          if (!(OPCD(w) == 19 && XO(w) == 50))
            addLeader(a + 4);
          break;
        }
      }
      return u;
    }

  private:
    const Image &m_img;
    const std::set<uint32_t> &m_excludes;
  };

  // ---------------------------------------------------------------------------------
  // Code generation
  // ---------------------------------------------------------------------------------

  uint32_t RotateMask(uint32_t mb, uint32_t me)
  {
    uint32_t mask = (0xFFFFFFFFu >> mb) ^ ((me >= 31) ? 0 : (0xFFFFFFFFu >> (me + 1)));
    if (mb > me)
      mask = ~mask;
    return mask;
  }

  std::string UnitName(uint32_t entry) { return Fmt("ppcr_unit_%08X", entry); }

  class Emitter
  {
  public:
    Emitter(const Image &img, const Unit &u, int ramIndex) : m_img(img), m_u(u), m_ramIndex(ramIndex) {}

    std::string Generate()
    {
      Line("int %s(uint32_t entry)", UnitName(m_u.entry).c_str());
      Line("{");
      Line("  PPCRState *const S = ppcr;");
      if (m_ramIndex >= 0)
      {
        Line("  if (!ppcr_ram_unit_ok(%d)) [[unlikely]]", m_ramIndex);
        Line("  {");
        Line("    S->npc = entry;");
        Line("    return PPCR_EXIT_INTERPRET;");
        Line("  }");
      }
      Line("  switch (entry)");
      Line("  {");
      for (uint32_t l : m_u.leaders)
        Line("  case 0x%08Xu: goto L_%08X;", l, l);
      Line("  default: S->npc = entry; return PPCR_EXIT_INTERPRET;");
      Line("  }");

      std::vector<uint32_t> leaders(m_u.leaders.begin(), m_u.leaders.end());
      for (size_t i = 0; i < leaders.size(); i++)
        EmitBlock(leaders[i]);
      Line("}");
      return m_out.str();
    }

  private:
    const Image &m_img;
    const Unit &m_u;
    int m_ramIndex;
    std::ostringstream m_out;

    // Per-block state while emitting
    uint32_t m_n = 0;        // instructions in the current block
    uint32_t m_k = 0;        // index of the current instruction in the block

    void Line(const char *fmt, ...)
    {
      char buf[1024];
      va_list ap;
      va_start(ap, fmt);
      vsnprintf(buf, sizeof(buf), fmt, ap);
      va_end(ap);
      m_out << buf << '\n';
    }

    std::string R(uint32_t r) { return Fmt("S->r[%u]", r); }

    // Instructions in the block that starts at a: up to and including a branch or
    // terminator, or up to the next leader
    std::vector<std::pair<uint32_t, uint32_t>> BlockInstrs(uint32_t a, uint32_t &fallthrough, bool &hasFallthrough)
    {
      std::vector<std::pair<uint32_t, uint32_t>> v;
      hasFallthrough = false;
      while (true)
      {
        uint32_t w;
        if (!m_img.Fetch(a, w))
        {
          fallthrough = a;
          hasFallthrough = true;   // leaves the image: let the dispatcher/interpreter handle it
          break;
        }
        v.push_back({a, w});
        Flow f = Classify(w);
        if (f != Flow::Normal)
          break;
        a += 4;
        if (m_u.leaders.count(a) || !m_u.instrs.count(a))
        {
          fallthrough = a;
          hasFallthrough = true;
          break;
        }
      }
      return v;
    }

    // Continue at target t: jump inside the unit when possible, otherwise exit.
    void Jump(uint32_t t, const char *indent = "  ")
    {
      if (m_u.leaders.count(t))
        Line("%sgoto L_%08X;", indent, t);
      else
        Line("%s{ S->npc = 0x%08Xu; return PPCR_EXIT_DISPATCH; }", indent, t);
    }

    void Exit(uint32_t t, const char *indent = "  ")
    {
      Line("%s{ S->npc = 0x%08Xu; return PPCR_EXIT_DISPATCH; }", indent, t);
    }

    void EndCycles(const char *indent = "  ") { Line("%sS->icount = c - %u;", indent, m_n); }

    void EmitBlock(uint32_t start)
    {
      uint32_t fallthrough = 0;
      bool hasFallthrough = false;
      auto instrs = BlockInstrs(start, fallthrough, hasFallthrough);
      m_n = (uint32_t)instrs.size();

      Line("L_%08X:", start);
      Line("{");
      if (m_n == 0)
      {
        Exit(start);
        Line("}");
        return;
      }
      Line("  const int c = S->icount;");
      Line("  if (c < %u || (uint32_t)c - 1u - (uint32_t)S->dec_trigger_cycle < %uu) [[unlikely]]", m_n, m_n);
      Line("  {");
      Line("    S->npc = 0x%08Xu;", start);
      Line("    return PPCR_EXIT_INTERPRET;");
      Line("  }");

      bool ended = false;
      for (m_k = 0; m_k < m_n; m_k++)
      {
        auto [a, w] = instrs[m_k];
        char mnem[32] = {}, oprs[128] = {};
        if (DisassemblePowerPC(w, a, mnem, oprs, sizeof(oprs), true) != Result::OKAY)
          strcpy(mnem, "???");
        Line("  // %08X: %08X  %s %s", a, w, mnem, oprs);
        ended = EmitInstr(a, w);
      }
      if (!ended && hasFallthrough)
      {
        EndCycles();
        Jump(fallthrough);
      }
      Line("}");
    }

    // Calls the interpreter's handler for one instruction
    void Handler(uint32_t a, uint32_t w, bool setup)
    {
      if (setup)
        Line("  S->pc = 0x%08Xu; S->npc = 0x%08Xu; S->icount = c - %u;", a, a + 4, m_k);
      Line("  ppcr_interp_op(0x%08Xu);", w);
    }

    // Handler that accesses memory: an interrupt may redirect execution
    void MemoryHandler(uint32_t a, uint32_t w)
    {
      Handler(a, w, true);
      Line("  if (S->npc != 0x%08Xu) [[unlikely]] { S->icount = c - %u; return PPCR_EXIT_DISPATCH; }", a + 4, m_k + 1);
    }

    void RedirectCheck(uint32_t a)
    {
      Line("    if (x) [[unlikely]] { S->icount = c - %u; return PPCR_EXIT_DISPATCH; }", m_k + 1);
    }

    std::string EA_D(uint32_t w)   // (rA|0) + d
    {
      if (RA(w) == 0)
        return Fmt("0x%08Xu", (uint32_t)SIMM(w));
      return Fmt("%s + 0x%08Xu", R(RA(w)).c_str(), (uint32_t)SIMM(w));
    }
    std::string EA_DU(uint32_t w)  // rA + d (update forms)
    {
      return Fmt("%s + 0x%08Xu", R(RA(w)).c_str(), (uint32_t)SIMM(w));
    }
    std::string EA_X(uint32_t w)   // (rA|0) + rB
    {
      if (RA(w) == 0)
        return R(RB(w));
      return Fmt("%s + %s", R(RA(w)).c_str(), R(RB(w)).c_str());
    }
    std::string EA_XU(uint32_t w)  // rA + rB
    {
      return Fmt("%s + %s", R(RA(w)).c_str(), R(RB(w)).c_str());
    }

    // kind: "32"/"16"/"8"; ext: how to widen into the register
    void Load(uint32_t a, uint32_t w, const std::string &ea, const char *bits, const char *conv, bool update)
    {
      Line("  {");
      Line("    const uint32_t ea = %s;", ea.c_str());
      Line("    uint%s_t v;", bits);
      Line("    const bool x = ppcr_ld%s(ea, v, 0x%08Xu, c - %u);", bits, a, m_k);
      Line("    %s = %s;", R(RT(w)).c_str(), conv);
      if (update)
        Line("    %s = ea;", R(RA(w)).c_str());
      RedirectCheck(a);
      Line("  }");
    }

    void Store(uint32_t a, uint32_t w, const std::string &ea, const char *bits, const std::string &val, bool update)
    {
      Line("  {");
      Line("    const uint32_t ea = %s;", ea.c_str());
      Line("    const bool x = ppcr_st%s(ea, %s, 0x%08Xu, c - %u);", bits, val.c_str(), a, m_k);
      if (update)
        Line("    %s = ea;", R(RA(w)).c_str());
      RedirectCheck(a);
      Line("  }");
    }

    void LoadFloatSingle(uint32_t a, uint32_t w, const std::string &ea, bool update)
    {
      Line("  {");
      Line("    const uint32_t ea = %s;", ea.c_str());
      Line("    uint32_t v;");
      Line("    const bool x = ppcr_ld32(ea, v, 0x%08Xu, c - %u);", a, m_k);
      Line("    S->fpr[%u].fd = (double)ppcr_u2f(v);", RT(w));
      if (update)
        Line("    %s = ea;", R(RA(w)).c_str());
      RedirectCheck(a);
      Line("  }");
    }

    void LoadFloatDouble(uint32_t a, uint32_t w, const std::string &ea, bool update)
    {
      Line("  {");
      Line("    const uint32_t ea = %s;", ea.c_str());
      Line("    uint64_t v;");
      Line("    const bool x = ppcr_ld64(ea, v, 0x%08Xu, c - %u);", a, m_k);
      Line("    S->fpr[%u].id = v;", RT(w));
      if (update)
        Line("    %s = ea;", R(RA(w)).c_str());
      RedirectCheck(a);
      Line("  }");
    }

    void Cr0(uint32_t w, uint32_t reg)
    {
      if (RC(w))
        Line("  ppcr_set_cr0(%s);", R(reg).c_str());
    }

    // Returns true if the instruction ended the block (branch or terminator)
    bool EmitInstr(uint32_t a, uint32_t w)
    {
      uint32_t op = OPCD(w);
      uint32_t rt = RT(w), ra = RA(w), rb = RB(w);
      switch (op)
      {
      case 14:  // addi
        if (ra == 0)
          Line("  %s = 0x%08Xu;", R(rt).c_str(), (uint32_t)SIMM(w));
        else
          Line("  %s = %s + 0x%08Xu;", R(rt).c_str(), R(ra).c_str(), (uint32_t)SIMM(w));
        return false;
      case 15:  // addis
        if (ra == 0)
          Line("  %s = 0x%08Xu;", R(rt).c_str(), UIMM(w) << 16);
        else
          Line("  %s = %s + 0x%08Xu;", R(rt).c_str(), R(ra).c_str(), UIMM(w) << 16);
        return false;
      case 12:  // addic
      case 13:  // addic.
        Line("  { const uint32_t i = 0x%08Xu, a = %s; %s = a + i; ppcr_set_ca(%s < a); }",
             (uint32_t)SIMM(w), R(ra).c_str(), R(rt).c_str(), R(rt).c_str());
        if (op == 13)
          Line("  ppcr_set_cr0(%s);", R(rt).c_str());
        return false;
      case 8:   // subfic
        Line("  { const uint32_t i = 0x%08Xu, a = %s; %s = i - a; ppcr_set_ca(!(i < a)); }",
             (uint32_t)SIMM(w), R(ra).c_str(), R(rt).c_str());
        return false;
      case 7:   // mulli
        Line("  %s = %s * 0x%08Xu;", R(rt).c_str(), R(ra).c_str(), (uint32_t)SIMM(w));
        return false;
      case 10:  // cmpli
        Line("  ppcr_cmpu(%u, %s, 0x%08Xu);", (w >> 23) & 7, R(ra).c_str(), UIMM(w));
        return false;
      case 11:  // cmpi
        Line("  ppcr_cmps(%u, (int32_t)%s, (int32_t)0x%08X);", (w >> 23) & 7, R(ra).c_str(), (uint32_t)SIMM(w));
        return false;
      case 24: Line("  %s = %s | 0x%08Xu;", R(ra).c_str(), R(rt).c_str(), UIMM(w)); return false;        // ori
      case 25: Line("  %s = %s | 0x%08Xu;", R(ra).c_str(), R(rt).c_str(), UIMM(w) << 16); return false;  // oris
      case 26: Line("  %s = %s ^ 0x%08Xu;", R(ra).c_str(), R(rt).c_str(), UIMM(w)); return false;        // xori
      case 27: Line("  %s = %s ^ 0x%08Xu;", R(ra).c_str(), R(rt).c_str(), UIMM(w) << 16); return false;  // xoris
      case 28:  // andi.
        Line("  %s = %s & 0x%08Xu;", R(ra).c_str(), R(rt).c_str(), UIMM(w));
        Line("  ppcr_set_cr0(%s);", R(ra).c_str());
        return false;
      case 29:  // andis.
        Line("  %s = %s & 0x%08Xu;", R(ra).c_str(), R(rt).c_str(), UIMM(w) << 16);
        Line("  ppcr_set_cr0(%s);", R(ra).c_str());
        return false;
      case 20:  // rlwimi
      case 21:  // rlwinm
      case 23:  // rlwnm
      {
        uint32_t mask = RotateMask((w >> 6) & 31, (w >> 1) & 31);
        if (op == 23)
          Line("  { const uint32_t rs = %s; const int sh = %s & 0x1f; const uint32_t r = (rs << sh) | (rs >> ((32 - sh) & 31)); %s = r & 0x%08Xu; }",
               R(rt).c_str(), R(rb).c_str(), R(ra).c_str(), mask);
        else
        {
          uint32_t sh = rb;
          std::string rot = sh ? Fmt("((rs << %u) | (rs >> %u))", sh, 32 - sh) : std::string("rs");
          if (op == 21)
            Line("  { const uint32_t rs = %s; %s = %s & 0x%08Xu; }", R(rt).c_str(), R(ra).c_str(), rot.c_str(), mask);
          else
            Line("  { const uint32_t rs = %s; %s = (%s & 0x%08Xu) | (%s & 0x%08Xu); }",
                 R(rt).c_str(), R(ra).c_str(), R(ra).c_str(), ~mask, rot.c_str(), mask);
        }
        Cr0(w, ra);
        return false;
      }
      case 32: Load(a, w, EA_D(w), "32", "v", false); return false;                  // lwz
      case 33: Load(a, w, EA_DU(w), "32", "v", true); return false;                  // lwzu
      case 34: Load(a, w, EA_D(w), "8", "(uint32_t)v", false); return false;         // lbz
      case 35: Load(a, w, EA_DU(w), "8", "(uint32_t)v", true); return false;         // lbzu
      case 40: Load(a, w, EA_D(w), "16", "(uint32_t)v", false); return false;        // lhz
      case 41: Load(a, w, EA_DU(w), "16", "(uint32_t)v", true); return false;        // lhzu
      case 42: Load(a, w, EA_D(w), "16", "(uint32_t)(int32_t)(int16_t)v", false); return false;  // lha
      case 43: Load(a, w, EA_DU(w), "16", "(uint32_t)(int32_t)(int16_t)v", true); return false;  // lhau
      case 36: Store(a, w, EA_D(w), "32", R(rt), false); return false;               // stw
      case 37: Store(a, w, EA_DU(w), "32", R(rt), true); return false;               // stwu
      case 38: Store(a, w, EA_D(w), "8", "(uint8_t)" + R(rt), false); return false;  // stb
      case 39: Store(a, w, EA_DU(w), "8", "(uint8_t)" + R(rt), true); return false;  // stbu
      case 44: Store(a, w, EA_D(w), "16", "(uint16_t)" + R(rt), false); return false;  // sth
      case 45: Store(a, w, EA_DU(w), "16", "(uint16_t)" + R(rt), true); return false;  // sthu
      case 46:  // lmw
      {
        Line("  {");
        Line("    uint32_t ea = %s;", EA_D(w).c_str());
        Line("    bool x = false;");
        for (uint32_t r = rt; r <= 31; r++)
          Line("    { uint32_t v; x |= ppcr_ld32(ea, v, 0x%08Xu, c - %u); %s = v; ea += 4; }", a, m_k, R(r).c_str());
        RedirectCheck(a);
        Line("  }");
        return false;
      }
      case 47:  // stmw
      {
        Line("  {");
        Line("    uint32_t ea = %s;", EA_D(w).c_str());
        Line("    bool x = false;");
        for (uint32_t r = rt; r <= 31; r++)
          Line("    { x |= ppcr_st32(ea, %s, 0x%08Xu, c - %u); ea += 4; }", R(r).c_str(), a, m_k);
        RedirectCheck(a);
        Line("  }");
        return false;
      }
      case 48: LoadFloatSingle(a, w, EA_D(w), false); return false;   // lfs
      case 49: LoadFloatSingle(a, w, EA_DU(w), true); return false;   // lfsu
      case 50: LoadFloatDouble(a, w, EA_D(w), false); return false;   // lfd
      case 51: LoadFloatDouble(a, w, EA_DU(w), true); return false;   // lfdu
      case 52: Store(a, w, EA_D(w), "32", Fmt("ppcr_f2u((float)S->fpr[%u].fd)", rt), false); return false;  // stfs
      case 53: Store(a, w, EA_DU(w), "32", Fmt("ppcr_f2u((float)S->fpr[%u].fd)", rt), true); return false;  // stfsu
      case 54: Store(a, w, EA_D(w), "64", Fmt("S->fpr[%u].id", rt), false); return false;   // stfd
      case 55: Store(a, w, EA_DU(w), "64", Fmt("S->fpr[%u].id", rt), true); return false;   // stfdu
      case 59: case 63:
        Handler(a, w, false);
        return false;
      case 18: return EmitB(a, w);
      case 16: return EmitBC(a, w);
      case 19: return Emit19(a, w);
      case 31: return Emit31(a, w);
      }
      return EmitTerminator(a, w);
    }

    bool EmitTerminator(uint32_t a, uint32_t w)
    {
      Handler(a, w, true);
      Line("  S->icount = c - %u;", m_k + 1);
      bool isRfi = OPCD(w) == 19 && XO(w) == 50;
      if (isRfi || OPCD(w) == 17)
      {
        Line("  return PPCR_EXIT_DISPATCH;");   // rfi / sc always leave; npc set by the handler
        return true;
      }
      Line("  if (S->npc != 0x%08Xu || S->icount == S->dec_trigger_cycle || S->fatalError) [[unlikely]] return PPCR_EXIT_DISPATCH;", a + 4);
      Jump(a + 4);
      return true;
    }

    bool EmitB(uint32_t a, uint32_t w)
    {
      uint32_t t = BranchTarget(a, w);
      EndCycles();
      if (w & 1)
      {
        Line("  S->lr = 0x%08Xu;", a + 4);
        Exit(t);
      }
      else
        Jump(t);
      return true;
    }

    bool EmitBC(uint32_t a, uint32_t w)
    {
      uint32_t t = BranchTarget(a, w);
      uint32_t bo = RT(w), bi = RA(w);
      bool lk = w & 1;
      EndCycles();
      Line("  {");
      Line("    const bool taken = ppcr_cond(%u, %u);", bo, bi);
      if (lk)
        Line("    S->lr = 0x%08Xu;", a + 4);
      Line("    if (taken)");
      if (lk)
        Exit(t, "      ");
      else
        Jump(t, "      ");
      Line("  }");
      Jump(a + 4);
      return true;
    }

    bool EmitIndirect(uint32_t a, uint32_t w, bool viaCtr)
    {
      uint32_t bo = RT(w), bi = RA(w);
      bool lk = w & 1;
      EndCycles();
      Line("  {");
      Line("    const bool taken = ppcr_cond(%u, %u);", bo, bi);
      // ppc_bcctrx reads CTR after the condition check (which may decrement it); ppc_bclrx reads LR
      Line("    const uint32_t target = S->%s & ~3u;", viaCtr ? "ctr" : "lr");
      if (lk)
        Line("    S->lr = 0x%08Xu;", a + 4);
      Line("    if (taken) { S->npc = target; return PPCR_EXIT_DISPATCH; }");
      Line("  }");
      Jump(a + 4);
      return true;
    }

    bool Emit19(uint32_t a, uint32_t w)
    {
      uint32_t rt = RT(w), ra = RA(w), rb = RB(w);
      switch (XO(w))
      {
      case 16: return EmitIndirect(a, w, false);   // bclr
      case 528: return EmitIndirect(a, w, true);   // bcctr
      case 0: Line("  S->cr[%u] = S->cr[%u];", rt >> 2, ra >> 2); return false;   // mcrf
      case 150: return false;                                                       // isync: no-op
      case 257: Line("  ppcr_set_crbit(%u, ppcr_crbit(%u) & ppcr_crbit(%u));", rt, ra, rb); return false;          // crand
      case 129: Line("  ppcr_set_crbit(%u, ppcr_crbit(%u) & (ppcr_crbit(%u) ^ 1));", rt, ra, rb); return false;    // crandc
      case 289: Line("  ppcr_set_crbit(%u, (ppcr_crbit(%u) ^ ppcr_crbit(%u)) ^ 1);", rt, ra, rb); return false;    // creqv
      case 225: Line("  ppcr_set_crbit(%u, (ppcr_crbit(%u) & ppcr_crbit(%u)) ^ 1);", rt, ra, rb); return false;    // crnand
      case 33:  Line("  ppcr_set_crbit(%u, (ppcr_crbit(%u) | ppcr_crbit(%u)) ^ 1);", rt, ra, rb); return false;    // crnor
      case 449: Line("  ppcr_set_crbit(%u, ppcr_crbit(%u) | ppcr_crbit(%u));", rt, ra, rb); return false;          // cror
      case 417: Line("  ppcr_set_crbit(%u, ppcr_crbit(%u) | (ppcr_crbit(%u) ^ 1));", rt, ra, rb); return false;    // crorc
      case 193: Line("  ppcr_set_crbit(%u, ppcr_crbit(%u) ^ ppcr_crbit(%u));", rt, ra, rb); return false;          // crxor
      }
      return EmitTerminator(a, w);
    }

    bool Emit31(uint32_t a, uint32_t w)
    {
      uint32_t rt = RT(w), ra = RA(w), rb = RB(w);
      uint32_t xo = XO(w);
      std::string RT_ = R(rt), RA_ = R(ra), RB_ = R(rb);
      const char *rd = RT_.c_str(), *rA = RA_.c_str(), *rB = RB_.c_str();

      // XO-form arithmetic (OE is bit 0x200 of xo)
      switch (xo & 0x1FF)
      {
      case 266:  // add
        Line("  { const uint32_t a = %s, b = %s; %s = a + b;%s }", rA, rB, rd, OE(w) ? Fmt(" ppcr_add_ov(%s, a, b);", rd).c_str() : "");
        Cr0(w, rt);
        return false;
      case 10:   // addc
        Line("  { const uint32_t a = %s, b = %s; %s = a + b; ppcr_set_ca(%s < a);%s }", rA, rB, rd, rd, OE(w) ? Fmt(" ppcr_add_ov(%s, a, b);", rd).c_str() : "");
        Cr0(w, rt);
        return false;
      case 138:  // adde
        Line("  { const uint32_t a = %s, b = %s, ca = (S->xer >> 29) & 1, t = b + ca; %s = a + t; ppcr_set_ca(t < b || %s < a);%s }",
             rA, rB, rd, rd, OE(w) ? Fmt(" ppcr_add_ov(%s, a, b);", rd).c_str() : "");
        Cr0(w, rt);
        return false;
      case 202:  // addze
        Line("  { const uint32_t a = %s, ca = (S->xer >> 29) & 1; %s = a + ca; ppcr_set_ca(%s < a);%s }",
             rA, rd, rd, OE(w) ? Fmt(" ppcr_add_ov(%s, a, ca);", rd).c_str() : "");
        Cr0(w, rt);
        return false;
      case 234:  // addme
        Line("  { const uint32_t a = %s, ca = (S->xer >> 29) & 1, t = a + ca; %s = t + 0xFFFFFFFFu; ppcr_set_ca(t < a || %s < t);%s }",
             rA, rd, rd, OE(w) ? Fmt(" ppcr_add_ov(%s, a, ca - 1);", rd).c_str() : "");
        Cr0(w, rt);
        return false;
      case 40:   // subf
        Line("  { const uint32_t a = %s, b = %s; %s = b - a;%s }", rA, rB, rd, OE(w) ? Fmt(" ppcr_sub_ov(%s, b, a);", rd).c_str() : "");
        Cr0(w, rt);
        return false;
      case 8:    // subfc
        Line("  { const uint32_t a = %s, b = %s; %s = b - a; ppcr_set_ca(!(b < a));%s }", rA, rB, rd, OE(w) ? Fmt(" ppcr_sub_ov(%s, b, a);", rd).c_str() : "");
        Cr0(w, rt);
        return false;
      case 104:  // neg
        Line("  %s = 0u - %s;", rd, rA);
        if (OE(w))
          Line("  if (%s == 0x80000000u) S->xer |= PPCR_XER_OV | PPCR_XER_SO; else S->xer &= ~PPCR_XER_OV;", rd);
        Cr0(w, rt);
        return false;
      case 235:  // mullw
        Line("  {");
        Line("    const int64_t r = (int64_t)(int32_t)%s * (int64_t)(int32_t)%s;", rA, rB);
        Line("    %s = (uint32_t)r;", rd);
        if (OE(w))
          Line("    S->xer &= ~PPCR_XER_OV; if (r != (int64_t)(int32_t)r) S->xer |= PPCR_XER_OV | PPCR_XER_SO;");
        Line("  }");
        Cr0(w, rt);
        return false;
      case 491:  // divw
        Line("  {");
        Line("    const uint32_t a = %s, b = %s;", rA, rB);
        Line("    if (b == 0 && a < 0x80000000u) { %s = 0;%s }", rd, OE(w) ? " S->xer |= PPCR_XER_SO | PPCR_XER_OV;" : "");
        Line("    else if (b == 0 || (b == 0xFFFFFFFFu && a == 0x80000000u)) { %s = 0xFFFFFFFFu;%s }", rd, OE(w) ? " S->xer |= PPCR_XER_SO | PPCR_XER_OV;" : "");
        Line("    else { %s = (uint32_t)((int32_t)a / (int32_t)b);%s }", rd, OE(w) ? " S->xer &= ~PPCR_XER_OV;" : "");
        Line("  }");
        Cr0(w, rt);
        return false;
      case 459:  // divwu
        Line("  {");
        Line("    const uint32_t a = %s, b = %s;", rA, rB);
        Line("    if (b == 0) { %s = 0;%s }", rd, OE(w) ? " S->xer |= PPCR_XER_SO | PPCR_XER_OV;" : "");
        Line("    else { %s = a / b;%s }", rd, OE(w) ? " S->xer &= ~PPCR_XER_OV;" : "");
        Line("  }");
        Cr0(w, rt);
        return false;
      case 136: case 200: case 232:  // subfe, subfze, subfme: rare, use the handler
        Handler(a, w, false);
        return false;
      }

      // X-form logical ops write rA from rS (in the rt field) and rB
      auto logical = [&](const char *expr) {
        Line("  %s = %s;", rA, Fmt(expr, rd, rB).c_str());
        Cr0(w, ra);
        return false;
      };
      switch (xo)
      {
      case 75:   // mulhw
        Line("  %s = (uint32_t)(((int64_t)(int32_t)%s * (int64_t)(int32_t)%s) >> 32);", rd, rA, rB);
        Cr0(w, rt);
        return false;
      case 11:   // mulhwu
        Line("  %s = (uint32_t)(((uint64_t)%s * (uint64_t)%s) >> 32);", rd, rA, rB);
        Cr0(w, rt);
        return false;
      case 28:  return logical("%s & %s");        // and
      case 60:  return logical("%s & ~%s");       // andc
      case 444: return logical("%s | %s");        // or
      case 412: return logical("%s | ~%s");       // orc
      case 124: return logical("~(%s | %s)");     // nor
      case 476: return logical("~(%s & %s)");     // nand
      case 316: return logical("%s ^ %s");        // xor
      case 284: return logical("~(%s ^ %s)");     // eqv
      case 954:  // extsb
        Line("  %s = (uint32_t)(int32_t)(int8_t)%s;", rA, rd);
        Cr0(w, ra);
        return false;
      case 922:  // extsh
        Line("  %s = (uint32_t)(int32_t)(int16_t)%s;", rA, rd);
        Cr0(w, ra);
        return false;
      case 26:   // cntlzw
        Line("  %s = ppcr_cntlzw(%s);", rA, rd);
        Cr0(w, ra);
        return false;
      case 24:   // slw
        Line("  { const int sh = %s & 0x3f; %s = (sh > 31) ? 0u : (%s << sh); }", rB, rA, rd);
        Cr0(w, ra);
        return false;
      case 536:  // srw
        Line("  { const int sh = %s & 0x3f; %s = (sh > 31) ? 0u : (%s >> sh); }", rB, rA, rd);
        Cr0(w, ra);
        return false;
      case 792:  // sraw
        Line("  {");
        Line("    const int sh = %s & 0x3f; const uint32_t rs = %s;", rB, rd);
        Line("    S->xer &= ~PPCR_XER_CA;");
        Line("    if (sh > 31) { %s = (rs & 0x80000000u) ? 0xFFFFFFFFu : 0u; if (%s) S->xer |= PPCR_XER_CA; }", rA, rA);
        Line("    else { %s = (uint32_t)((int32_t)rs >> sh); if ((int32_t)rs < 0 && (rs & (uint32_t)((1ull << sh) - 1))) S->xer |= PPCR_XER_CA; }", rA);
        Line("  }");
        Cr0(w, ra);
        return false;
      case 824:  // srawi
      {
        uint32_t sh = rb;
        uint32_t mask = (uint32_t)((1ull << sh) - 1);
        Line("  { const uint32_t rs = %s; S->xer &= ~PPCR_XER_CA; if ((int32_t)rs < 0 && (rs & 0x%08Xu)) S->xer |= PPCR_XER_CA; %s = (uint32_t)((int32_t)rs >> %u); }",
             rd, mask, rA, sh);
        Cr0(w, ra);
        return false;
      }
      case 0:    // cmp
        Line("  ppcr_cmps(%u, (int32_t)%s, (int32_t)%s);", (w >> 23) & 7, rA, rB);
        return false;
      case 32:   // cmpl
        Line("  ppcr_cmpu(%u, %s, %s);", (w >> 23) & 7, rA, rB);
        return false;
      case 23:  Load(a, w, EA_X(w), "32", "v", false); return false;                                // lwzx
      case 55:  Load(a, w, EA_XU(w), "32", "v", true); return false;                                // lwzux
      case 87:  Load(a, w, EA_X(w), "8", "(uint32_t)v", false); return false;                       // lbzx
      case 119: Load(a, w, EA_XU(w), "8", "(uint32_t)v", true); return false;                       // lbzux
      case 279: Load(a, w, EA_X(w), "16", "(uint32_t)v", false); return false;                      // lhzx
      case 311: Load(a, w, EA_XU(w), "16", "(uint32_t)v", true); return false;                      // lhzux
      case 343: Load(a, w, EA_X(w), "16", "(uint32_t)(int32_t)(int16_t)v", false); return false;    // lhax
      case 151: Store(a, w, EA_X(w), "32", RT_, false); return false;                               // stwx
      case 183: Store(a, w, EA_XU(w), "32", RT_, true); return false;                               // stwux
      case 215: Store(a, w, EA_X(w), "8", "(uint8_t)" + RT_, false); return false;                  // stbx
      case 407: Store(a, w, EA_X(w), "16", "(uint16_t)" + RT_, false); return false;                // sthx
      case 535: LoadFloatSingle(a, w, EA_X(w), false); return false;                                // lfsx
      case 599: LoadFloatDouble(a, w, EA_X(w), false); return false;                                // lfdx
      case 663: Store(a, w, EA_X(w), "32", Fmt("ppcr_f2u((float)S->fpr[%u].fd)", rt), false); return false;  // stfsx
      case 727: Store(a, w, EA_X(w), "64", Fmt("S->fpr[%u].id", rt), false); return false;          // stfdx
      case 19:   // mfcr
        Line("  %s = ppcr_get_cr();", rd);
        return false;
      case 144:  // mtcrf
      {
        uint32_t fxm = (w >> 12) & 0xFF;
        for (int f = 0; f < 8; f++)
          if (fxm & (0x80 >> f))
            Line("  S->cr[%d] = (uint8_t)((%s >> %d) & 0xf);", f, rd, 28 - 4 * f);
        return false;
      }
      case 83:   // mfmsr
        Line("  %s = S->msr;", rd);
        return false;
      case 339:  // mfspr
      {
        uint32_t spr = SPRN(w);
        if (spr == 1) Line("  %s = S->xer;", rd);
        else if (spr == 8) Line("  %s = S->lr;", rd);
        else if (spr == 9) Line("  %s = S->ctr;", rd);
        else Handler(a, w, true);   // DEC/TB reads depend on icount
        return false;
      }
      case 467:  // mtspr
      {
        uint32_t spr = SPRN(w);
        if (spr == 1) { Line("  S->xer = %s;", rd); return false; }
        if (spr == 8) { Line("  S->lr = %s;", rd); return false; }
        if (spr == 9) { Line("  S->ctr = %s;", rd); return false; }
        return EmitTerminator(a, w);
      }
      case 598: case 854: case 86: case 470: case 54: case 278: case 246: case 982:
        return false;   // sync, eieio, dcbf, dcbi, dcbst, dcbt, dcbtst, icbi: no-ops in Supermodel
      case 371: case 595: case 659: case 512:
        Handler(a, w, true);   // mftb (reads icount), mfsr, mfsrin, mcrxr
        return false;
      }
      if (IsMemoryHandler31(xo))
      {
        MemoryHandler(a, w);
        return false;
      }
      return EmitTerminator(a, w);
    }
  };

  // ---------------------------------------------------------------------------------
  // Output
  // ---------------------------------------------------------------------------------

  void WriteFile(const std::string &path, const std::string &text)
  {
    std::ofstream out(path, std::ios::binary);
    if (!out)
      Fatal("cannot write %s", path.c_str());
    out << text;
  }

  const char *kHeader =
    "// Generated by tools/ppcrecomp from the user's ROM set. Do not edit or distribute.\n"
    "#include \"ppc_state.h\"\n\n";
}

int main(int argc, char **argv)
{
  std::string cfgPath, cromPath, outDir;
  std::vector<std::string> seedPaths;
  int shards = 16;
  for (int i = 1; i < argc; i++)
  {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc)
        Fatal("%s needs an argument", a.c_str());
      return argv[++i];
    };
    if (a == "--config") cfgPath = next();
    else if (a == "--crom") cromPath = next();
    else if (a == "--seeds") seedPaths.push_back(next());
    else if (a == "--shards") shards = atoi(next().c_str());
    else if (a == "--out") outDir = next();
    else Fatal("unknown option %s", a.c_str());
  }
  if (cfgPath.empty() || cromPath.empty() || outDir.empty() || shards < 1)
    Fatal("usage: ppcrecomp --config <cfg> --crom <crom.bin> [--seeds <file>]... --shards <n> --out <dir>");

  Config cfg = LoadConfig(cfgPath);
  std::filesystem::create_directories(outDir);

  Image img;
  {
    std::ifstream in(cromPath, std::ios::binary);
    if (!in)
      Fatal("cannot open %s", cromPath.c_str());
    img.crom.assign(std::istreambuf_iterator<char>(in), {});
    if (img.crom.size() != CROM_SIZE)
      Fatal("%s: expected 8 MB fixed CROM image, got %zu bytes", cromPath.c_str(), img.crom.size());
  }
  img.ram.assign(RAM_SIZE, 0);
  img.ramValid.assign(RAM_SIZE / 4, false);
  for (auto &rc : cfg.ramCopies)
  {
    if (rc.src < CROM_BASE || rc.dst + rc.size > RAM_SIZE || (rc.dst | rc.src | rc.size) & 3)
      Fatal("bad ram_copy %08X %08X %X", rc.dst, rc.src, rc.size);
    memcpy(&img.ram[rc.dst], &img.crom[rc.src - CROM_BASE], rc.size);
    for (uint32_t a = rc.dst; a < rc.dst + rc.size; a += 4)
      img.ramValid[a / 4] = true;
  }

  // Seeds: config entries, then seed files
  std::set<uint32_t> entries;
  std::map<uint32_t, std::string> names;
  for (auto &[addr, name] : cfg.entries)
  {
    entries.insert(addr);
    if (!name.empty())
      names[addr] = name;
  }
  size_t fileSeeds = 0, unusable = 0;
  for (auto &p : seedPaths)
    for (uint32_t s : LoadSeeds(p))
    {
      uint32_t w;
      if (cfg.excludes.count(s))
        continue;
      if (!img.Fetch(s, w))
      {
        unusable++;
        continue;
      }
      fileSeeds += entries.insert(s).second;
    }

  // Iterate until the set of function entries (seeds + discovered call targets) is stable
  Analyzer an(img, cfg.excludes);
  std::map<uint32_t, Unit> units;
  std::set<uint32_t> callTargets;
  for (int pass = 0;; pass++)
  {
    std::set<uint32_t> calls;
    units.clear();
    for (uint32_t e : entries)
      units.emplace(e, an.Explore(e, entries, calls));
    callTargets = calls;
    size_t before = entries.size();
    entries.insert(calls.begin(), calls.end());
    if (entries.size() == before)
      break;
    if (pass > 64)
      Fatal("entry discovery did not converge");
  }

  // A seed that lands inside code another unit already covers (an interrupt return
  // point, or a Ghidra function the code falls through into) does not need a unit of
  // its own: split a block there instead. Config entries and call targets always keep
  // their own unit. Larger units are kept first so the big ones absorb the small ones.
  size_t merged = 0;
  {
    std::vector<Unit *> bySize;
    for (auto &[e, u] : units)
      bySize.push_back(&u);
    std::stable_sort(bySize.begin(), bySize.end(), [](const Unit *a, const Unit *b) {
      return a->instrs.size() > b->instrs.size();
    });
    auto mandatory = [&](uint32_t e) { return names.count(e) || callTargets.count(e) ||
                                              std::any_of(cfg.entries.begin(), cfg.entries.end(), [&](auto &p) { return p.first == e; }); };
    std::map<uint32_t, Unit *> owner;   // instruction -> first kept unit covering it
    std::vector<uint32_t> drop;
    for (int round = 0; round < 2; round++)       // round 0: mandatory units, round 1: the rest
      for (Unit *u : bySize)
      {
        bool m = mandatory(u->entry);
        if ((round == 0) != m)
          continue;
        auto it = owner.find(u->entry);
        if (!m && it != owner.end())
        {
          it->second->leaders.insert(u->entry);
          drop.push_back(u->entry);
          continue;
        }
        for (uint32_t a : u->instrs)
          owner.emplace(a, u);
      }
    for (uint32_t e : drop)
      units.erase(e);
    merged = drop.size();
  }

  // Assign RAM unit indices and collect RAM code ranges
  std::vector<const Unit *> ordered;
  for (auto &[e, u] : units)
    ordered.push_back(&u);
  std::map<uint32_t, int> ramIndex;
  struct Range { uint32_t start, words, offset; };
  std::vector<Range> ranges;
  std::vector<std::pair<uint32_t, uint32_t>> ramUnitRanges;   // first, count
  std::vector<uint32_t> ramCode;
  for (const Unit *u : ordered)
  {
    if (!u->inRam())
      continue;
    ramIndex[u->entry] = (int)ramUnitRanges.size();
    uint32_t first = (uint32_t)ranges.size();
    uint32_t start = 0, prev = 0;
    bool open = false;
    auto close = [&]() {
      uint32_t words = (prev - start) / 4 + 1;
      ranges.push_back({start, words, (uint32_t)ramCode.size()});
      for (uint32_t a = start; a <= prev; a += 4)
      {
        uint32_t w;
        img.Fetch(a, w);
        ramCode.push_back(w);
      }
    };
    for (uint32_t a : u->instrs)
    {
      if (open && a == prev + 4)
      {
        prev = a;
        continue;
      }
      if (open)
        close();
      start = prev = a;
      open = true;
    }
    if (open)
      close();
    ramUnitRanges.push_back({first, (uint32_t)ranges.size() - first});
  }

  // Generate units into shards
  std::vector<std::string> shardText(shards, kHeader);
  size_t totalInstrs = 0;
  for (size_t i = 0; i < ordered.size(); i++)
  {
    const Unit *u = ordered[i];
    auto it = ramIndex.find(u->entry);
    Emitter em(img, *u, it == ramIndex.end() ? -1 : it->second);
    std::string code = em.Generate();
    auto name = names.find(u->entry);
    if (name != names.end())
      shardText[i % shards] += "// " + name->second + "\n";
    shardText[i % shards] += code + "\n";
    totalInstrs += u->instrs.size();
  }
  for (int s = 0; s < shards; s++)
    WriteFile(Fmt("%s/ppc_recomp_%02d.cpp", outDir.c_str(), s), shardText[s]);

  // Entry table: a unit's own entry wins over other units that contain the same address
  std::map<uint32_t, uint32_t> table;   // addr -> unit entry
  for (const Unit *u : ordered)
    table[u->entry] = u->entry;
  for (const Unit *u : ordered)
    for (uint32_t l : u->leaders)
      table.emplace(l, u->entry);

  std::ostringstream t;
  t << kHeader;
  for (const Unit *u : ordered)
    t << "int " << UnitName(u->entry) << "(uint32_t entry);\n";
  t << "\nconst PPCRUnitEntry ppcr_units[] = {\n";
  for (auto &[addr, entry] : table)
    t << Fmt("  {0x%08Xu, %s},\n", addr, UnitName(entry).c_str());
  t << "};\nconst uint32_t ppcr_num_units = " << table.size() << ";\n\n";
  t << "const PPCRRamUnit ppcr_ram_units[] = {\n";
  for (auto &[first, count] : ramUnitRanges)
    t << Fmt("  {%u, %u},\n", first, count);
  if (ramUnitRanges.empty())
    t << "  {0, 0},\n";
  t << "};\nconst uint32_t ppcr_num_ram_units = " << ramUnitRanges.size() << ";\n\n";
  t << "const PPCRRamRange ppcr_ram_ranges[] = {\n";
  for (auto &r : ranges)
    t << Fmt("  {0x%08Xu, %u, %u},\n", r.start, r.words, r.offset);
  if (ranges.empty())
    t << "  {0, 0, 0},\n";
  t << "};\nconst uint32_t ppcr_num_ram_ranges = " << ranges.size() << ";\n\n";
  t << "const uint32_t ppcr_ram_code[] = {";
  for (size_t i = 0; i < ramCode.size(); i++)
    t << (i % 8 ? " " : "\n  ") << Fmt("0x%08Xu,", ramCode[i]);
  if (ramCode.empty())
    t << "0";
  t << "\n};\n";
  WriteFile(outDir + "/ppc_table.cpp", t.str());

  printf("ppcrecomp: %zu units (%zu in RAM), %zu entry points, %zu instructions; %zu new seeds from files "
         "(%zu folded into other units), %zu unusable\n",
         units.size(), ramUnitRanges.size(), table.size(), totalInstrs, fileSeeds, merged, unusable);
  return 0;
}
