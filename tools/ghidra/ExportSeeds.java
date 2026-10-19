// Ghidra post-analysis script: writes ppcrecomp seeds.
//
// Args: <output file>
//
// Writes one hex address per line for every function entry and every target of a
// computed jump (switch tables) or computed call that Ghidra resolved, so ppcrecomp
// can compile code the game only reaches through indirect branches.
//@category Model3

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.RefType;

import java.io.PrintWriter;
import java.util.TreeSet;

public class ExportSeeds extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 1) {
            printerr("usage: ExportSeeds.java <out>");
            return;
        }
        TreeSet<Long> seeds = new TreeSet<>();
        int functions = 0, computed = 0;
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            seeds.add(f.getEntryPoint().getOffset());
            functions++;
        }
        ReferenceIterator refs = currentProgram.getReferenceManager().getReferenceIterator(toAddr(0));
        while (refs.hasNext()) {
            Reference r = refs.next();
            RefType t = r.getReferenceType();
            if ((t.isComputed() && (t.isJump() || t.isCall())) && r.getToAddress().isMemoryAddress()) {
                Address to = r.getToAddress();
                if (currentProgram.getMemory().getBlock(to) != null &&
                    currentProgram.getMemory().getBlock(to).isExecute()) {
                    seeds.add(to.getOffset());
                    computed++;
                }
            }
        }
        try (PrintWriter out = new PrintWriter(args[0])) {
            out.println("# Exported from Ghidra by tools/ghidra/ExportSeeds.java");
            for (long s : seeds) out.println(String.format("%08X", s));
        }
        println(String.format("Exported %d seeds (%d functions, %d computed branch targets)", seeds.size(), functions, computed));
    }
}
