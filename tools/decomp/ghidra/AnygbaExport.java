// Exports guest functions of a GBA ROM for `anygba decomp` (tools/decomp/decomp.py,
// docs/dev/OVERRIDES.md): pseudo-C, disassembly, callees, globals and known symbol names, as JSON.
//
// Run by analyzeHeadless as a post-script with two arguments: a request file and the output path.
// Request lines: "func <hex addr> <arm|thumb> [name]" (export it) and "sym <hex addr> <arm|thumb|data>
// <name>" (a known name). The program is imported without auto-analysis; this script adds the GBA
// memory map, disassembles from each entry and creates the functions it needs, and the project keeps
// that work for the next run.
//@category AnyGBA

import com.google.gson.Gson;
import com.google.gson.GsonBuilder;
import com.google.gson.JsonArray;
import com.google.gson.JsonObject;

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FlowOverride;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;

import java.math.BigInteger;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

public class AnygbaExport extends GhidraScript {

    private record Entry(long addr, boolean thumb, String name) {}

    private static final long[][] REGIONS = {
        // base, size, writable, volatile
        {0x02000000L, 0x40000L, 1, 0},
        {0x03000000L, 0x8000L, 1, 0},
        {0x04000000L, 0x400L, 1, 1},
        {0x05000000L, 0x400L, 1, 0},
        {0x06000000L, 0x18000L, 1, 0},
        {0x07000000L, 0x400L, 1, 0},
    };
    private static final String[] REGION_NAMES = {"EWRAM", "IWRAM", "IO", "PALETTE", "VRAM", "OAM"};

