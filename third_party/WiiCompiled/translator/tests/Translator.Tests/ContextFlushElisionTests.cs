using Translator.Core.Build;
using Xunit;

namespace Translator.Tests;

public sealed class ContextFlushElisionTests
{
    private static string Function(params string[] body)
    {
        var lines = new List<string>
        {
            "extern \"C\" void func_80001000(CpuContext* MKW_RESTRICT ctx)",
            "{",
            "    uint32_t r0_rot_0 = 0;",
            "",
            "    uint32_t r3 = ctx->gpr[3];",
            "    uint32_t r4 = ctx->gpr[4];",
            "    uint32_t r31 = ctx->gpr[31];",
            "    PPC_FPR f1 = ctx->fpr[1];",
            "    uint32_t cr = ctx->cr;",
            "    uint32_t xer = ctx->xer;",
            "",
            "    goto loc_80001000;",
            "",
        };
        lines.AddRange(body);
        lines.Add("}");
        return string.Join("\n", lines) + "\n";
    }

    private static string[] Apply(string source, ContextFlushElisionMode mode = ContextFlushElisionMode.Elide) =>
        ContextFlushElision.Apply(source, mode).Split('\n');

    [Fact]
    public void UnchangedLocalIsNotStoredButAWrittenOneIs()
    {
        var output = Apply(Function(
            "loc_80001000:",
            "{",
            "    r3 = (r3 + 4);",
            "    ctx->gpr[3] = r3;",
            "    ctx->gpr[31] = r31;",
            "    InvokeDirectCpu<0x80002000u>(ctx);",
            "    r3 = ctx->gpr[3];",
            "    return;",
            "}"));

        Assert.Contains("    ctx->gpr[3] = r3;", output);
        Assert.DoesNotContain("    ctx->gpr[31] = r31;", output);
    }

    [Fact]
    public void ReloadMakesTheNextFlushRedundantUntilTheLocalChanges()
    {
        var output = Apply(Function(
            "loc_80001000:",
            "{",
            "    r4 = 1;",
            "    ctx->gpr[4] = r4;",
            "    InvokeDirectCpu<0x80002000u>(ctx);",
            "    r4 = ctx->gpr[4];",
            "    ctx->gpr[4] = r4;",
            "    InvokeDirectCpu<0x80002000u>(ctx);",
            "    r4 = ctx->gpr[4];",
            "    r4 = (r4 + 1);",
            "    ctx->gpr[4] = r4;",
            "    return;",
            "}"));

        Assert.Equal(2, output.Count(static line => line == "    ctx->gpr[4] = r4;"));
    }

    [Fact]
    public void WriteOnOneBranchKeepsTheFlushAfterTheJoin()
    {
        var output = Apply(Function(
            "loc_80001000:",
            "{",
            "    if (((cr & 0x20000000u) == 0)) {",
            "        goto loc_80001010;",
            "    }",
            "}",
            "",
            "loc_80001008:",
            "{",
            "    r31 = (r31 + 1);",
            "}",
            "",
            "loc_80001010:",
            "{",
            "    ctx->gpr[31] = r31;",
            "    ctx->gpr[3] = r3;",
            "    InvokeDirectCpu<0x80002000u>(ctx);",
            "    return;",
            "}"));

        Assert.Contains("    ctx->gpr[31] = r31;", output);
        Assert.DoesNotContain("    ctx->gpr[3] = r3;", output);
    }

    [Fact]
    public void FlushInsideAnIfWithoutElseDoesNotCleanTheSkippedPath()
    {
        var output = Apply(Function(
            "loc_80001000:",
            "{",
            "    r3 = 7;",
            "    if (r4 != 0) {",
            "        ctx->gpr[3] = r3;",
            "        InvokeDirectCpu<0x80002000u>(ctx);",
            "    }",
            "    ctx->gpr[3] = r3;",
            "    return;",
            "}"));

        Assert.Equal(2, output.Count(static line => line.Trim() == "ctx->gpr[3] = r3;"));
    }

    [Fact]
    public void ElseBranchIsAnalysedAsItsOwnPath()
    {
        var output = Apply(Function(
            "loc_80001000:",
            "{",
            "    if (r4 != 0) {",
            "        r31 = 1;",
            "    } else {",
            "        r3 = 2;",
            "    }",
            "    ctx->gpr[31] = r31;",
            "    ctx->gpr[3] = r3;",
            "    ctx->gpr[4] = r4;",
            "    return;",
            "}"));

        Assert.Contains("    ctx->gpr[31] = r31;", output);
        Assert.Contains("    ctx->gpr[3] = r3;", output);
        Assert.DoesNotContain("    ctx->gpr[4] = r4;", output);
    }

