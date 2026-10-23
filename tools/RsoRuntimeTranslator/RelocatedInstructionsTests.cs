using Translator.Core.Disassembly;
using Translator.Core.Ir;
using Translator.Core.Lifting;

public static class RelocatedInstructionsTests
{
    public static void Run()
    {
        const uint pc = 0x8069720C;
        foreach (short original in new short[] { 24, -24 })
        foreach (short relocated in new short[] { 9, -9 })
        {
            // stb r7, d(r3): both signed halves and a non-byte source value.
            var word = (38u << 26) | (7u << 21) | (3u << 16) | (ushort)original;
            var instruction = new PpcInstruction(pc, word, "stb",
                [new PpcRegisterOperand("r7", 7), new PpcDisplacementOperand(original, "r3", 3)],
                [], false, false, false);
            var lifted = new PpcLifter().Lift([instruction]).Single();
            var transformed = new RelocatedInstructions(12, new Dictionary<uint, RsoSite> {
                [pc] = new(pc, 4, false)
            }).Transform(lifted);
            var registers = new Dictionary<string, long> { ["r3"] = 0x1000, ["r7"] = 0x123456AB };
            var memory = new Dictionary<long, byte> { [0x1000 + relocated - 1] = 0x55, [0x1000 + relocated + 1] = 0x66 };
            long Value(IrValue value) => value.Constant ?? registers[value.RegisterName!];
            foreach (var op in transformed.Ir)
            {
                switch (op)
                {
                case IrCall call when call.Target == "PPC_RsoReadImmediate":
                    if (call.Arguments.Select(Value).SequenceEqual(new long[] { 12, pc, 1 }) == false)
                        throw new Exception("stb immediate must come from the live module's signed low half");
                    registers[call.Destination] = relocated;
                    break;
                case IrBinary binary when binary.Op == "add":
                    registers[binary.Destination] = Value(binary.Left) + Value(binary.Right);
                    break;
                case IrStore store when store.SizeBytes == 1:
                    memory[registers[store.Address.Base] + store.Address.Offset] = unchecked((byte)Value(store.Source));
                    break;
                default: throw new Exception("Unexpected relocated byte-store lowering");
                }
            }
            if (memory.Count != 3 || memory[0x1000 + relocated] != 0xAB ||
                memory[0x1000 + relocated - 1] != 0x55 || memory[0x1000 + relocated + 1] != 0x66 ||
                registers["r3"] != 0x1000 || registers["r7"] != 0x123456AB)
                throw new Exception("Relocated stb changed the wrong address, width, neighbors or source/base register");
        }
        Console.WriteLine("PASS relocated stb: four signed-offset combinations, byte width, neighbor and register preservation");
    }
}
