using System.Security.Cryptography;
using System.Text.Json;
using Translator.Core.Analysis;
using Translator.Core.Loading;
using Translator.Core.Parsing.Dol;
using Translator.Core.Translation;

public static class DolFunctionRepair
{
    public static void Run(string root, uint entry, string destination)
    {
        var dol = Path.Combine(root, "recomp/input/RM3E01_00/main.dol");
        if (Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(dol))).ToLowerInvariant() !=
            "6b550f221602074747a2e61b0aa064203fd493f6865dfb3b1a912682065e6104")
            throw new InvalidOperationException("Unexpected DOL revision");
        var map = FunctionMap.Load(Path.Combine(root, "recomp/config/RM3E01_00/functions.txt"));
        GuestSaveRestoreThunks.Current = GuestSaveRestoreThunks.FromFunctionMap(map);
        var image = new ProgramImageBuilder().Build(DolFile.Load(dol), ramSize: 0x700000);
        var result = new FunctionTranslator(image).Translate(entry, new TranslationOptions(
            KnownFunctionEntryPoints: map.Addresses.ToHashSet(), EmitStateFreeLeafVariant: false));
        result.Ssa.ValidateUseDef();
        if (result.Metrics.UnsupportedInstructionCount != 0 || result.Emission?.Registration is null)
            throw new InvalidOperationException("Repair has unsupported instructions or missing build registration");
        Directory.CreateDirectory(destination);
        var filename = $"func_{entry:X8}.cpp";
        var source = Path.Combine(destination, filename);
        if (!File.Exists(source) || File.ReadAllText(source) != result.CxxCode) File.WriteAllText(source, result.CxxCode);
        var facts = result.Emission;
        var metadata = new {
            relativePath = filename, size = new FileInfo(source).Length,
            sha256 = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(source))).ToLowerInvariant(),
            entryPoint = entry, localLabelAddresses = facts.EmittedLocalLabelAddresses,
            build = new { symbol = facts.Registration.Symbol, preservesNonvolatileFprs = facts.Registration.PreservesNonvolatileFprs,
                nonvolatileFprWriteMask = facts.Registration.NonvolatileFprWriteMask,
                directCallDependencies = facts.EmittedDirectCallTargets, guestAbiComment = facts.GuestAbiMarker,
                estimatedIrInstructions = result.Metrics.IrInstructionCount }, interiorToOtherTranslation = false
        };
        File.WriteAllText(Path.Combine(destination, $"func_{entry:X8}.json"), JsonSerializer.Serialize(metadata, new JsonSerializerOptions { WriteIndented = true }));
        Console.WriteLine($"Retranslated DOL function {entry:X8}: {result.Instructions.Count} PPC instructions, zero unsupported.");
    }
}