    [Fact]
    public void CompareHelperWritesOnlyItsDestination()
    {
        var output = Apply(Function(
            "loc_80001000:",
            "{",
            "    SetCRResident(cr, xer, 0, static_cast<int32_t>(r3), static_cast<int32_t>(0));",
            "    ctx->cr = cr;",
            "    ctx->xer = xer;",
            "    ctx->gpr[3] = r3;",
            "    return;",
            "}"));

        Assert.Contains("    ctx->cr = cr;", output);
        Assert.DoesNotContain("    ctx->xer = xer;", output);
        Assert.DoesNotContain("    ctx->gpr[3] = r3;", output);
    }

    [Fact]
    public void UnknownCalleeArgumentCountsAsWritten()
    {
        var output = Apply(Function(
            "loc_80001000:",
            "{",
            "    SomeHelperByReference(r31, static_cast<uint32_t>(r3));",
            "    ctx->gpr[31] = r31;",
            "    ctx->gpr[3] = r3;",
            "    return;",
            "}"));

        Assert.Contains("    ctx->gpr[31] = r31;", output);
        Assert.Contains("    ctx->gpr[3] = r3;", output);
    }

    [Fact]
    public void ContextHandedToAnUnknownCalleeForgetsEveryPair()
    {
        var output = Apply(Function(
            "loc_80001000:",
            "{",
            "    SomeContextHelper(ctx);",
            "    ctx->gpr[31] = r31;",
            "    return;",
            "}"));

        Assert.Contains("    ctx->gpr[31] = r31;", output);
    }

    [Fact]
    public void DirectSlotWriteBreaksThePair()
    {
        var output = Apply(Function(
            "loc_80001000:",
            "{",
            "    ctx->gpr[31] = 0;",
            "    ctx->gpr[31] = r31;",
            "    return;",
            "}"));

        Assert.Contains("    ctx->gpr[31] = r31;", output);
    }

    [Fact]
    public void FprMemberWriteCountsAsWritten()
    {
        var output = Apply(Function(
            "loc_80001000:",
            "{",
            "    f1.d = 1.0;",
            "    ctx->fpr[1] = f1;",
            "    PpcSetPairedFprInline(f1, 2.0);",
            "    ctx->fpr[1] = f1;",
            "    return;",
            "}"));

        Assert.Equal(2, output.Count(static line => line == "    ctx->fpr[1] = f1;"));
    }

    [Fact]
    public void SwitchCasesAreAllReachable()
    {
        var output = Apply(Function(
            "loc_80001000:",
            "{",
            "    switch (static_cast<uint32_t>(r4)) {",
            "    case 0x80001010u:",
            "        goto loc_80001010;",
            "        break;",
            "    case 0x80001020u:",
            "        goto loc_80001020;",
            "        break;",
            "    }",
            "}",
            "",
            "loc_80001010:",
            "{",
            "    r31 = 5;",
            "}",
            "",
            "loc_80001020:",
            "{",
            "    ctx->gpr[31] = r31;",
            "    return;",
            "}"));

        Assert.Contains("    ctx->gpr[31] = r31;", output);
    }

    [Fact]
    public void LabelBlocksAtColumnZeroStayInsideTheFunction()
    {
        var source = Function(
            "loc_80001000:",
            "{",
            "    ctx->gpr[31] = r31;",
            "    return;",
            "}") + Function(
            "loc_80001000:",
            "{",
            "    r31 = 3;",
            "    ctx->gpr[31] = r31;",
            "    return;",
            "}");

        var output = Apply(source);

        Assert.Single(output, static line => line == "    ctx->gpr[31] = r31;");
    }

    [Fact]
    public void VerifyModeKeepsAStoreBehindTheCheck()
    {
        var output = Apply(Function(
            "loc_80001000:",
            "{",
            "    ctx->gpr[31] = r31;",
            "    ctx->fpr[1] = f1;",
            "    return;",
            "}"), ContextFlushElisionMode.Verify);

        Assert.Contains("    MKW_CTX_FLUSH_CHECK(ctx->gpr[31], r31, \"func_80001000\", \"r31\");", output);
        Assert.Contains("    MKW_CTX_FLUSH_CHECK(ctx->fpr[1], f1, \"func_80001000\", \"f1\");", output);
    }

