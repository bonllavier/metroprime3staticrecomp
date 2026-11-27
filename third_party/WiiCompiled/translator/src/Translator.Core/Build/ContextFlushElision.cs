using System;
using System.Collections.Generic;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;

namespace Translator.Core.Build;

/// <summary>How the shard emitter treats a flush whose local already equals its <c>CpuContext</c> slot.</summary>
public enum ContextFlushElisionMode
{
    /// <summary>Sources are emitted unchanged (and shard identities are unchanged).</summary>
    Off,
    /// <summary>The redundant store is removed.</summary>
    Elide,
    /// <summary>
    /// The redundant store becomes <c>MKW_CTX_FLUSH_CHECK</c>: it compares the slot with the
    /// local, logs a mismatch once per function and register, and stores anyway. A playable
    /// build that proves the analysis in the real game.
    /// </summary>
    Verify,
}

/// <summary>Thread-safe totals for one emission.</summary>
public sealed class ContextFlushElisionCounters
{
    private long _functions;
    private long _analyzed;
    private long _flushes;
    private long _elided;
    private long _nonvolatileReloads;
    private long _reloadsElided;
    private readonly Dictionary<string, long> _skipReasons = new(StringComparer.Ordinal);

    public long Functions => Interlocked.Read(ref _functions);
    public long AnalyzedFunctions => Interlocked.Read(ref _analyzed);
    public long Flushes => Interlocked.Read(ref _flushes);
    public long Elided => Interlocked.Read(ref _elided);
    /// <summary>Reloads of r1, r2 and r13-r31 right after a direct translated call (candidates of the reload pass).</summary>
    public long NonvolatileReloads => Interlocked.Read(ref _nonvolatileReloads);
    /// <summary>Of those, the reloads removed (or checked, in verify mode).</summary>
    public long ReloadsElided => Interlocked.Read(ref _reloadsElided);
    /// <summary>Call targets after which every pair is forgotten (see <see cref="ContextFlushElision.FindUntrustedCallTargets"/>).</summary>
    public int UntrustedCallTargets { get; set; }

    public IReadOnlyDictionary<string, long> SkipReasons
    {
        get { lock (_skipReasons) return new Dictionary<string, long>(_skipReasons, StringComparer.Ordinal); }
    }

    internal void AddFunction() => Interlocked.Increment(ref _functions);
    internal void AddAnalyzed(long flushes, long elided)
    {
        Interlocked.Increment(ref _analyzed);
        Interlocked.Add(ref _flushes, flushes);
        Interlocked.Add(ref _elided, elided);
    }

    internal void AddReloads(long candidates, long elided)
    {
        Interlocked.Add(ref _nonvolatileReloads, candidates);
        Interlocked.Add(ref _reloadsElided, elided);
    }

    internal void AddSkip(string reason)
    {
        lock (_skipReasons) _skipReasons[reason] = _skipReasons.GetValueOrDefault(reason) + 1;
    }
}

/// <summary>
/// Removes stores of a resident register back into <c>CpuContext</c> when the slot already holds
/// the local's value (it was loaded from it, reloaded from it, or flushed to it, and neither the
/// local nor the slot changed since on any path). The C++ compiler cannot do this itself because
/// <c>ctx</c> escapes into every callee.
///
/// Soundness rests on the generator's own residency invariant: between call boundaries a slot only
/// changes where this text writes it, and across a call it only changes for registers the generator
/// reloads afterwards (a reload makes the pair equal again). Interrupts run on an isolated register
/// file (<c>GuestInterruptCallbackContext</c>) and do not touch the interrupted <c>ctx</c>.
///
/// The analysis is a forward must-analysis over the emitted statements, conservative wherever the
/// text is not one of the recognised shapes: a local passed to any callee not known to take its
/// arguments by value counts as written, <c>ctx</c> handed to an unknown callee forgets every pair,
/// and a function with any unrecognised construct is left untouched.
/// </summary>
public static partial class ContextFlushElision
{
    private const int MaxLocals = 128;

