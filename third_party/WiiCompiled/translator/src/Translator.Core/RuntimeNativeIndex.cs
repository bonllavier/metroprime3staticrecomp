using System.Text.RegularExpressions;
using Translator.Core.Analysis;
using Translator.Core.CodeGen;

namespace Translator.Core;

// todo: the entire reading of cpp/h files using regex is a bit fragile, but it works for now. 
// however this does eventually need to be replaced with another solution.
public sealed record RuntimeNativeRegistration(
    uint Address,
    string Symbol,
    string SourceFile,
    bool IsTranslatedOverride,
    bool ExcludesBaseTranslation);

public sealed record RuntimeNativeAbiEntry(
    uint Address,
    string[] ArgumentRegisters,
    string[] ScalarFloatArgumentRegisters);

public sealed record RuntimeNativeEffectEntry(
    uint Address,
    GuestAbiContract Contract,
    bool IsPrecise);

/// <summary>
/// One process-local view of the runtime's native registrations and their guest
/// ABI contracts. C++ remains the only source of truth: the translator builds
/// this index once and shares it across all consumers. Only emit-native-index
/// persists it (<see cref="RuntimeNativeIndexFile"/>).
/// </summary>
public sealed record RuntimeNativeIndex(
    RuntimeNativeRegistration[] Registrations,
    RuntimeNativeAbiEntry[] VoidStubAbis,
    RuntimeNativeEffectEntry[] Effects)
{
    public RuntimeNativeGuestEffectSet ToGuestEffectSet()
    {
        var contracts = Effects.ToDictionary(static entry => entry.Address, static entry => entry.Contract);
        var precise = Effects.Where(static entry => entry.IsPrecise)
            .Select(static entry => entry.Address).ToHashSet();
        var conservative = Effects.Where(static entry => !entry.IsPrecise)
            .Select(static entry => entry.Address).ToHashSet();
        return new RuntimeNativeGuestEffectSet(contracts, precise, conservative);
    }
}

public static class RuntimeNativeIndexBuilder
{
    public static RuntimeNativeIndex Build(
        string nativeSourceDirectory, IReadOnlySet<string>? activeDefines = null)
    {
        var sourceRoot = Path.GetFullPath(nativeSourceDirectory);
        if (!Directory.Exists(sourceRoot))
            return new RuntimeNativeIndex([], [], []);

        var sources = NativeSourceParsing.ReadDirectory(sourceRoot);
        var effects = RuntimeNativeGuestEffectAnalyzer.AnalyzeSources(sources);
        var abis = RuntimeNativeFunctionAbiProvider.AnalyzeVoidStubAbis(sources);
        return new RuntimeNativeIndex(
            ScanRegistrations(sources, activeDefines).ToArray(),
            abis.OrderBy(static item => item.Key)
                .Select(static item => new RuntimeNativeAbiEntry(
                    item.Key,
                    item.Value.ArgumentRegisters.Order(StringComparer.OrdinalIgnoreCase).ToArray(),
                    item.Value.ScalarFloatArgumentRegisters.Order(StringComparer.OrdinalIgnoreCase).ToArray()))
                .ToArray(),
            effects.Contracts.OrderBy(static item => item.Key)
                .Select(item => new RuntimeNativeEffectEntry(
                    item.Key, item.Value, effects.PreciseContracts.Contains(item.Key)))
                .ToArray());
    }

    /// <summary>
    /// The index for <paramref name="nativeSourceDirectory"/>: the serialized index at
    /// <paramref name="indexPath"/> when it matches the sources (or the sources are absent),
    /// otherwise a fresh scan.
    /// </summary>
    public static RuntimeNativeIndex Resolve(
        string nativeSourceDirectory, string? indexPath, TextWriter? diagnostics = null,
        IReadOnlySet<string>? activeDefines = null)
    {
        var sourceRoot = Path.GetFullPath(nativeSourceDirectory);
        // A persisted index records every conditional branch. When the caller
        // selects a compile profile, rescan registrations with those defines so
        // the generated dispatcher agrees with the C++ preprocessor.
        if (activeDefines is { Count: > 0 })
        {
            diagnostics?.WriteLine("[translator] scanning native registrations for active profile defines");
            return Build(sourceRoot, activeDefines);
        }
        if (string.IsNullOrWhiteSpace(indexPath) || !File.Exists(indexPath))
            return Build(sourceRoot);

        var expectedDigest = Directory.Exists(sourceRoot)
            ? RuntimeNativeIndexFile.ComputeSourceDigest(sourceRoot)
            : null;
        if (RuntimeNativeIndexFile.TryLoad(indexPath, expectedDigest, out var index, out var reason))
            return index;
        if (expectedDigest is null)
            throw new InvalidOperationException(
                $"The native index {indexPath} is unusable ({reason}) and the native sources it describes are missing: {sourceRoot}");

        diagnostics?.WriteLine($"[translator] ignoring native index {indexPath} ({reason}); scanning {sourceRoot}");
        return Build(sourceRoot);
    }

