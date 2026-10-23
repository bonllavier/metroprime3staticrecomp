using System.Buffers.Binary;
using Translator.Core.Analysis;
using Translator.Core.Ir;
using Translator.Core.Loading;
using Translator.Core.Translation;

namespace Translator.Tests;

public sealed class RuntimeInstructionTransformTests
{
    private static FunctionTranslator Translator()
    {
        var memory = new byte[12];
        BinaryPrimitives.WriteUInt32BigEndian(memory.AsSpan(0), 0x3C601234); // lis r3,0x1234
        BinaryPrimitives.WriteUInt32BigEndian(memory.AsSpan(4), 0x38635678); // addi r3,r3,0x5678
        BinaryPrimitives.WriteUInt32BigEndian(memory.AsSpan(8), 0x4E800020);
        var range = new AddressRange(0x80001000, 0x8000100C);
        return new FunctionTranslator(new ProgramImage(memory, range, range, new AddressRange(0, 0), "fixture", 0x80001000));
    }

    [Fact]
    public void IdentityTransformPreservesSourceAndVisitsEveryInstructionOnce()
    {
        var visited = new List<uint>();
        var baseline = Translator().Translate(0x80001000, new TranslationOptions(EnableLeafInlining: false));
        var transformed = Translator().Translate(0x80001000, new TranslationOptions(
            EnableLeafInlining: false, InstructionTransform: instruction => {
                visited.Add(instruction.Origin.Address);
                return instruction;
            }));
        Assert.Equal(new uint[] { 0x80001000, 0x80001004, 0x80001008 }, visited);
        Assert.Equal(baseline.CxxCode, transformed.CxxCode);
    }

    [Fact]
    public void RelocatedReadEntersBeforeSsaAndDoesNotFoldToTheNominalAddress()
    {
        var translated = Translator().Translate(0x80001000, new TranslationOptions(
            EnableLeafInlining: false, InstructionTransform: instruction => instruction.Origin.Address == 0x80001000
                ? instruction with { Ir = new IrInstruction[] { new IrCall("r3", "PPC_RsoReadImmediate",
                    new[] { IrValue.Imm(1), IrValue.Imm(0x80001000), IrValue.Imm(3) }) } }
                : instruction));
        Assert.Contains("PPC_RsoReadImmediate(1,", translated.CxxCode);
        translated.Ssa.ValidateUseDef();
        Assert.Equal(0, translated.Metrics.UnsupportedInstructionCount);
    }

    [Fact]
    public void TransformsCannotSilentlyBypassInlinedCalleeSites()
    {
        Assert.Throws<InvalidOperationException>(() => Translator().Translate(0x80001000,
            new TranslationOptions(InstructionTransform: instruction => instruction)));
    }

    [Fact]
    public void RelocationReadersHaveNoImplicitArchitecturalWrites()
    {
        foreach (var name in new[] { "PPC_RsoReadImmediate", "PPC_RsoRuntimeAddress" })
        {
            var effect = GuestHelperEffectCatalog.Analyze(new IrCall("temporary", name, Array.Empty<IrValue>()));
            Assert.Equal(0u, effect.GprWriteMask);
            Assert.Equal(0u, effect.FprWriteMask);
            Assert.False(effect.WritesLr);
            Assert.False(effect.WritesCtr);
        }
    }

    [Fact]
    public void ModuleImportPublishesTheCompleteContextAndMaySwitchThreads()
    {
        var effect = GuestHelperEffectCatalog.Analyze(new IrCall(string.Empty, "PPC_RsoImportCall", Array.Empty<IrValue>()));
        Assert.Equal(uint.MaxValue, effect.GprWriteMask);
        Assert.Equal(uint.MaxValue, effect.FprWriteMask);
        Assert.True(effect.WritesLr);
        Assert.True(effect.WritesCtr);
        Assert.NotEqual(0, (int)effect.BoundaryFlags);
    }
}