    /// <summary>
    /// Callees that take every argument by value (or by const reference) and do not write guest
    /// registers in <c>ctx</c>; a resident local passed to them is only read. Checked against the
    /// runtime declarations (overloads included) by <c>ContextFlushElisionTests</c>' header scan.
    /// </summary>
    internal static readonly HashSet<string> ByValueCallees = new(StringComparer.Ordinal)
    {
        "static_cast", "reinterpret_cast", "std::fabs",
        "PpcFmulsInline", "PpcForceSingleValueInline", "PpcRotl32Inline", "PpcBitCastToFloatInline",
        "PPC_PsMaddInline", "PPC_PsToScalarInline", "PPC_PsFromScalarInline", "PPC_PsMulInline",
        "PPC_PsMerge00Inline", "PPC_PsMerge01Inline", "PPC_PsMerge10Inline", "PPC_PsMerge11Inline",
        "PPC_PsAddInline", "PPC_PsSubInline", "PPC_PsMuls0Inline", "PPC_PsMuls1Inline",
        "PPC_Fsel", "PPC_Fctiwz", "PPC_FprLowWordInline", "PPC_Frsqrte",
        "PPC_PsqStStackInline", "PPC_PsqLStackInline", "PPC_PsqLGqrInline", "PPC_PsqStGqrInline",
        "PPC_PsqLResolvedInline", "PPC_PsqStResolvedInline",
        "PPC_PsqLKnownResolvedInline", "PPC_PsqStKnownResolvedInline",
        "InvokeIndirectCpu", "InvokeIndirectJump", "PpcCrLogicalResident", "PpcGetPairedFprInline",
    };

    /// <summary>
    /// Callees whose first parameter is a non-const reference (the destination) and whose other
    /// parameters are taken by value: <c>SetCRResident(uint32_t&amp; cr, uint32_t xer, ...)</c>,
    /// <c>SetCRFloatResident(uint32_t&amp; cr, ...)</c>, <c>PpcSetPairedFprInline(PPC_FPR&amp;, double)</c>.
    /// </summary>
    internal static readonly HashSet<string> FirstArgumentWrittenCallees = new(StringComparer.Ordinal)
    {
        "SetCRResident", "SetCRFloatResident", "PpcSetPairedFprInline",
    };

    /// <summary>
    /// Callees that receive <c>ctx</c> without changing guest registers outside the reloads the
    /// generator emits after them: translated calls (their contract drives the reload set) and the
    /// quantized load/store helpers (they read GQRs only).
    /// </summary>
    internal static readonly HashSet<string> ContextPreservingCallees = new(StringComparer.Ordinal)
    {
        "InvokeDirectCpu", "MKW_STATIC_TRANSLATED_CALL", "InvokeIndirectCpu", "InvokeIndirectJump",
        "PPC_PsqStStackInline", "PPC_PsqLStackInline", "PPC_PsqLGqrInline", "PPC_PsqStGqrInline",
        "PPC_PsqLResolvedInline", "PPC_PsqStResolvedInline",
        "PPC_PsqLKnownResolvedInline", "PPC_PsqStKnownResolvedInline", "PPC_PsToScalarInline",
    };

    private const string HeaderPrefix = "extern \"C\" void ";
    private const string HeaderSuffix = "(CpuContext* MKW_RESTRICT ctx)";

    /// <summary>
    /// Call targets after which no pair may be trusted: functions whose source writes register slots
    /// itself (hand-written switch repairs, in-place argument rounding: their effect is not in the
    /// contract the generator computed from the original PowerPC), native-winner overrides, and every
    /// function that reaches one of them through direct calls. Found on 19/11/2026: the repair of
    /// 0x8042E428 calls 0x8042EF94, which leaves its f0 in the slot, while the callers of 0x80429434
    /// neither flush nor reload f0.
    /// </summary>
    /// <remarks>
    /// With <paramref name="includeIndirectCallers"/> false the functions that only reach an indirect
    /// call are left out: the set of targets that may break the PowerPC EABI itself (r1, r2 and r13-r31
    /// preserved), used by the nonvolatile reload pass.
    /// </remarks>
    public static HashSet<uint> FindUntrustedCallTargets(
        IEnumerable<(uint Address, string Source, IEnumerable<uint> DirectCalls)> functions,
        IEnumerable<uint> nativeWinners, bool includeIndirectCallers = true)
    {
        var seeds = new HashSet<uint>(nativeWinners);
        var callers = new Dictionary<uint, HashSet<uint>>();
        foreach (var (address, source, directCalls) in functions)
        {
            var targets = new HashSet<uint>(directCalls);
            foreach (Match m in CallTargetRegex().Matches(source))
                targets.Add(Convert.ToUInt32(m.Groups["target"].Value, 16));
            foreach (var target in targets)
            {
                if (!callers.TryGetValue(target, out var set)) callers[target] = set = new HashSet<uint>();
                set.Add(address);
            }
            // An indirect call or jump runs code whose effect the contract computed from the PowerPC
            // does not describe (found on 21/11/2026: 0x80317008 leaves an indirect callee's r4 in its
            // slot while the contract callers use says it does not write r4).
            if (includeIndirectCallers && (source.Contains("InvokeIndirectCpu(", StringComparison.Ordinal) ||
                source.Contains("InvokeIndirectJump(", StringComparison.Ordinal) ||
                source.Contains("PRIME_RSO_TryInvoke(", StringComparison.Ordinal)))
            {
                seeds.Add(address);
            }
            // Start-up code that installs the stack and the small-data bases (0x8000648C sets r1, r2 and
            // r13 to constants and zeroes r14-r31; found by the reload verify build on 26/11/2026) does
            // not keep the EABI by design. Ordinary code never loads a constant into r1, r2 or r13.
            if (!includeIndirectCallers && AbiBaseConstantRegex().IsMatch(source))
            {
                seeds.Add(address);
                continue;
            }
            foreach (Match m in SlotStoreRegex().Matches(source))
            {
                var slot = m.Groups["slot"].Value;
                var rhs = m.Groups["rhs"].Value.Trim();
                // An exact flush (ctx->gpr[N] = rN) is the generator's own; anything else is not.
                if (m.Groups["member"].Success || SlotLocalName(slot) != rhs)
                {
                    seeds.Add(address);
                    break;
                }
            }
        }

        var untrusted = new HashSet<uint>(seeds);
        var queue = new Queue<uint>(seeds);
        while (queue.Count != 0)
        {
            var address = queue.Dequeue();
            if (!callers.TryGetValue(address, out var set)) continue;
            foreach (var caller in set)
                if (untrusted.Add(caller)) queue.Enqueue(caller);
        }
        return untrusted;
    }

