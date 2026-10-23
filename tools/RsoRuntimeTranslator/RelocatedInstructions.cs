using Translator.Core.Ir;
using Translator.Core.Lifting;

public sealed record RsoSite(uint Pc, int Type, bool External);

public sealed class RelocatedInstructions(uint moduleId, IReadOnlyDictionary<uint, RsoSite> sites)
{
    public LiftedInstruction Transform(LiftedInstruction lifted)
    {
        var pc = lifted.Origin.Address;
        var output = new List<IrInstruction>();
        // LR is a guest address even when the instruction has no relocation.
        foreach (var instruction in lifted.Ir)
        {
            if (instruction is IrAssign { Destination: "lr", Value.Constant: not null } lr)
                output.Add(new IrCall("lr", "PPC_RsoRuntimeAddress", [IrValue.Imm(moduleId), lr.Value]));
            else output.Add(instruction);
        }
        if (!sites.TryGetValue(pc, out var site)) return lifted with { Ir = output };
        if (site.Type == 10)
        {
            if (!site.External) return lifted with { Ir = output };
            // Execute the SDK's real resolved import thunk, including its
            // linkage-area write and CTR update. It is not a direct DOL call.
            if (lifted.Origin.Word >> 26 != 18) throw new InvalidOperationException("REL24 is not a branch");
            output = output.Where(op => op is not IrCall { Target: not "PPC_RsoRuntimeAddress" } && op is not IrJump && op is not IrReturn).ToList();
            output.Add(new IrCall("", "PPC_RsoImportCall", [IrValue.Imm(moduleId), IrValue.Imm(pc)]));
            if ((lifted.Origin.Word & 1) == 0) output.Add(new IrReturn(null));
            return lifted with { Ir = output };
        }
        if (site.Type is not (4 or 6)) throw new InvalidOperationException($"Unsupported code relocation {site.Type} at {pc:X8}");
        var opcode = lifted.Origin.Word >> 26;
        if (opcode is not (14 or 15 or 32 or 33 or 34 or 36 or 38 or 48 or 50 or 52))
            throw new InvalidOperationException($"Unreviewed relocated D-form opcode {opcode} at {pc:X8}");
        // GunTurret's static initialization stores one byte through an ADDR16_LO
        // address. Only its EA displacement is relocated; preserve the source
        // register and one-byte store (never promote it to a word write).
        if (opcode == 38 && (output.Count != 1 || output[0] is not IrStore { SizeBytes: 1 }))
            throw new InvalidOperationException($"Unreviewed relocated stb lowering at {pc:X8}");
        // These values are defined and consumed within one instruction. Follow
        // the lifter's block-local convention so joins do not synthesize phis
        // for temporaries that have no incoming value on the other edge.
        var temporary = $"addr_rso_imm_{pc:X8}_loc";
        var prelude = new IrCall(temporary, "PPC_RsoReadImmediate",
            [IrValue.Imm(moduleId), IrValue.Imm(pc), IrValue.Imm(opcode == 15 ? 3 : 1)]);
        var replacement = IrValue.Register(temporary);
        var rewritten = new List<IrInstruction> { prelude };
        var changed = 0;
        var signedImmediate = (short)(lifted.Origin.Word & 0xFFFF);
        var expected = opcode == 15 ? signedImmediate << 16 : signedImmediate;
        var zeroBase = ((lifted.Origin.Word >> 16) & 31) == 0;
        if (opcode == 33) { // lwzu: relocate the base update, then load at offset zero.
            var baseName = $"r{(lifted.Origin.Word >> 16) & 31}";
            var destination = $"r{(lifted.Origin.Word >> 21) & 31}";
            if (zeroBase || baseName == destination || output.Count != 2 ||
                output[0] is not IrBinary update || update.Destination != baseName || update.Op != "add" ||
                update.Left.RegisterName != baseName || update.Right.Constant != signedImmediate ||
                output[1] is not IrLoad load || load.Destination != destination || load.SizeBytes != 4 ||
                load.Address.Base != baseName || load.Address.Offset != 0)
                throw new InvalidOperationException($"Unreviewed relocated lwzu lowering at {pc:X8}");
            rewritten.Add(update with { Right = replacement });
            rewritten.Add(load);
            return lifted with { Ir = rewritten };
        }
        foreach (var op in output)
        {
            if (op is IrAssign assign && assign.Value.Constant == expected && (opcode is 14 or 15 || zeroBase))
            {
                rewritten.Add(assign with { Value = replacement });
                ++changed;
            }
            else if (op is IrBinary binary && binary.Right.Constant == expected && opcode is 14 or 15)
            {
                rewritten.Add(binary with { Right = replacement });
                ++changed;
            }
            else if (!zeroBase && op is IrLoad load && load.Address.Offset == signedImmediate)
            {
                var address = $"addr_rso_ea_{pc:X8}_loc";
                rewritten.Add(new IrBinary(address, IrValue.Register(load.Address.Base), replacement, "add"));
                rewritten.Add(load with { Address = new IrAddress(address, 0) });
                ++changed;
            }
            else if (!zeroBase && op is IrStore store && store.Address.Offset == signedImmediate)
            {
                var address = $"addr_rso_ea_{pc:X8}_loc";
                rewritten.Add(new IrBinary(address, IrValue.Register(store.Address.Base), replacement, "add"));
                rewritten.Add(store with { Address = new IrAddress(address, 0) });
                ++changed;
            }
            else rewritten.Add(op);
        }
        if (changed != 1) throw new InvalidOperationException($"Relocation at {pc:X8} rewrote {changed} operands, expected exactly one");
        return lifted with { Ir = rewritten };
    }
}
