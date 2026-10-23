using System.Security.Cryptography;
using System.Text.Json;
using System.Text.RegularExpressions;
using Translator.Core.Loading;
using Translator.Core.Parsing.Dol;
using Translator.Core.Parsing.Rel;
using Translator.Core.Translation;

if (args.Length == 1 && args[0] == "--self-test-relocations") {
    RelocatedInstructionsTests.Run();
    return;
}
if (args.Length == 4 && args[0] == "--dol-repair") {
    DolFunctionRepair.Run(args[1], Convert.ToUInt32(args[2], 16), args[3]);
    return;
}
if (args.Length != 2) throw new ArgumentException("usage: RsoRuntimeTranslator manifest.json output-directory");
var jsonOptions = new JsonSerializerOptions { PropertyNameCaseInsensitive = true, WriteIndented = true };
var input = JsonSerializer.Deserialize<RsoInput>(File.ReadAllText(args[0]), jsonOptions)!;
var moduleImage = RelFile.Load(input.Rel).BuildImage(input.Base);
var image = new ProgramImageBuilder().Build(DolFile.Load(input.Dol), moduleImage,
    ramSize: 0x700000);
var translator = new FunctionTranslator(image);
var rewriter = new RelocatedInstructions(input.ModuleId, input.Sites.ToDictionary(site => site.Pc));
var boundaries = input.Boundaries.ToHashSet();
for (var address = input.TextStart; address < input.TextEnd; address += 4)
{
    var instruction = System.Buffers.Binary.BinaryPrimitives.ReadUInt32BigEndian(image.Memory.AsSpan(image.GetOffset(address), 4));
    if ((instruction & 0xFC000003u) != 0x48000001u) continue;
    var displacement = (int)(instruction & 0x03FFFFFCu);
    if ((displacement & 0x02000000) != 0) displacement -= 0x04000000;
    var target = unchecked(address + (uint)displacement);
    if (target >= input.TextStart && target < input.TextEnd) boundaries.Add(target);
}
var queue = new Queue<uint>(input.Roots);
var translated = new HashSet<uint>();
var records = new List<object>();
Directory.CreateDirectory(args[1]);
var imagePath = Path.Combine(args[1], "relocated_module.bin");
if (!File.Exists(imagePath) || !File.ReadAllBytes(imagePath).SequenceEqual(moduleImage.Data))
    File.WriteAllBytes(imagePath, moduleImage.Data);
bool InModule(uint address) => address >= input.TextStart && address < input.TextEnd;
while (queue.TryDequeue(out var entry))
{
    if (!translated.Add(entry)) continue;
    if (!InModule(entry)) throw new InvalidOperationException($"Module entry out of text: {entry:X8}");
    var result = translator.Translate(entry, new TranslationOptions(
        EnableLeafInlining: false, InstructionTransform: rewriter.Transform,
        KnownFunctionEntryPoints: boundaries, AllowUnsupportedInstructions: false));
    if (result.Metrics.UnsupportedInstructionCount != 0) throw new InvalidOperationException("Unsupported module opcode");
    var code = result.CxxCode;
    code = code.Replace("#include \"guest_abi/guest_abi.h\"", "#include \"guest_abi/guest_abi.h\"\n#include \"hle/prime_rso_runtime.h\"");
    code = Regex.Replace(code, @"\bfunc_([0-9A-F]{8})\b", match => $"rso{input.ModuleId}_func_{match.Groups[1].Value}");
    code = Regex.Replace(code, @"InvokeDirectCpu<0x([0-9A-F]{8})u>\(ctx\)", match => {
        var target = Convert.ToUInt32(match.Groups[1].Value, 16);
        return InModule(target) ? $"InvokeIndirectCpu(PPC_RsoRuntimeAddress({input.ModuleId}u, 0x{target:X8}u), ctx)" : match.Value;
    });
    var lines = code.Split('\n');
    for (var i = 0; i < lines.Length; ++i)
    {
        var switchMatch = Regex.Match(lines[i], @"^(\s*)switch \((.+)\)\s*\{");
        if (!switchMatch.Success) continue;
        var depth = 0;
        var cases = new List<uint>();
        for (var j = i; j < lines.Length; ++j)
        {
            depth += lines[j].Count(c => c == '{') - lines[j].Count(c => c == '}');
            var caseMatch = Regex.Match(lines[j], @"case 0x([0-9A-F]{8})u?:");
            if (caseMatch.Success) cases.Add(Convert.ToUInt32(caseMatch.Groups[1].Value, 16));
            if (depth == 0) break;
        }
        if (cases.Count != 0 && cases.All(InModule))
            lines[i] = $"{switchMatch.Groups[1].Value}switch (PPC_RsoNominalAddress({input.ModuleId}u, {switchMatch.Groups[2].Value})) {{";
        else throw new InvalidOperationException("Unreviewed module switch representation");
    }
    code = string.Join('\n', lines);
    var path = Path.Combine(args[1], $"rso{input.ModuleId}_func_{entry:X8}.cpp");
    WriteChanged(path, code);
    var dependencies = result.Emission?.EmittedDirectCallTargets ?? [];
    foreach (var target in dependencies.Where(InModule)) queue.Enqueue(target);
    records.Add(new { entry, name = $"rso{input.ModuleId}_func_{entry:X8}", path = Path.GetFileName(path),
        sha256 = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path))).ToLowerInvariant(),
        ppcInstructions = result.Instructions.Count, directTargets = dependencies,
        instructionAddresses = result.Instructions.Select(instruction => instruction.Address).ToArray() });
}
WriteChanged(Path.Combine(args[1], "translation.json"), JsonSerializer.Serialize(new {
    input.ModuleId, input.Base, functions = records, unsupportedInstructions = 0,
    runtimeRelocationSites = input.Sites.Count, leafInlining = false,
    limitation = "Function boundaries and live-link bindings require independent verification; not a complete reconstructed RSO."
}, jsonOptions));
Console.WriteLine($"Translated {records.Count} runtime-relocatable module functions with zero unsupported instructions.");

static void WriteChanged(string path, string content)
{
    if (!File.Exists(path) || File.ReadAllText(path) != content) File.WriteAllText(path, content);
}

sealed record RsoInput(string Dol, string Rel, uint Base, uint ModuleId,
    uint TextStart, uint TextEnd, List<uint> Roots, List<uint> Boundaries, List<RsoSite> Sites);