    public static string Apply(
        string source, ContextFlushElisionMode mode, ContextFlushElisionCounters? counters = null,
        IReadOnlySet<uint>? untrustedCallTargets = null,
        ContextFlushElisionMode reloadMode = ContextFlushElisionMode.Off,
        IReadOnlySet<uint>? abiUntrustedCallTargets = null)
    {
        if (mode == ContextFlushElisionMode.Off && reloadMode == ContextFlushElisionMode.Off) return source;
        var newline = source.Contains("\r\n", StringComparison.Ordinal) ? "\r\n" : "\n";
        var lines = source.Split('\n');
        // Analysis sees the text without '\r'; output keeps every untouched line byte for byte.
        var text = new string[lines.Length];
        for (var i = 0; i < lines.Length; i++) text[i] = lines[i].TrimEnd('\r');

        var replace = new Dictionary<int, string?>();
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
            var name = text[i][HeaderPrefix.Length..^HeaderSuffix.Length];
            AnalyzeFunction(text, i + 2, end, name, mode, replace, counters, untrustedCallTargets, reloadMode,
                abiUntrustedCallTargets);
            i = end;
        }

        if (replace.Count == 0) return source;
        var builder = new StringBuilder(source.Length);
        for (var i = 0; i < lines.Length; i++)
        {
            string? line = lines[i];
            if (replace.TryGetValue(i, out var replacement))
            {
                if (replacement is null) continue;
                line = replacement + (lines[i].EndsWith('\r') ? "\r" : string.Empty);
            }
            builder.Append(line);
            if (i + 1 < lines.Length) builder.Append('\n');
        }
        _ = newline;
        return builder.ToString();
    }

    /// <summary>Line of the brace that closes the body opened at <paramref name="open"/>, or -1.</summary>
    private static int FunctionEnd(string[] text, int open)
    {
        var depth = 0;
        for (var i = open; i < text.Length; i++)
        {
            var s = text[i].Trim();
            if (s.StartsWith("//", StringComparison.Ordinal)) continue;
            if (s.StartsWith('}')) depth--;
            if (depth == 0 && i > open) return i;
            if (s.EndsWith('{')) depth++;
            if (depth == 0) return -1;
        }
        return -1;
    }

    private sealed class Function
    {
        public required string[] Text;
        public required int Start;
        public required int End;
        public readonly Dictionary<string, int> Locals = new(StringComparer.Ordinal);
    }

    private static void AnalyzeFunction(
        string[] text, int bodyStart, int bodyEnd, string functionName, ContextFlushElisionMode mode,
        Dictionary<int, string?> replace, ContextFlushElisionCounters? counters,
        IReadOnlySet<uint>? untrustedCallTargets, ContextFlushElisionMode reloadMode,
        IReadOnlySet<uint>? abiUntrustedCallTargets)
    {
        string? Skip(string reason)
        {
            counters?.AddSkip(reason);
            return null;
        }

        var function = new Function { Text = text, Start = bodyStart, End = bodyEnd };
        var entryEnd = -1;
        for (var i = bodyStart; i < bodyEnd; i++)
        {
            var m = EntryLoadRegex().Match(text[i]);
            if (m.Success)
            {
                var local = m.Groups["local"].Value;
                if (!SlotMatchesLocal(m.Groups["slot"].Value, local) || function.Locals.ContainsKey(local))
                {
                    Skip("entry-load-shape");
                    return;
                }
                function.Locals[local] = function.Locals.Count;
                entryEnd = i + 1;
                continue;
            }
            if (entryEnd >= 0) break;
        }

        if (entryEnd < 0 || function.Locals.Count == 0)
        {
            Skip("no-resident-locals");
            return;
        }
        if (function.Locals.Count > MaxLocals)
        {
            Skip("too-many-locals");
            return;
        }

        // Declarations before the entry loads must not mention a resident local.
        for (var i = bodyStart; i < entryEnd; i++)
        {
            if (EntryLoadRegex().IsMatch(text[i])) continue;
            if (MentionedLocals(text[i], function.Locals).Count != 0)
            {
                Skip("local-before-entry");
                return;
            }
        }

        // Shape checks and the statement graph.
        var labels = new Dictionary<string, int>(StringComparer.Ordinal);
        var match = new Dictionary<int, int>();
        var openers = new Stack<int>();
        var switchOf = new Dictionary<int, int>();
        var switchStack = new Stack<int>();
        for (var i = entryEnd; i < bodyEnd; i++)
        {
            var s = text[i].Trim();
            if (s.Length == 0 || s.StartsWith("//", StringComparison.Ordinal)) continue;
            if (s.StartsWith('#')) { Skip("preprocessor"); return; }
            if (s.Contains("} else if", StringComparison.Ordinal)) { Skip("else-if"); return; }
            if (LambdaRegex().IsMatch(s)) { Skip("lambda"); return; }
            if (s.Contains("&ctx", StringComparison.Ordinal) || s.Contains("&(ctx", StringComparison.Ordinal))
            {
                Skip("ctx-address");
                return;
            }
            if (!(s.EndsWith(';') || s.EndsWith('{') || s.EndsWith('}') || s.EndsWith(':')))
            {
                Skip("multi-line-statement");
                return;
            }
            if (s.StartsWith("while", StringComparison.Ordinal) || s.StartsWith("for", StringComparison.Ordinal) ||
                s.StartsWith("do ", StringComparison.Ordinal) || s == "do" || s.StartsWith("continue", StringComparison.Ordinal))
            {
                Skip("loop");
                return;
            }
            if (s.EndsWith(':') && !s.StartsWith("case ", StringComparison.Ordinal) && s != "default:")
            {
                var label = s[..^1];
                if (!LabelRegex().IsMatch(label)) { Skip("label-shape"); return; }
                labels[label] = i;
            }
            if (s.StartsWith('}'))
            {
                if (openers.Count == 0) { Skip("brace-balance"); return; }
                var opener = openers.Pop();
                match[opener] = i;
                if (switchStack.Count != 0 && switchStack.Peek() == opener) switchStack.Pop();
            }
            if (switchStack.Count != 0) switchOf[i] = switchStack.Peek();
            if (s.EndsWith('{'))
            {
                openers.Push(i);
                if (s.StartsWith("switch", StringComparison.Ordinal)) switchStack.Push(i);
            }
        }
        if (openers.Count != 0) { Skip("brace-balance"); return; }

        var successors = new List<int>[bodyEnd];
        for (var i = entryEnd; i < bodyEnd; i++)
        {
            var list = new List<int>(2);
            successors[i] = list;
            var s = text[i].Trim();
            var gotoMatch = GotoRegex().Match(s);
            if (s.StartsWith("return", StringComparison.Ordinal))
            {
                continue;
            }
            if (gotoMatch.Success)
            {
                if (!labels.TryGetValue(gotoMatch.Groups["label"].Value, out var target))
                {
                    Skip("unknown-label");
                    return;
                }
                list.Add(target);
                var unconditional = s.StartsWith("goto", StringComparison.Ordinal) ||
                                    s.StartsWith("case ", StringComparison.Ordinal) ||
                                    s.StartsWith("default:", StringComparison.Ordinal);
                if (!unconditional) list.Add(i + 1);
                if (!unconditional && !s.StartsWith("if", StringComparison.Ordinal))
                {
                    Skip("goto-shape");
                    return;
                }
                continue;
            }
            if (s == "break;")
            {
                if (!switchOf.TryGetValue(i, out var owner)) { Skip("break-outside-switch"); return; }
                list.Add(match[owner]);
                continue;
            }
            if (s.StartsWith("if", StringComparison.Ordinal) && s.EndsWith('{'))
            {
                var close = match[i];
                list.Add(i + 1);
                list.Add(text[close].Trim().StartsWith("} else", StringComparison.Ordinal) ? close + 1 : close);
                continue;
            }
            if (s.StartsWith("} else", StringComparison.Ordinal))
            {
                if (!s.EndsWith('{')) { Skip("else-shape"); return; }
                list.Add(match[i]);
                continue;
            }
            if (s.StartsWith("switch", StringComparison.Ordinal))
            {
                var close = match[i];
                list.Add(close);
                for (var k = i + 1; k < close; k++)
                {
                    var t = text[k].TrimStart();
                    if (t.StartsWith("case ", StringComparison.Ordinal) || t.StartsWith("default:", StringComparison.Ordinal))
                        list.Add(k);
                }
                continue;
            }
            if (s.StartsWith("if", StringComparison.Ordinal) || s.StartsWith("else", StringComparison.Ordinal))
            {
                Skip("if-shape");
                return;
            }
            if (i + 1 < bodyEnd) list.Add(i + 1);
        }

        // Forward must-analysis: bit set = local equals its ctx slot. Unreached nodes stay at top.
        var all = function.Locals.Count == 128 ? UInt128.MaxValue : (UInt128.One << function.Locals.Count) - UInt128.One;
        var predecessors = new List<int>[bodyEnd];
        for (var i = entryEnd; i < bodyEnd; i++)
        {
            foreach (var s in successors[i])
            {
                if (s < entryEnd || s >= bodyEnd) continue;
                (predecessors[s] ??= new List<int>()).Add(i);
            }
        }

        var outState = new UInt128[bodyEnd];
        var inState = new UInt128[bodyEnd];
        for (var i = entryEnd; i < bodyEnd; i++) { outState[i] = all; inState[i] = all; }
        var transfer = new Transfer?[bodyEnd];
        var nonvolatile = UInt128.Zero;
        foreach (var (local, index) in function.Locals)
            if (IsNonvolatileGpr(local)) nonvolatile |= UInt128.One << index;
        for (var i = entryEnd; i < bodyEnd; i++)
            transfer[i] = BuildTransfer(text[i], function.Locals, all, untrustedCallTargets, abiUntrustedCallTargets, nonvolatile);

        var work = new Stack<int>();
        var queued = new bool[bodyEnd];
        for (var i = bodyEnd - 1; i >= entryEnd; i--) { work.Push(i); queued[i] = true; }
        while (work.Count != 0)
        {
            var i = work.Pop();
            queued[i] = false;
            var value = all;
            if (i != entryEnd && predecessors[i] is { } preds)
            {
                foreach (var p in preds) value &= outState[p];
            }
            inState[i] = value;
            var t = transfer[i]!;
            var next = (value & t.Keep) | t.Set;
            if (next == outState[i]) continue;
            outState[i] = next;
            foreach (var s in successors[i])
            {
                if (s < entryEnd || s >= bodyEnd || queued[s]) continue;
                queued[s] = true;
                work.Push(s);
            }
        }

        long flushes = 0, elided = 0;
        for (var i = entryEnd; i < bodyEnd; i++)
        {
            var t = transfer[i]!;
            if (mode == ContextFlushElisionMode.Off) break;
            if (t.FlushLocal is not { } local) continue;
            flushes++;
            var bit = UInt128.One << function.Locals[local];
            if ((inState[i] & bit) == UInt128.Zero) continue;
            elided++;
            if (mode == ContextFlushElisionMode.Elide)
            {
                replace[i] = null;
            }
            else
            {
                var indent = text[i][..(text[i].Length - text[i].TrimStart().Length)];
                replace[i] = $"{indent}MKW_CTX_FLUSH_CHECK({t.FlushSlot}, {local}, \"{functionName}\", \"{local}\");";
            }
        }
        counters?.AddAnalyzed(flushes, elided);
        if (reloadMode != ContextFlushElisionMode.Off)
            ElideNonvolatileReloads(text, entryEnd, bodyEnd, functionName, reloadMode, function.Locals, inState, replace, counters);
    }

    /// <summary>
    /// Reloads of the registers the PowerPC EABI makes the callee preserve (r1, r2, r13-r31) that the
    /// generator emits after a direct translated call because the callee's possible-write mask
    /// includes them (it saves and restores them itself; the mask is a union, not the net effect).
    /// When the local equals its slot before the call (<paramref name="inState"/>, carried across the
    /// call by the same analysis), an ABI-conforming callee leaves the slot as it found it, so the
    /// reload reads back the local's own value. Untrusted targets
    /// (<see cref="FindUntrustedCallTargets"/>) already clear every pair; indirect calls and the
    /// register save/restore helpers (<see cref="IsRegisterHelperTarget"/>) are excluded. FPRs are left
    /// alone: the paired-single halves of f14-f31 are not covered by <c>_savefpr</c>.
    /// </summary>
    private static void ElideNonvolatileReloads(
        string[] text, int entryEnd, int bodyEnd, string functionName, ContextFlushElisionMode mode,
        Dictionary<string, int> locals, UInt128[] inState, Dictionary<int, string?> replace,
        ContextFlushElisionCounters? counters)
    {
        long candidates = 0, elided = 0;
        for (var i = entryEnd; i < bodyEnd; i++)
        {
            var reload = ReloadRegex().Match(text[i]);
            if (!reload.Success) continue;
            var local = reload.Groups["local"].Value;
            if (!IsNonvolatileGpr(local) || !SlotMatchesLocal(reload.Groups["slot"].Value, local) ||
                !locals.TryGetValue(local, out var index) || replace.ContainsKey(i))
                continue;
            // The statement before the run of reloads this one belongs to must be one direct call.
            var k = i - 1;
            while (k >= entryEnd && ReloadRegex().IsMatch(text[k])) k--;
            if (k < entryEnd || !IsSingleDirectCall(text[k], out var target) || IsRegisterHelperTarget(target)) continue;
            candidates++;
            if ((inState[i] & (UInt128.One << index)) == UInt128.Zero) continue;
            elided++;
            if (mode == ContextFlushElisionMode.Elide)
            {
                replace[i] = null;
            }
            else
            {
                var indent = text[i][..(text[i].Length - text[i].TrimStart().Length)];
                replace[i] = $"{indent}MKW_CTX_RELOAD_CHECK({local}, ctx->{reload.Groups["slot"].Value}, \"{functionName}\", \"reload {local}\");";
            }
        }
        counters?.AddReloads(candidates, elided);
    }

    private static bool IsNonvolatileGpr(string local) =>
        local.Length >= 2 && local[0] == 'r' && int.TryParse(local.AsSpan(1), out var n) &&
        (n is 1 or 2 || n is >= 13 and <= 31);

    /// <summary><c>__save_fpr</c>, <c>__restore_fpr</c>, <c>__save_gpr</c> and <c>__restore_gpr</c> of RM3E01 and their entry labels.</summary>
    private static bool IsRegisterHelperTarget(uint target) => target is >= 0x803059E4u and < 0x80305B14u;

    private static bool IsSingleDirectCall(string line, out uint target)
    {
        target = 0;
        var s = line.Trim();
        if (s.Contains("InvokeIndirect", StringComparison.Ordinal) || s.Contains("RSO", StringComparison.Ordinal)) return false;
        var m = DirectCallStatementRegex().Match(s);
        if (!m.Success) return false;
        target = Convert.ToUInt32(m.Groups["target"].Value, 16);
        return true;
    }

    private sealed record Transfer(UInt128 Keep, UInt128 Set, string? FlushLocal, string? FlushSlot);

    private static Transfer BuildTransfer(
        string line, Dictionary<string, int> locals, UInt128 all, IReadOnlySet<uint>? untrustedCallTargets,
        IReadOnlySet<uint>? abiUntrustedCallTargets, UInt128 nonvolatile)
    {
        var flush = FlushRegex().Match(line);
        if (flush.Success && SlotMatchesLocal(flush.Groups["slot"].Value, flush.Groups["local"].Value) &&
            locals.TryGetValue(flush.Groups["local"].Value, out var flushIndex))
        {
            var bit = UInt128.One << flushIndex;
            return new Transfer(all, bit, flush.Groups["local"].Value, "ctx->" + flush.Groups["slot"].Value);
        }

        var reload = ReloadRegex().Match(line);
        if (reload.Success && SlotMatchesLocal(reload.Groups["slot"].Value, reload.Groups["local"].Value) &&
            locals.TryGetValue(reload.Groups["local"].Value, out var reloadIndex))
        {
            return new Transfer(all, UInt128.One << reloadIndex, null, null);
        }

        var keep = all;
        // Any other store into a register slot breaks that pair.
        foreach (Match m in ContextSlotWriteRegex().Matches(line))
        {
            var local = SlotLocalName(m.Groups["slot"].Value);
            if (local is not null && locals.TryGetValue(local, out var index)) keep &= ~(UInt128.One << index);
        }
        var writtenLocals = WrittenLocals(line, locals, out var contextEscapes);
        foreach (var written in writtenLocals)
        {
            keep &= ~(UInt128.One << locals[written]);
        }
        if (contextEscapes) keep = UInt128.Zero;
        if (untrustedCallTargets is { Count: > 0 } || abiUntrustedCallTargets is { Count: > 0 })
        {
            untrustedCallTargets ??= new HashSet<uint>();
            foreach (Match m in CallTargetRegex().Matches(line))
            {
                var target = Convert.ToUInt32(m.Groups["target"].Value, 16);
                if (abiUntrustedCallTargets is null)
                {
                    if (untrustedCallTargets.Contains(target)) keep = UInt128.Zero;
                }
                else if (abiUntrustedCallTargets.Contains(target))
                {
                    // Breaks the EABI itself (or reaches code that does): no pair survives. The ABI set
                    // also has seeds of its own, such as the start-up code that installs r1/r2/r13.
                    keep = UInt128.Zero;
                }
                else if (untrustedCallTargets.Contains(target))
                {
                    // Untrusted only because it reaches an indirect call: it still keeps the EABI, so
                    // the pairs of r1, r2 and r13-r31 survive it (ElideNonvolatileReloads).
                    keep &= nonvolatile;
                }
            }
        }
        return new Transfer(keep, UInt128.Zero, null, null);
    }

    private static HashSet<string> WrittenLocals(string line, Dictionary<string, int> locals, out bool contextEscapes)
    {
        var written = new HashSet<string>(StringComparer.Ordinal);
        contextEscapes = false;
        foreach (Match m in AssignmentRegex().Matches(line)) written.Add(m.Groups["local"].Value);
        foreach (Match m in IncDecRegex().Matches(line))
            written.Add(m.Groups["pre"].Success ? m.Groups["pre"].Value : m.Groups["post"].Value);
        foreach (Match m in AddressOfRegex().Matches(line)) written.Add(m.Groups["local"].Value);

        // Call arguments. Spans of trusted callees are masked so a nested unknown call is still seen.
        var contextHandled = new bool[line.Length];
        foreach (Match m in CallRegex().Matches(line))
        {
            var callee = m.Groups["callee"].Value;
            if (callee is "if" or "switch" or "return" or "sizeof" or "while" or "for") continue;
            var open = m.Index + m.Length - 1;
            var close = MatchingParen(line, open);
            if (close < 0) { contextEscapes = true; written.UnionWith(locals.Keys); break; }
            var args = line.Substring(open + 1, close - open - 1);
            if (FirstArgumentWrittenCallees.Contains(callee))
            {
                foreach (var local in MentionedLocals(FirstTopLevelArgument(args), locals)) written.Add(local);
            }
            else if (!ByValueCallees.Contains(callee) && !callee.StartsWith("MemoryInline::", StringComparison.Ordinal))
            {
                foreach (var local in MentionedLocals(args, locals)) written.Add(local);
            }
            if (ContextPreservingCallees.Contains(callee) || callee.StartsWith("MemoryInline::", StringComparison.Ordinal))
            {
                foreach (Match c in BareContextRegex().Matches(args))
                    contextHandled[open + 1 + c.Index] = true;
            }
        }
        // ctx anywhere else (an unknown callee, an alias) may change any slot.
        foreach (Match c in BareContextRegex().Matches(line))
        {
            if (!contextHandled[c.Index]) { contextEscapes = true; break; }
        }
        written.IntersectWith(locals.Keys);
        return written;
    }

    private static string FirstTopLevelArgument(string args)
    {
        var depth = 0;
        for (var i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case '(' or '<' or '[': depth++; break;
                case ')' or '>' or ']': depth--; break;
                case ',' when depth == 0: return args[..i];
            }
        }
        return args;
    }

    private static int MatchingParen(string line, int open)
    {
        var depth = 0;
        for (var i = open; i < line.Length; i++)
        {
            if (line[i] == '(') depth++;
            else if (line[i] == ')' && --depth == 0) return i;
        }
        return -1;
    }

    private static HashSet<string> MentionedLocals(string text, Dictionary<string, int> locals)
    {
        var found = new HashSet<string>(StringComparer.Ordinal);
        foreach (Match m in LocalTokenRegex().Matches(text))
        {
            if (locals.ContainsKey(m.Value)) found.Add(m.Value);
        }
        return found;
    }

    private static bool SlotMatchesLocal(string slot, string local) => SlotLocalName(slot) == local;

    private static string? SlotLocalName(string slot)
    {
        if (slot.StartsWith("gpr[", StringComparison.Ordinal)) return "r" + slot[4..^1];
        if (slot.StartsWith("fpr[", StringComparison.Ordinal)) return "f" + slot[4..^1];
        return slot is "cr" or "ctr" or "xer" ? slot : null;
    }

    private const string Local = @"(?:r(?:[12]?\d|3[01])|f(?:[12]?\d|3[01])|cr|ctr|xer)";
    private const string Slot = @"(?:gpr\[\d{1,2}\]|fpr\[\d{1,2}\]|cr|ctr|xer)";

    [GeneratedRegex(@"^    (?:uint32_t|PPC_FPR) (?<local>" + Local + @") = ctx->(?<slot>" + Slot + @");$")]
    private static partial Regex EntryLoadRegex();

    [GeneratedRegex(@"^\s*ctx->(?<slot>" + Slot + @") = (?<local>" + Local + @");$")]
    private static partial Regex FlushRegex();

    [GeneratedRegex(@"^\s*(?<local>" + Local + @") = ctx->(?<slot>" + Slot + @");$")]
    private static partial Regex ReloadRegex();

    [GeneratedRegex(@"ctx->(?<slot>" + Slot + @")(?:\.\w+)*\s*(?:[-+*/%|&^]|<<|>>)?=(?!=)")]
    private static partial Regex ContextSlotWriteRegex();

    [GeneratedRegex(@"(?<![\w.>])(?<local>" + Local + @")(?![\w\[])(?:\.\w+)*\s*(?:[-+*/%|&^]|<<|>>)?=(?!=)")]
    private static partial Regex AssignmentRegex();

    [GeneratedRegex(@"(?:\+\+|--)\s*(?<pre>" + Local + @")(?![\w\[])|(?<![\w.>])(?<post>" + Local + @")(?![\w\[])(?:\.\w+)*\s*(?:\+\+|--)")]
    private static partial Regex IncDecRegex();

    [GeneratedRegex(@"&\s*\(?\s*(?<local>" + Local + @")(?![\w\[])")]
    private static partial Regex AddressOfRegex();

    [GeneratedRegex(@"(?<![\w.>])" + Local + @"(?![\w\[])")]
    private static partial Regex LocalTokenRegex();

    [GeneratedRegex(@"(?<callee>[A-Za-z_][\w:]*)\s*(?:<[^<>()]*(?:<[^<>()]*>[^<>()]*)*>)?\s*\(")]
    private static partial Regex CallRegex();

    [GeneratedRegex(@"(?<![\w.>])ctx(?!\w)(?!\s*->)")]
    private static partial Regex BareContextRegex();

    [GeneratedRegex(@"(?:InvokeDirectCpu<0x|MKW_STATIC_TRANSLATED_CALL\(0x)(?<target>[0-9A-Fa-f]{8})u")]
    private static partial Regex CallTargetRegex();

    [GeneratedRegex(@"ctx->(?<slot>" + Slot + @")(?<member>\.\w+)?\s*=(?!=)\s*(?<rhs>[^;]*);")]
    private static partial Regex SlotStoreRegex();

    [GeneratedRegex(@"^(?:MKW_STATIC_TRANSLATED_CALL\(0x(?<target>[0-9A-Fa-f]{8})u, func_[0-9A-Fa-f]{8}, ctx\)|InvokeDirectCpu<0x(?<target>[0-9A-Fa-f]{8})u>\(ctx\));$")]
    private static partial Regex DirectCallStatementRegex();

    [GeneratedRegex(@"^\s*r(?:1|2|13) = (?:-?\d+|0x[0-9A-Fa-f]+)u?;\s*$", RegexOptions.Multiline)]
    private static partial Regex AbiBaseConstantRegex();

    [GeneratedRegex(@"goto\s+(?<label>\w+)\s*;")]
    private static partial Regex GotoRegex();

    [GeneratedRegex(@"^\w+$")]
    private static partial Regex LabelRegex();

    [GeneratedRegex(@"\[[^\]]*\]\s*\(|=\s*\[")]
    private static partial Regex LambdaRegex();
}