    private static IEnumerable<RuntimeNativeRegistration> ScanRegistrations(
        IReadOnlyList<NativeSourceFile> sources, IReadOnlySet<string>? activeDefines)
    {
        var registrations = new List<RuntimeNativeRegistration>();
        foreach (var sourceFile in sources)
        {
            var source = activeDefines is { Count: > 0 }
                ? MaskInactiveProfileBranches(sourceFile.Content, activeDefines)
                : sourceFile.Content;
            foreach (Match match in GeneratedMarkers.NativeFunctionRegistrationPattern().Matches(source))
                Add(match, match.Groups["symbol"].Value, false, !match.Groups["as"].Success);
            foreach (Match match in GeneratedMarkers.TranslatedFunctionRegistrationPattern().Matches(source))
                Add(match, match.Groups["symbol"].Value, true, true);
            foreach (Match match in GeneratedMarkers.NativeOverridePattern().Matches(source))
                Add(match, match.Groups["symbol"].Value, false, true);
            foreach (Match match in GeneratedMarkers.FatalStubPattern().Matches(source))
                Add(match, $"GX_FATAL_STUB_{match.Groups["address"].Value}", false, true);

            void Add(Match match, string symbol, bool translated, bool excludesBase) =>
                registrations.Add(new RuntimeNativeRegistration(
                    ParseAddress(match.Groups["address"].Value), symbol, sourceFile.RelativePath,
                    translated, excludesBase));
        }

        return registrations
            .Distinct()
            .OrderBy(static registration => registration.Address)
            .ThenBy(static registration => registration.Symbol, StringComparer.Ordinal)
            .ThenBy(static registration => registration.SourceFile, StringComparer.Ordinal)
            .ThenBy(static registration => registration.IsTranslatedOverride)
            .ThenBy(static registration => registration.ExcludesBaseTranslation);
    }

    private readonly record struct ConditionalFrame(bool ParentActive, bool Condition, bool ElseSeen);

    private static string MaskInactiveProfileBranches(string source, IReadOnlySet<string> activeDefines)
    {
        var chars = source.ToCharArray();
        var frames = new List<ConditionalFrame>();
        var active = true;
        var offset = 0;
        while (offset < source.Length)
        {
            var lineEnd = source.IndexOf('\n', offset);
            if (lineEnd < 0) lineEnd = source.Length;
            var line = source.AsSpan(offset, lineEnd - offset);
            var trimmed = line.Trim();
            if (trimmed.StartsWith("#if ", StringComparison.Ordinal) ||
                trimmed.StartsWith("#ifdef ", StringComparison.Ordinal) ||
                trimmed.StartsWith("#ifndef ", StringComparison.Ordinal))
            {
                var condition = EvaluateProfileCondition(trimmed, activeDefines);
                frames.Add(new ConditionalFrame(active, condition, false));
                active = active && condition;
            }
            else if (trimmed.SequenceEqual("#else") && frames.Count != 0)
            {
                var frame = frames[^1];
                frames[^1] = frame with { ElseSeen = true };
                active = frame.ParentActive && !frame.Condition;
            }
            else if (trimmed.StartsWith("#elif ", StringComparison.Ordinal) && frames.Count != 0)
            {
                var frame = frames[^1];
                var condition = !frame.Condition && EvaluateProfileCondition(trimmed, activeDefines);
                frames[^1] = frame with { Condition = frame.Condition || condition };
                active = frame.ParentActive && condition;
            }
            else if (trimmed.SequenceEqual("#endif") && frames.Count != 0)
            {
                active = frames[^1].ParentActive;
                frames.RemoveAt(frames.Count - 1);
            }

            if (!active)
            {
                for (var index = offset; index < lineEnd; ++index)
                    if (chars[index] != '\r') chars[index] = ' ';
            }
            offset = lineEnd + 1;
        }
        return new string(chars);
    }

    private static bool EvaluateProfileCondition(ReadOnlySpan<char> directive, IReadOnlySet<string> activeDefines)
    {
        var text = directive.ToString();
        var match = Regex.Match(text,
            @"^#if\s+(?<neg>!)?\s*defined\s*\(\s*(?<name>[A-Za-z_][A-Za-z0-9_]*)\s*\)\s*$|^#ifdef\s+(?<ifdef>[A-Za-z_][A-Za-z0-9_]*)\s*$|^#ifndef\s+(?<ifndef>[A-Za-z_][A-Za-z0-9_]*)\s*$|^#elif\s+(?<elifneg>!)?\s*defined\s*\(\s*(?<elifname>[A-Za-z_][A-Za-z0-9_]*)\s*\)\s*$");
        if (!match.Success) return true; // Preserve unknown build guards conservatively.
        var name = match.Groups["name"].Success ? match.Groups["name"].Value
            : match.Groups["ifdef"].Success ? match.Groups["ifdef"].Value
            : match.Groups["ifndef"].Success ? match.Groups["ifndef"].Value
            : match.Groups["elifname"].Value;
        var defined = activeDefines.Contains(name);
        return match.Groups["neg"].Success || match.Groups["ifndef"].Success || match.Groups["elifneg"].Success
            ? !defined
            : defined;
    }

    private static uint ParseAddress(string value) => GuestTargetParser.ParseHexAddress(value);
}