    [Fact]
    public void OffAndCrLfAreByteExact()
    {
        var source = Function(
            "loc_80001000:",
            "{",
            "    ctx->gpr[31] = r31;",
            "    r3 = 1;",
            "    return;",
            "}").Replace("\n", "\r\n");

        Assert.Same(source, ContextFlushElision.Apply(source, ContextFlushElisionMode.Off));
        var elided = ContextFlushElision.Apply(source, ContextFlushElisionMode.Elide);
        Assert.Equal(source.Replace("    ctx->gpr[31] = r31;\r\n", string.Empty), elided);
    }

    [Fact]
    public void UnrecognisedShapesLeaveTheFunctionUntouched()
    {
        var source = Function(
            "loc_80001000:",
            "{",
            "    auto call = [ctx](uint32_t slot) { return slot; };",
            "    ctx->gpr[31] = r31;",
            "    return;",
            "}");
        var counters = new ContextFlushElisionCounters();

        Assert.Equal(source, ContextFlushElision.Apply(source, ContextFlushElisionMode.Elide, counters));
        Assert.Equal(1, counters.SkipReasons["lambda"]);
    }

    [Fact]
    public void CallsReachingASlotWriterForgetEveryPair()
    {
        const string writer = "extern \"C\" void func_80003000(CpuContext* MKW_RESTRICT ctx)\n{\n    ctx->fpr[0].d = 1.0;\n}\n";
        const string middle = "extern \"C\" void func_80002000(CpuContext* MKW_RESTRICT ctx)\n{\n    InvokeDirectCpu<0x80003000u>(ctx);\n}\n";
        var untrusted = ContextFlushElision.FindUntrustedCallTargets(
            new (uint, string, IEnumerable<uint>)[]
            {
                (0x80003000u, writer, Array.Empty<uint>()),
                (0x80002000u, middle, Array.Empty<uint>()),
                (0x80004000u, "extern \"C\" void func_80004000(CpuContext* MKW_RESTRICT ctx)\n{\n    ctx->gpr[3] = r3;\n}\n", Array.Empty<uint>()),
            },
            Array.Empty<uint>());

        Assert.Equal(new HashSet<uint> { 0x80003000u, 0x80002000u }, untrusted);

        var source = Function(
            "loc_80001000:",
            "{",
            "    MKW_STATIC_TRANSLATED_CALL(0x80002000u, func_80002000, ctx);",
            "    ctx->fpr[1] = f1;",
            "    InvokeDirectCpu<0x80004000u>(ctx);",
            "    ctx->gpr[31] = r31;",
            "    return;",
            "}");
        var output = ContextFlushElision.Apply(source, ContextFlushElisionMode.Elide, null, untrusted).Split('\n');

        Assert.Contains("    ctx->fpr[1] = f1;", output);
        Assert.Contains("    ctx->gpr[31] = r31;", output);
        var trusted = ContextFlushElision.Apply(source, ContextFlushElisionMode.Elide).Split('\n');
        Assert.DoesNotContain("    ctx->fpr[1] = f1;", trusted);
    }

    private static string StackFunction(params string[] body)
    {
        var lines = new List<string>
        {
            "extern \"C\" void func_80001000(CpuContext* MKW_RESTRICT ctx)",
            "{",
            "    uint32_t r1 = ctx->gpr[1];",
            "    uint32_t r3 = ctx->gpr[3];",
            "    uint32_t r13 = ctx->gpr[13];",
            "",
            "    goto loc_80001000;",
            "",
            "loc_80001000:",
            "{",
        };
        lines.AddRange(body);
        lines.Add("    return;");
        lines.Add("}");
        lines.Add("}");
        return string.Join("\n", lines) + "\n";
    }

    [Fact]
    public void StackAndSdaAccessesLoseTheMmioCheck()
    {
        var source = StackFunction(
            "    r1 = (r1 + -32);",
            "    r3 = MemoryInline::FlatRead32((r1 + 8));",
            "    r3 = MemoryInline::FlatRead8(r1);",
            "    MemoryInline::FlatWrite32((r13 + -9464), r3);",
            "    r3 = MemoryInline::FlatRead32((r3 + 8));",
            "    ctx->gpr[1] = r1;",
            "    InvokeDirectCpu<0x80002000u>(ctx);",
            "    r1 = ctx->gpr[1];",
            "    r3 = MemoryInline::FlatRead32((r1 + 40000));");
        var counters = new ProvenRamAccessCounters();
        var output = ContextFlushElision.LowerProvenRamAccesses(source, counters).Split('\n');

        Assert.Contains("    r3 = MemoryInline::FlatReadRam32((r1 + 8));", output);
        Assert.Contains("    r3 = MemoryInline::FlatReadRam8(r1);", output);
        Assert.Contains("    MemoryInline::FlatWriteProven32((r13 + -9464), r3);", output);
        Assert.Contains("    r3 = MemoryInline::FlatRead32((r3 + 8));", output);
        Assert.Contains("    r3 = MemoryInline::FlatRead32((r1 + 40000));", output);
        Assert.Equal(2, counters.Reads(0));
        Assert.Equal(1, counters.Writes(2));
    }

