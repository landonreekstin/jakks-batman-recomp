// Seeds the µ'nSP vectors + main, lets Ghidra follow calls, then exports the function list.
// Usage: -postScript SeedAndExport.java <out.tsv>
import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.SourceType;
import java.io.PrintWriter;

public class SeedAndExport extends GhidraScript {
    @Override
    public void run() throws Exception {
        String out = getScriptArgs()[0];
        String[][] seeds = {
            {"0x5dcd", "reset"}, {"0x5cd0", "irq_handler"}, {"0x5cce", "fiq_break_stub"},
            {"0xdbfa8", "main"},
        };
        for (String[] s : seeds) {
            Address a = toAddr(s[0]);
            new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
            createFunction(a, s[1]);
        }
        analyzeChanges(currentProgram);
        try (PrintWriter w = new PrintWriter(out)) {
            w.println("addr\tname\tsize_words\tcallers\tcallees");
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                w.printf("%06x\t%s\t%d\t%d\t%d%n", f.getEntryPoint().getAddressableWordOffset(), f.getName(),
                    f.getBody().getNumAddresses(), f.getCallingFunctions(monitor).size(),
                    f.getCalledFunctions(monitor).size());
            }
        }
        println("functions: " + currentProgram.getFunctionManager().getFunctionCount());
        println("instructions: " + currentProgram.getListing().getNumInstructions());
    }
}
