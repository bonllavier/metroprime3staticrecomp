using System.Buffers.Binary;
using Translator.Core.Disassembly;
using Translator.Core.Ir;
using Translator.Core.Lifting;
using Translator.Core.Loading;
using Translator.Core.Translation;
using Xunit;

public sealed class RelocationControlFlowTests
{
    [Fact]
    public void ImportCallExplicitlyExposesRestrictedContextToHostCompiler()
    {
        uint[] words = { 0x38600025, 0x48000011, 0x2C030000, 0x4E800020 };
        var bytes = new byte[words.Length * 4];
        for (var i=0;i<words.Length;++i) BinaryPrimitives.WriteUInt32BigEndian(bytes.AsSpan(i*4),words[i]);
        var range=new AddressRange(0x80001000,0x80001010);
        var translator=new FunctionTranslator(new ProgramImage(bytes,range,range,new AddressRange(0,0),"fixture",range.Start));
        var rewrite=new RelocatedInstructions(3,new Dictionary<uint,RsoSite> { [0x80001004]=new(0x80001004,10,true) });
        var result=translator.Translate(range.Start,new TranslationOptions(EnableLeafInlining:false,InstructionTransform:rewrite.Transform));
        result.Ssa.ValidateUseDef();
        Assert.Contains("PPC_RsoImportCall(ctx, 3,",result.CxxCode);
        Assert.Contains("r3 = ctx->gpr[3];",result.CxxCode);
        Assert.Equal(0,result.Metrics.UnsupportedInstructionCount);
    }
    private static uint Lwzu(int destination, int source, int immediate) =>
        (33u << 26) | ((uint)destination << 21) | ((uint)source << 16) | (unchecked((uint)immediate) & 0xFFFFu);

    [Theory]
    [InlineData(0)]
    [InlineData(-32768)]
    [InlineData(32767)]
    public void RelocatedUpdateLoadKeepsTheBaseWritebackAndZeroOffsetLoad(int immediate)
    {
        var origin = new PpcInstruction(0x80001000, Lwzu(7, 8, immediate), "lwzu",
            Array.Empty<PpcOperand>(), Array.Empty<uint>(), false, false, false);
        var lifted = new LiftedInstruction(origin, new IrInstruction[] {
            new IrBinary("r8", IrValue.Register("r8"), IrValue.Imm(immediate), "add"),
            new IrLoad("r7", new IrAddress("r8", 0), 4) });
        var rewrite = new RelocatedInstructions(3, new Dictionary<uint, RsoSite> { [origin.Address] = new(origin.Address, 4, false) });
        var ir = rewrite.Transform(lifted).Ir;
        Assert.Equal(3, ir.Count);
        var read = Assert.IsType<IrCall>(ir[0]);
        var update = Assert.IsType<IrBinary>(ir[1]);
        Assert.Equal("r8", update.Destination);
        Assert.Equal("r8", update.Left.RegisterName);
        Assert.Equal(read.Destination, update.Right.RegisterName);
        Assert.Equal(lifted.Ir[1], ir[2]);
    }

    [Fact]
    public void UpdateLoadPublishesTheModifiedBaseAfterFullTranslation()
    {
        var bytes = new byte[8];
        BinaryPrimitives.WriteUInt32BigEndian(bytes, Lwzu(7, 8, 0));
        BinaryPrimitives.WriteUInt32BigEndian(bytes.AsSpan(4), 0x4E800020);
        var range = new AddressRange(0x80001000, 0x80001008);
        var translator = new FunctionTranslator(new ProgramImage(bytes, range, range, new AddressRange(0, 0), "fixture", range.Start));
        var rewrite = new RelocatedInstructions(3, new Dictionary<uint, RsoSite> { [range.Start] = new(range.Start, 4, false) });
        var result = translator.Translate(range.Start, new TranslationOptions(EnableLeafInlining: false, InstructionTransform: rewrite.Transform));
        result.Ssa.ValidateUseDef();
        Assert.Contains("FlatRead32(r8)", result.CxxCode);
        Assert.Contains("ctx->gpr[8] = r8;", result.CxxCode);
    }

    [Theory]
    [InlineData(0)]
    [InlineData(7)]
    public void InvalidUpdateLoadRegisterCombinationsFailClosed(int baseRegister)
    {
        var origin = new PpcInstruction(0x80001000, Lwzu(7, baseRegister, 0), "lwzu",
            Array.Empty<PpcOperand>(), Array.Empty<uint>(), false, false, false);
        var rewrite = new RelocatedInstructions(3, new Dictionary<uint, RsoSite> { [origin.Address] = new(origin.Address, 4, false) });
        Assert.Throws<InvalidOperationException>(() => rewrite.Transform(new LiftedInstruction(origin, Array.Empty<IrInstruction>())));
    }
    private static LiftedInstruction Branch(bool linked) => new(
        new PpcInstruction(0x80001000, linked ? 0x48000101u : 0x48000100u,
            linked ? "bl" : "b", Array.Empty<PpcOperand>(), new uint[] { 0x80001100 }, false, linked, false),
        linked ? new IrInstruction[] { new IrAssign("lr", IrValue.Imm(0x80001004)),
            new IrCall("", "0x80001100", Array.Empty<IrValue>()) }
        : new IrInstruction[] { new IrCall("", "0x80001100", Array.Empty<IrValue>()), new IrReturn(null) });

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void ExternalBranchExecutesImportBeforeReturning(bool linked)
    {
        var transform = new RelocatedInstructions(2, new Dictionary<uint, RsoSite> {
            [0x80001000] = new(0x80001000, 10, true) });
        var result = transform.Transform(Branch(linked)).Ir.ToList();
        var importIndex = result.FindIndex(op => op is IrCall { Target: "PPC_RsoImportCall" });
        Assert.True(importIndex >= 0);
        Assert.DoesNotContain(result.Take(importIndex), op => op is IrReturn);
        Assert.Equal(!linked, result.Last() is IrReturn);
        Assert.Equal(linked, result.Any(op => op is IrCall { Destination: "lr", Target: "PPC_RsoRuntimeAddress" }));
    }

    [Fact]
    public void ConditionalStoreHasNoUndefinedTemporaryPhiOnTheSkippedEdge()
    {
        uint[] words = { 0x3C601234, 0x80835678, 0x2C040000, 0x41820008, 0x90835678, 0x4E800020 };
        var bytes = new byte[words.Length * 4];
        for (var i = 0; i < words.Length; ++i) BinaryPrimitives.WriteUInt32BigEndian(bytes.AsSpan(i * 4), words[i]);
        var range = new AddressRange(0x80001000, 0x80001018);
        var translator = new FunctionTranslator(new ProgramImage(bytes, range, range, new AddressRange(0, 0), "fixture", range.Start));
        var rewrite = new RelocatedInstructions(2, new Dictionary<uint, RsoSite> {
            [0x80001000] = new(0x80001000, 6, false), [0x80001004] = new(0x80001004, 4, false),
            [0x80001010] = new(0x80001010, 4, false) });
        var result = translator.Translate(0x80001000, new TranslationOptions(EnableLeafInlining: false,
            InstructionTransform: rewrite.Transform));
        result.Ssa.ValidateUseDef();
        Assert.Contains("PPC_RsoReadImmediate", result.CxxCode);
        Assert.Equal(0, result.Metrics.UnsupportedInstructionCount);
    }
}