    [Fact]
    public void BaseSetFromAnythingElseKeepsTheCheck()
    {
        foreach (var setter in new[]
                 {
                     "    r1 = MemoryInline::FlatRead32((r3 + 4));",
                     "    r1 = r3;",
                     "    ctx->gpr[1] = r3;",
                     "    SomeHelperByReference(r1);",
                 })
        {
            var source = StackFunction(setter, "    r3 = MemoryInline::FlatRead32((r1 + 8));");
            Assert.Equal(source, ContextFlushElision.LowerProvenRamAccesses(source));
        }
    }

    [Fact]
    public void IndirectCallersAreUntrusted()
    {
        const string indirect = "extern \"C\" void func_80005000(CpuContext* MKW_RESTRICT ctx)\n{\n    InvokeIndirectCpu(r12, ctx);\n}\n";
        const string caller = "extern \"C\" void func_80006000(CpuContext* MKW_RESTRICT ctx)\n{\n    InvokeDirectCpu<0x80005000u>(ctx);\n}\n";
        var untrusted = ContextFlushElision.FindUntrustedCallTargets(
            new (uint, string, IEnumerable<uint>)[]
            {
                (0x80005000u, indirect, Array.Empty<uint>()),
                (0x80006000u, caller, Array.Empty<uint>()),
            },
            Array.Empty<uint>());

        Assert.Contains(0x80005000u, untrusted);
        Assert.Contains(0x80006000u, untrusted);
    }

    private static string[] ApplyReloads(string source, ContextFlushElisionMode reloadMode = ContextFlushElisionMode.Elide,
        IReadOnlySet<uint>? untrusted = null) =>
        ContextFlushElision.Apply(source, ContextFlushElisionMode.Off, null, untrusted, reloadMode).Split('\n');

    [Fact]
    public void NonvolatileReloadAfterDirectCallIsRemovedVolatileOneKept()
    {
        var output = ApplyReloads(Function(
            "loc_80001000:",
            "{",
            "    MKW_STATIC_TRANSLATED_CALL(0x80002000u, func_80002000, ctx);",
            "    r3 = ctx->gpr[3];",
            "    r31 = ctx->gpr[31];",
            "    return;",
            "}"));

        Assert.Contains("    r3 = ctx->gpr[3];", output);
        Assert.DoesNotContain("    r31 = ctx->gpr[31];", output);
    }

    [Fact]
    public void NonvolatileReloadIsKeptWhenTheLocalDifferedFromItsSlot()
    {
        // r31 changed and was not flushed before the call: the slot holds the old value, so the
        // reload really changes the local (the contract said the callee does not read r31).
        var output = ApplyReloads(Function(
            "loc_80001000:",
            "{",
            "    r31 = (r31 + 4);",
            "    MKW_STATIC_TRANSLATED_CALL(0x80002000u, func_80002000, ctx);",
            "    r31 = ctx->gpr[31];",
            "    return;",
            "}"));

        Assert.Contains("    r31 = ctx->gpr[31];", output);
    }

    [Fact]
    public void NonvolatileReloadIsKeptAfterIndirectUntrustedAndHelperCalls()
    {
        var indirect = ApplyReloads(Function(
            "loc_80001000:",
            "{",
            "    InvokeIndirectCpu(r3, ctx);",
            "    r31 = ctx->gpr[31];",
            "    return;",
            "}"));
        var untrusted = ApplyReloads(Function(
            "loc_80001000:",
            "{",
            "    MKW_STATIC_TRANSLATED_CALL(0x80002000u, func_80002000, ctx);",
            "    r31 = ctx->gpr[31];",
            "    return;",
            "}"), untrusted: new HashSet<uint> { 0x80002000u });
        var helper = ApplyReloads(Function(
            "loc_80001000:",
            "{",
            "    InvokeDirectCpu<0x80305A60u>(ctx);",
            "    r31 = ctx->gpr[31];",
            "    return;",
            "}"));

        Assert.Contains("    r31 = ctx->gpr[31];", indirect);
        Assert.Contains("    r31 = ctx->gpr[31];", untrusted);
        Assert.Contains("    r31 = ctx->gpr[31];", helper);
    }

