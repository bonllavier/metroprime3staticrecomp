using System;
using System.Collections.Generic;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;

namespace Translator.Core.Build;

/// <summary>Totals of <see cref="ContextFlushElision.LowerProvenRamAccesses"/> for one emission.</summary>
public sealed class ProvenRamAccessCounters
{
    private long _functions;
    private readonly long[] _eligible = new long[3];
    private readonly long[] _reads = new long[3];
    private readonly long[] _writes = new long[3];

    public long Functions => Interlocked.Read(ref _functions);
    public long Eligible(int baseIndex) => Interlocked.Read(ref _eligible[baseIndex]);
    public long Reads(int baseIndex) => Interlocked.Read(ref _reads[baseIndex]);
    public long Writes(int baseIndex) => Interlocked.Read(ref _writes[baseIndex]);

    internal void AddFunction() => Interlocked.Increment(ref _functions);
    internal void AddEligible(int baseIndex) => Interlocked.Increment(ref _eligible[baseIndex]);
    internal void AddRead(int baseIndex) => Interlocked.Increment(ref _reads[baseIndex]);
    internal void AddWrite(int baseIndex) => Interlocked.Increment(ref _writes[baseIndex]);
}

public static partial class ContextFlushElision
{
    /// <summary>Base registers whose `base + constant` accesses can be proven to stay out of MMIO.</summary>
    public static readonly string[] ProvenRamBases = { "r1", "r2", "r13" };

    /// <summary>
    /// Drops the MMIO mask/compare from guest accesses at `rB + constant` (|constant| &lt; 0x8000) for
    /// rB = r1 (stack pointer), r2 and r13 (small-data bases), in functions where rB is only ever
    /// loaded at entry or reloaded from its own <c>CpuContext</c> slot (r1 may also be adjusted by a
    /// constant: stwu/addi), that slot is never stored with anything but rB, and rB is never handed
    /// by reference to a helper. Those registers then hold the guest stack and the SDA bases, which
    /// are MEM1 addresses, so the access cannot hit 0xCC000000..0xCDFFFFFF in correct execution.
    /// The translator already emits every r1-relative store check-free (<c>FlatWriteRam*</c>); this
    /// extends the same proof to loads and to r2/r13. Reads become <c>FlatReadRam*</c>, writes
    /// <c>FlatWriteProven*</c> (runtime/include/guest_abi/memory_access.h); both keep the checked path
    /// on hosts whose page is larger than a Wii page.
    /// </summary>
    public static string LowerProvenRamAccesses(string source, ProvenRamAccessCounters? counters = null)
    {
        var lines = source.Split('\n');
        var text = new string[lines.Length];
        for (var i = 0; i < lines.Length; i++) text[i] = lines[i].TrimEnd('\r');
        var changed = false;
        for (var i = 0; i < text.Length; i++)
        {
            if (!text[i].StartsWith(HeaderPrefix, StringComparison.Ordinal) ||
                !text[i].EndsWith(HeaderSuffix, StringComparison.Ordinal) ||
                i + 1 >= text.Length || text[i + 1] != "{")
            {
                continue;
            }
            var end = FunctionEnd(text, i + 1);
            if (end < 0) break;
            counters?.AddFunction();
            if (!BodyShapeIsPlain(text, i + 2, end))
            {
                i = end;
                continue;
            }
            for (var b = 0; b < ProvenRamBases.Length; b++)
            {
                var reg = ProvenRamBases[b];
                if (!BaseIsStable(text, i + 2, end, reg)) continue;
                counters?.AddEligible(b);
                var access = ProvenAccessRegex(reg);
                for (var k = i + 2; k < end; k++)
                {
                    var rewritten = access.Replace(text[k], m =>
                    {
                        if (m.Groups["off"].Success &&
                            (!int.TryParse(m.Groups["off"].Value, out var off) || off <= -0x8000 || off >= 0x8000))
                        {
                            return m.Value;
                        }
                        var read = m.Groups["op"].Value == "Read";
                        if (read) counters?.AddRead(b); else counters?.AddWrite(b);
                        var helper = read ? "FlatReadRam" : "FlatWriteProven";
                        return $"MemoryInline::{helper}{m.Groups["kind"].Value}({m.Groups["addr"].Value}{m.Groups["tail"].Value}";
                    });
                    if (!ReferenceEquals(rewritten, text[k]) && rewritten != text[k])
                    {
                        lines[k] = rewritten + (lines[k].EndsWith('\r') ? "\r" : string.Empty);
                        text[k] = rewritten;
                        changed = true;
                    }
                }
            }
            i = end;
        }
        return changed ? string.Join("\n", lines) : source;
    }

    /// <summary>Same exclusions as the flush analysis: no lambdas, ctx aliases, preprocessor or split statements.</summary>
    private static bool BodyShapeIsPlain(string[] text, int start, int end)
    {
        for (var i = start; i < end; i++)
        {
            var s = text[i].Trim();
            if (s.Length == 0 || s.StartsWith("//", StringComparison.Ordinal)) continue;
            if (s.StartsWith('#') || LambdaRegex().IsMatch(s) ||
                s.Contains("&ctx", StringComparison.Ordinal) || s.Contains("&(ctx", StringComparison.Ordinal))
            {
                return false;
            }
            if (!(s.EndsWith(';') || s.EndsWith('{') || s.EndsWith('}') || s.EndsWith(':'))) return false;
        }
        return true;
    }

    private static bool BaseIsStable(string[] text, int start, int end, string reg)
    {
        var index = reg[1..];
        var entry = $"    uint32_t {reg} = ctx->gpr[{index}];";
        var reload = $"{reg} = ctx->gpr[{index}];";
        var flush = $"ctx->gpr[{index}] = {reg};";
        var adjust = reg == "r1" ? StackAdjustRegex() : null;
        var locals = new Dictionary<string, int>(StringComparer.Ordinal) { [reg] = 0 };
        for (var i = start; i < end; i++)
        {
            var line = text[i];
            if (line == entry) continue;
            var s = line.Trim();
            if (s == reload || s == flush) continue;
            if (adjust is not null && adjust.IsMatch(s)) continue;
            // Any other store into its slot (a hand-written frame switch, a context load).
            foreach (Match m in ContextSlotWriteRegex().Matches(line))
            {
                if (m.Groups["slot"].Value == $"gpr[{index}]") return false;
            }
            if (WrittenLocals(line, locals, out var contextEscapes).Count != 0) return false;
            _ = contextEscapes; // a call that may change the slot is followed by the reload above
        }
        return true;
    }

    private static readonly Dictionary<string, Regex> ProvenAccessRegexes = new(StringComparer.Ordinal);
    private static readonly object ProvenAccessRegexLock = new();

    private static Regex ProvenAccessRegex(string reg)
    {
        lock (ProvenAccessRegexLock)
        {
            if (!ProvenAccessRegexes.TryGetValue(reg, out var regex))
            {
                regex = new Regex(
                    @"MemoryInline::Flat(?<op>Read|Write)(?<kind>8|16|32|Float32|Float64)\(" +
                    @"(?<addr>\(" + reg + @" \+ (?<off>-?\d+)\)|" + reg + @")(?<tail>[,)])",
                    RegexOptions.CultureInvariant | RegexOptions.Compiled);
                ProvenAccessRegexes[reg] = regex;
            }
            return regex;
        }
    }

    [GeneratedRegex(@"^r1 = \(r1 \+ -?\d+\);$")]
    private static partial Regex StackAdjustRegex();
}