    private final Map<Long, String> names = new TreeMap<>();

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 2) throw new IllegalArgumentException("usage: AnygbaExport.java <request file> <out.json>");
        List<Entry> funcs = new ArrayList<>();
        List<Entry> syms = new ArrayList<>();
        for (String raw : Files.readAllLines(Path.of(args[0]), StandardCharsets.UTF_8)) {
            String line = raw.strip();
            if (line.isEmpty() || line.startsWith("#")) continue;
            String[] p = line.split("\\s+");
            if (p.length < 3) throw new IllegalArgumentException("bad request line: " + line);
            long addr = Long.parseLong(p[1].replaceFirst("^0[xX]", ""), 16);
            Entry e = new Entry(addr & ~1L, p[2].equals("thumb"), p.length > 3 ? p[3] : null);
            if (p[0].equals("func")) funcs.add(e);
            else if (p[0].equals("sym")) syms.add(e);
            else throw new IllegalArgumentException("bad request line: " + line);
        }

        setUpMemory();
        for (Entry s : syms) names.put(s.addr, s.name);
        for (Entry f : funcs) {
            if (f.name != null) names.put(f.addr, f.name);
        }
        for (Entry s : syms) {
            if (!s.name.isEmpty()) label(toAddr(s.addr), s.name);
        }

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        JsonArray out = new JsonArray();
        for (Entry f : funcs) {
            out.add(export(decomp, f));
        }
        decomp.dispose();

        JsonObject root = new JsonObject();
        root.addProperty("program", currentProgram.getName());
        root.addProperty("ghidra", getGhidraVersion());
        root.add("functions", out);
        Gson gson = new GsonBuilder().setPrettyPrinting().disableHtmlEscaping().create();
        Files.writeString(Path.of(args[1]), gson.toJson(root), StandardCharsets.UTF_8);
        println("anygba export: " + funcs.size() + " functions written to " + args[1]);
    }

    private void setUpMemory() throws Exception {
        Memory mem = currentProgram.getMemory();
        for (int i = 0; i < REGIONS.length; ++i) {
            Address base = toAddr(REGIONS[i][0]);
            if (mem.getBlock(base) != null) continue;
            MemoryBlock b = mem.createUninitializedBlock(REGION_NAMES[i], base, REGIONS[i][1], false);
            b.setRead(true);
            b.setWrite(REGIONS[i][2] != 0);
            b.setExecute(i <= 1);
            b.setVolatile(REGIONS[i][3] != 0);
        }
        // The cartridge is read-only, so the decompiler folds literal-pool loads into constants.
        MemoryBlock rom = mem.getBlock(toAddr(0x08000000L));
        if (rom == null) throw new IllegalStateException("the ROM is not loaded at 0x08000000");
        rom.setWrite(false);
        rom.setExecute(true);
    }

    private void label(Address a, String name) throws Exception {
        Symbol primary = currentProgram.getSymbolTable().getPrimarySymbol(a);
        if (primary != null && primary.getName().equals(name)) return;
        Function f = currentProgram.getFunctionManager().getFunctionAt(a);
        if (f != null) {
            f.setName(name, SourceType.USER_DEFINED);
        } else {
            currentProgram.getSymbolTable().createLabel(a, name, SourceType.USER_DEFINED);
        }
    }

    private Function ensureFunction(long addr, boolean thumb) throws Exception {
        Address a = toAddr(addr);
        Register tmode = currentProgram.getProgramContext().getRegister("TMode");
        if (getInstructionAt(a) == null) {
            currentProgram.getProgramContext().setValue(tmode, a, a, thumb ? BigInteger.ONE : BigInteger.ZERO);
            new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
        }
        Function f = getFunctionAt(a);
        if (f == null) {
            new CreateFunctionCmd(a).applyTo(currentProgram, monitor);
            f = getFunctionAt(a);
        }
        if (f == null) throw new IllegalStateException(String.format("cannot create a function at 0x%08x", addr));
        if (fixReturns(f)) CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);
        String name = names.get(addr);
        if (name != null && !f.getName().equals(name)) f.setName(name, SourceType.USER_DEFINED);
        return f;
    }

    // Thumb code returns with `pop {rN}; bx rN` (interworking returns). Ghidra reads that bx as a
    // computed jump, often as a jump table into the following code; mark it as a return.
    private boolean fixReturns(Function f) {
        boolean changed = false;
        InstructionIterator it = currentProgram.getListing().getInstructions(f.getBody(), true);
        Instruction prev = null;
        while (it.hasNext()) {
            Instruction in = it.next();
            if (in.getMnemonicString().equalsIgnoreCase("bx") && in.getNumOperands() == 1 && prev != null &&
                prev.getMnemonicString().equalsIgnoreCase("pop") && in.getFlowOverride() != FlowOverride.RETURN) {
                Register r = in.getRegister(0);
                if (r != null && !r.getName().equalsIgnoreCase("lr") && popsRegister(prev, r.getName())) {
                    in.setFlowOverride(FlowOverride.RETURN);
                    changed = true;
                }
            }
            prev = in;
        }
        return changed;
    }

    private static boolean popsRegister(Instruction pop, String reg) {
        String text = pop.toString().toLowerCase();
        int open = text.indexOf('{');
        int close = text.indexOf('}');
        if (open < 0 || close < open) return false;
        for (String r : text.substring(open + 1, close).split(",")) {
            if (r.strip().equals(reg.toLowerCase())) return true;
        }
        return false;
    }

    private JsonObject export(DecompInterface decomp, Entry e) throws Exception {
        Function f = ensureFunction(e.addr, e.thumb);
        // A fixed return can end the body earlier: disassemble and fix until the body is stable.
        for (int i = 0; i < 4 && fixReturns(f); ++i) CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);

        JsonObject j = new JsonObject();
        j.addProperty("addr", String.format("0x%08x", e.addr));
        j.addProperty("mode", e.thumb ? "thumb" : "arm");
        j.addProperty("name", f.getName());

        JsonArray asm = new JsonArray();
        Map<Long, JsonObject> callees = new LinkedHashMap<>();
        Map<Long, JsonObject> globals = new TreeMap<>();
        List<String> fixed = new ArrayList<>();
        AddressSet body = new AddressSet(f.getBody());
        long end = e.addr;
        InstructionIterator it = currentProgram.getListing().getInstructions(body, true);
        while (it.hasNext()) {
            Instruction in = it.next();
            long at = in.getAddress().getOffset();
            end = Math.max(end, at + in.getLength());
            JsonObject line = new JsonObject();
            line.addProperty("addr", String.format("0x%08x", at));
            line.addProperty("text", in.toString());
            if (in.getFlowOverride() == FlowOverride.RETURN) {
                line.addProperty("note", "return (pop + bx)");
                fixed.add(String.format("0x%08x", at));
            }
            asm.add(line);
            for (Reference ref : in.getReferencesFrom()) {
                long to = ref.getToAddress().getOffset();
                if (ref.getReferenceType().isCall()) {
                    if (!callees.containsKey(to)) {
                        boolean thumb = isThumb(in, ref);
                        JsonObject c = new JsonObject();
                        c.addProperty("addr", String.format("0x%08x", to));
                        c.addProperty("mode", thumb ? "thumb" : "arm");
                        c.addProperty("call_site", String.format("0x%08x", at));
                        String name = names.get(to);
                        if (name == null) {
                            Function cf = getFunctionAt(ref.getToAddress());
                            name = cf != null ? cf.getName() : null;
                        }
                        c.addProperty("name", name != null ? name : String.format("FUN_%08x", to));
                        c.addProperty("known", names.containsKey(to));
                        callees.put(to, c);
                    }
                } else if (ref.getReferenceType().isData()) {
                    addGlobal(globals, to, at, ref.getReferenceType().isWrite() ? "write" : "read");
                    // A literal-pool word in the ROM that holds an address: the global it points to.
                    if (to >= 0x08000000L && to < 0x0E000000L) {
                        try {
                            long value = currentProgram.getMemory().getInt(ref.getToAddress()) & 0xFFFFFFFFL;
                            if (value >= 0x02000000L && value < 0x0E000000L) addGlobal(globals, value, at, "pool");
                        } catch (Exception ignored) {
                            // not a readable word: no pool value
                        }
                    }
                }
            }
        }
        j.addProperty("size", end - e.addr);
        j.add("disassembly", asm);
        JsonArray fixedArr = new JsonArray();
        fixed.forEach(fixedArr::add);
        j.add("fixed_returns", fixedArr);
        JsonArray calleeArr = new JsonArray();
        callees.values().forEach(calleeArr::add);
        j.add("callees", calleeArr);
        JsonArray globalArr = new JsonArray();
        globals.values().forEach(globalArr::add);
        j.add("globals", globalArr);

        DecompileResults r = decomp.decompileFunction(f, 120, monitor);
        if (r != null && r.decompileCompleted() && r.getDecompiledFunction() != null) {
            j.addProperty("pseudo_c", r.getDecompiledFunction().getC());
            j.addProperty("signature", r.getDecompiledFunction().getSignature());
        } else {
            j.addProperty("pseudo_c", "");
            j.addProperty("decompile_error", r == null ? "no result" : r.getErrorMessage());
        }
        return j;
    }

    // bl keeps the instruction set, blx switches it.
    private boolean isThumb(Instruction call, Reference ref) {
        Register tmode = currentProgram.getProgramContext().getRegister("TMode");
        BigInteger here = currentProgram.getProgramContext().getValue(tmode, call.getAddress(), false);
        boolean thumbHere = here != null && here.signum() != 0;
        boolean switches = call.getMnemonicString().toLowerCase().startsWith("blx");
        return thumbHere != switches;
    }

    private void addGlobal(Map<Long, JsonObject> globals, long addr, long at, String access) {
        JsonObject g = globals.get(addr);
        if (g == null) {
            g = new JsonObject();
            g.addProperty("addr", String.format("0x%08x", addr));
            g.addProperty("region", region(addr));
            String name = names.get(addr);
            if (name != null) g.addProperty("name", name);
            g.add("access", new JsonArray());
            g.add("used_at", new JsonArray());
            globals.put(addr, g);
        }
        JsonArray acc = g.getAsJsonArray("access");
        boolean seen = false;
        for (var x : acc) seen |= x.getAsString().equals(access);
        if (!seen) acc.add(access);
        JsonArray used = g.getAsJsonArray("used_at");
        if (used.size() < 8) used.add(String.format("0x%08x", at));
    }

    private static String region(long a) {
        int top = (int) (a >>> 24);
        return switch (top) {
            case 0x02 -> "ewram";
            case 0x03 -> "iwram";
            case 0x04 -> "io";
            case 0x05 -> "palette";
            case 0x06 -> "vram";
            case 0x07 -> "oam";
            case 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D -> "rom";
            default -> "other";
        };
    }
}