    [Fact]
    public void NonvolatileReloadAfterFlushIsRemovedAndVerifyChecksIt()
    {
        var source = Function(
            "loc_80001000:",
            "{",
            "    r31 = (r31 + 4);",
            "    ctx->gpr[31] = r31;",
            "    InvokeDirectCpu<0x80002000u>(ctx);",
            "    r31 = ctx->gpr[31];",
            "    return;",
            "}");
        var elided = ApplyReloads(source);
        var verified = ApplyReloads(source, ContextFlushElisionMode.Verify);

        Assert.DoesNotContain("    r31 = ctx->gpr[31];", elided);
        Assert.Contains("    ctx->gpr[31] = r31;", elided);
        Assert.Contains("    MKW_CTX_RELOAD_CHECK(r31, ctx->gpr[31], \"func_80001000\", \"reload r31\");", verified);
    }

    [Fact]
    public void IndirectOnlyUntrustedCalleeKeepsNonvolatilePairsOnly()
    {
        var source = Function(
            "loc_80001000:",
            "{",
            "    MKW_STATIC_TRANSLATED_CALL(0x80002000u, func_80002000, ctx);",
            "    r3 = ctx->gpr[3];",
            "    r31 = ctx->gpr[31];",
            "    ctx->gpr[4] = r4;",
            "    return;",
            "}");
        var untrusted = new HashSet<uint> { 0x80002000u };
        var output = ContextFlushElision.Apply(source, ContextFlushElisionMode.Elide, null, untrusted,
            ContextFlushElisionMode.Elide, abiUntrustedCallTargets: new HashSet<uint>()).Split('\n');
        var abiBreaking = ContextFlushElision.Apply(source, ContextFlushElisionMode.Elide, null, untrusted,
            ContextFlushElisionMode.Elide, abiUntrustedCallTargets: untrusted).Split('\n');

        Assert.DoesNotContain("    r31 = ctx->gpr[31];", output);
        Assert.Contains("    ctx->gpr[4] = r4;", output);  // a volatile pair does not survive the call
        Assert.Contains("    r31 = ctx->gpr[31];", abiBreaking);
    }

    [Fact]
    public void AbiUntrustedSetLeavesOutIndirectCallers()
    {
        const string indirect = "extern \"C\" void func_80005000(CpuContext* MKW_RESTRICT ctx)\n{\n    InvokeIndirectCpu(r12, ctx);\n}\n";
        var set = ContextFlushElision.FindUntrustedCallTargets(
            new (uint, string, IEnumerable<uint>)[] { (0x80005000u, indirect, Array.Empty<uint>()) },
            Array.Empty<uint>(), includeIndirectCallers: false);

        Assert.DoesNotContain(0x80005000u, set);
    }

    [Fact]
    public void StartupCodeThatInstallsTheStackIsAbiUntrusted()
    {
        const string init = "extern \"C\" void func_8000648C(CpuContext* MKW_RESTRICT ctx)\n{\n    r1 = 0x80690000u;\n    r1 = (r1 | 41896);\n}\n";
        const string start = "extern \"C\" void func_80006320(CpuContext* MKW_RESTRICT ctx)\n{\n    MKW_STATIC_TRANSLATED_CALL(0x8000648Cu, func_8000648C, ctx);\n}\n";
        var set = ContextFlushElision.FindUntrustedCallTargets(
            new (uint, string, IEnumerable<uint>)[]
            {
                (0x8000648Cu, init, Array.Empty<uint>()),
                (0x80006320u, start, Array.Empty<uint>()),
            },
            Array.Empty<uint>(), includeIndirectCallers: false);

        Assert.Contains(0x8000648Cu, set);
        Assert.Contains(0x80006320u, set);
    }

    [Fact]
    public void AbiUntrustedTargetOutsideTheGeneralSetClearsNonvolatilePairs()
    {
        var output = ContextFlushElision.Apply(Function(
            "loc_80001000:",
            "{",
            "    MKW_STATIC_TRANSLATED_CALL(0x8000648Cu, func_8000648C, ctx);",
            "    r31 = ctx->gpr[31];",
            "    return;",
            "}"), ContextFlushElisionMode.Elide, null, new HashSet<uint>(),
            ContextFlushElisionMode.Elide, abiUntrustedCallTargets: new HashSet<uint> { 0x8000648Cu }).Split('\n');

        Assert.Contains("    r31 = ctx->gpr[31];", output);
    }
}
