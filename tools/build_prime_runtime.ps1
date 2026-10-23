param([int]$Jobs = 4)
$ErrorActionPreference = 'Stop'
$taskRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Push-Location $taskRoot
try {
    $taskNod = Join-Path $taskRoot 'recomp/.tools/nod-source'
    $taskNodLibrary = Join-Path $taskNod 'target/x86_64-pc-windows-gnu/release/libnod.a'
    if (-not (Test-Path -LiteralPath $taskNodLibrary)) {
        throw 'Pinned nod archive missing; run recomp/tools/build_and_run_nod_iso_probe.ps1 first.'
    }
    $taskNodCommit = (& git -C $taskNod rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or $taskNodCommit -ne '6a54b418e541ce5eeb364399116afd6e47b817bb') {
        throw 'Unexpected nod source revision'
    }
    $taskNodChanges = & git -C $taskNod status --porcelain
    if ($LASTEXITCODE -ne 0 -or $taskNodChanges) { throw 'Pinned nod source is modified' }
    & recomp/.tools/dotnet/dotnet.exe build recomp/third_party/WiiCompiled/translator/src/Translator.Cli/Translator.Cli.csproj -c Release --nologo --no-incremental
    if ($LASTEXITCODE -ne 0) { throw 'Prime shard translator build failed' }
    & recomp/.tools/dotnet/dotnet.exe build recomp/tools/RsoRuntimeTranslator/RsoRuntimeTranslator.csproj -c Release --nologo --no-incremental
    if ($LASTEXITCODE -ne 0) { throw 'Prime RSO translator build failed' }
    & recomp/.tools/dotnet/dotnet.exe recomp/tools/RsoRuntimeTranslator/bin/Release/net8.0/RsoRuntimeTranslator.dll --self-test-relocations
    if ($LASTEXITCODE -ne 0) { throw 'Prime RSO relocated instruction tests failed' }
    foreach ($taskModule in @('rhbm', 'Tweaks', 'ScriptGUIScreenFrontEndUI', 'ScriptWorldTeleporter','ScriptAchievement','ScriptContextSensitiveActivator','ScriptGrapplePoint','ScriptMultiModelActor','ScriptContextSensitiveAction','ScriptContextActionButtonPressing','Friendly','GunTurret','MapScreenWebInterface')) {
        python -B recomp/tools/convert_rso_to_rel_probe.py "recomp/.tools/disc-extraction-rm3e01/files/RSO/Rev2 Production/RSO_$taskModule.rso" "recomp/.tools/rso-rel/$taskModule.synthetic.rel"
        if ($LASTEXITCODE -ne 0) { throw "Prime $taskModule analysis image conversion failed" }
        python -B recomp/tools/prepare_prime_rso_translation.py --module $taskModule
        if ($LASTEXITCODE -ne 0) { throw "Prime $taskModule manifest validation failed" }
        & recomp/.tools/dotnet/dotnet.exe recomp/tools/RsoRuntimeTranslator/bin/Release/net8.0/RsoRuntimeTranslator.dll `
            "recomp/.tools/rso-runtime/$taskModule/input.json" "recomp/.tools/rso-runtime/$taskModule/functions"
        if ($LASTEXITCODE -ne 0) { throw "Prime $taskModule translation failed" }
        python -B recomp/tools/verify_rso_rel_probe.py "recomp/.tools/disc-extraction-rm3e01/files/RSO/Rev2 Production/RSO_$taskModule.rso" `
            "recomp/.tools/rso-rel/$taskModule.synthetic.rel" "recomp/.tools/rso-runtime/$taskModule/functions/relocated_module.bin" `
            --output "recomp/.tools/rso-runtime/$taskModule/nominal_relocations.json"
        if ($LASTEXITCODE -ne 0) { throw "Prime $taskModule independent relocation verification failed" }
        python -B recomp/tools/emit_prime_rso_registry.py --module $taskModule
        if ($LASTEXITCODE -ne 0) { throw "Prime $taskModule registry validation failed" }
    }
    python -B recomp/tools/audit_prime_rso_address_taken.py --output recomp/.tools/rso-runtime/address_taken_coverage.json
    if ($LASTEXITCODE -ne 0) { throw 'Prime RSO address-taken entry coverage failed' }
    $taskCache = Get-Content -LiteralPath 'recomp/build/RM3E01_00_runtime_link_probe/CMakeCache.txt' -Raw
    $taskExpectedNod = $taskNod.Replace('\', '/')
    $taskExpectedRso = (Join-Path $taskRoot 'recomp/.tools/rso-runtime/rhbm').Replace('\', '/')
    if ($taskCache -notmatch ('(?m)^MKW_PRIME_NOD_SOURCE_DIR:PATH=' + [regex]::Escape($taskExpectedNod) + '\r?$') -or
        $taskCache -notmatch ('(?m)^MKW_PRIME_RSO_DIR:PATH=' + [regex]::Escape($taskExpectedRso) + '\r?$')) {
        cmake -S recomp/third_party/WiiCompiled/runtime -B recomp/build/RM3E01_00_runtime_link_probe "-DMKW_PRIME_NOD_SOURCE_DIR=$taskExpectedNod" "-DMKW_PRIME_RSO_DIR=$taskExpectedRso" '-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG' -DCMAKE_SUPPRESS_REGENERATION=ON
        if ($LASTEXITCODE -ne 0) { throw 'Prime ISO adapter configuration failed' }
    }
    foreach ($taskRepair in @('800E01F0','80420444','8019804C','8042B530','80423190','80423EF8',
            '8040D9D8','8040F98C','80414E20','80415CB8','804167CC','80419848',
            '80425B10','80427A34','804280AC')) {
        & recomp/.tools/dotnet/dotnet.exe recomp/tools/RsoRuntimeTranslator/bin/Release/net8.0/RsoRuntimeTranslator.dll `
            --dol-repair . $taskRepair recomp/.tools/dol-runtime-repairs
        if ($LASTEXITCODE -ne 0) { throw "Prime DOL switch repair $taskRepair translation failed" }
    }
    python -B recomp/tools/prepare_prime_runtime_hooks.py
    if ($LASTEXITCODE -ne 0) { throw 'Runtime overlay verification failed' }
    & recomp/.tools/dotnet/dotnet.exe recomp/third_party/WiiCompiled/translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll emit-build-shards `
        --project recomp/config/RM3E01_00/project_full_conservative.yml `
        --base-metadata recomp/generated/RM3E01_00_conservative/runtime_hooks/base_translation_output.json `
        --base-functions-dir recomp/generated/RM3E01_00_conservative `
        --native-source-dir recomp/third_party/WiiCompiled/runtime/src `
        --out recomp/generated/RM3E01_00_conservative/build_shards
    if ($LASTEXITCODE -ne 0) { throw 'Runtime shard emission failed' }
    # Reconfigure explicitly: a backwards host-clock adjustment can make changed
    # CMake inputs appear older than the already generated build graph.
    cmake -S recomp/third_party/WiiCompiled/runtime -B recomp/build/RM3E01_00_runtime_link_probe `
        "-DMKW_PRIME_NOD_SOURCE_DIR=$taskExpectedNod" "-DMKW_PRIME_RSO_DIR=$taskExpectedRso" '-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG' -DCMAKE_SUPPRESS_REGENERATION=ON
    if ($LASTEXITCODE -ne 0) { throw 'Prime runtime graph configuration failed' }
    $taskBuildRoot = (Resolve-Path 'recomp/build/RM3E01_00_runtime_link_probe').Path
    foreach ($taskObjectTarget in @('mkw_runtime_common', 'mkw_prime_rso')) {
        $taskObjectRoot = Join-Path $taskBuildRoot "CMakeFiles/$taskObjectTarget.dir"
        if (Test-Path -LiteralPath $taskObjectRoot) {
            $taskFutureObjects = @(Get-ChildItem -LiteralPath $taskObjectRoot -Recurse -File -Filter '*.obj' |
                Where-Object { $_.LastWriteTimeUtc -gt [DateTime]::UtcNow.AddSeconds(2) })
            foreach ($taskObject in $taskFutureObjects) {
                $taskResolvedObject = (Resolve-Path -LiteralPath $taskObject.FullName).Path
                if (-not $taskResolvedObject.StartsWith($taskBuildRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
                    throw 'Refusing to invalidate an object outside the intended build directory'
                }
                Remove-Item -LiteralPath $taskResolvedObject
            }
            if ($taskFutureObjects.Count) { Write-Output "Invalidated $($taskFutureObjects.Count) future-dated objects in $taskObjectTarget after host clock change." }
        }
    }
    cmake --build recomp/build/RM3E01_00_runtime_link_probe --target WiiCompiled.exe --parallel $Jobs
    if ($LASTEXITCODE -ne 0) { throw 'Runtime build failed' }
    python -B recomp/tools/repair_prime_clock_metadata.py
    if ($LASTEXITCODE -ne 0) { throw 'Post-build clock metadata verification failed' }
} finally { Pop-Location }
