// Ghidra pre-analysis script for a Model 3 (Step 2.x) fixed CROM image.
//
// Args: <ppcrecomp config> [seed file]...
//
// Expects the program to be crom.bin imported raw at 0xFF800000 as PowerPC:BE:32:default
// (see analyze.sh). It then
//   - creates the RAM code image described by the config's ram_copy lines,
//   - adds uninitialized blocks for the main MMIO regions (from Supermodel's
//     CModel3::Read32/Write32 decode), so references to them get sensible labels,
//   - marks config entries and seed addresses as entry points and disassembles them.
//@category Model3

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;

import java.io.BufferedReader;
import java.io.ByteArrayInputStream;
import java.io.FileReader;

public class SetupModel3 extends GhidraScript {

    private static final String[][] MMIO = {
        // name, start, size
        {"REAL3D_REGS",    "0x84000000", "0x100"},
        {"REAL3D_CMD",     "0x88000000", "0x1000000"},
        {"REAL3D_CULLING", "0x8C000000", "0x1000000"},
        {"REAL3D_POLY",    "0x98000000", "0x1000000"},
        {"REAL3D_DMA",     "0xC2000000", "0x100"},
        {"INPUTS",         "0xF0040000", "0x100"},
        {"SOUND_MIDI",     "0xF0080000", "0x10"},
        {"BACKUP_RAM",     "0xF00C0000", "0x20000"},
        {"SYSTEM_REGS",    "0xF0100000", "0x100"},
        {"RTC",            "0xF0140000", "0x100"},
        {"SECURITY_RAM",   "0xF0180000", "0x20000"},
        {"SECURITY_REGS",  "0xF01A0000", "0x100"},
        {"PCI_MPC106",     "0xF0E00000", "0x100000"},
        {"TILEGEN",        "0xF1000000", "0x200000"},
    };

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            printerr("usage: SetupModel3.java <config> [seeds]...");
            return;
        }
        Memory mem = currentProgram.getMemory();

        int entries = 0;
        try (BufferedReader in = new BufferedReader(new FileReader(args[0]))) {
            String line;
            while ((line = in.readLine()) != null) {
                int hash = line.indexOf('#');
                if (hash >= 0) line = line.substring(0, hash);
                String[] f = line.trim().split("\\s+");
                if (f.length >= 4 && f[0].equals("ram_copy")) {
                    long dst = Long.decode(f[1]), src = Long.decode(f[2]), size = Long.decode(f[3]);
                    byte[] bytes = new byte[(int) size];
                    mem.getBytes(toAddr(src), bytes);
                    Address at = toAddr(dst);
                    if (mem.getBlock(at) == null) {
                        MemoryBlock b = mem.createInitializedBlock(String.format("RAM_%08X", dst), at,
                            new ByteArrayInputStream(bytes), size, monitor, false);
                        b.setExecute(true);
                        b.setWrite(true);
                        println(String.format("RAM code image %08X-%08X from CROM %08X", dst, dst + size, src));
                    }
                } else if (f.length >= 2 && f[0].equals("entry")) {
                    markEntry(toAddr(Long.decode(f[1])), f.length >= 3 ? f[2] : null);
                    entries++;
                }
            }
        }

        for (String[] m : MMIO) {
            Address at = toAddr(Long.decode(m[1]));
            if (mem.getBlock(at) == null) {
                MemoryBlock b = mem.createUninitializedBlock(m[0], at, Long.decode(m[2]), false);
                b.setVolatile(true);
            }
        }

        for (int i = 1; i < args.length; i++) {
            try (BufferedReader in = new BufferedReader(new FileReader(args[i]))) {
                String line;
                while ((line = in.readLine()) != null) {
                    int hash = line.indexOf('#');
                    if (hash >= 0) line = line.substring(0, hash);
                    line = line.trim();
                    if (line.isEmpty()) continue;
                    Address a = toAddr(Long.parseLong(line, 16));
                    if (mem.contains(a)) {
                        markEntry(a, null);
                        entries++;
                    }
                }
            }
        }
        println("Marked " + entries + " entry points");
    }

    private void markEntry(Address a, String name) throws Exception {
        if (!currentProgram.getMemory().contains(a)) return;
        disassemble(a);
        if (getFunctionAt(a) == null) createFunction(a, null);
        if (name != null) createLabel(a, name, true, SourceType.USER_DEFINED);
    }
}
