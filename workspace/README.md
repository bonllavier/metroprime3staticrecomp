# Workspace support files

The build scripts run from the folder that contains `recomp/` (the "workspace root") and read four
files from outside this repository. They come from the matching decompilation project of the same
game revision. They are symbol metadata (names, addresses, sizes, hashes), not game data. Copies live
here, and `tools/setup_workspace.sh` / `tools/setup_workspace.ps1` place them where the scripts
expect them, without overwriting existing files:

| Copy here | Placed at (workspace root) | Used by |
|---|---|---|
| `tools/inspect_startup_rso.py` | `tools/inspect_startup_rso.py` | RSO conversion and translation |
| `tools/inspect_static_sel.py` | `tools/inspect_static_sel.py` | RSO conversion, registry |
| `decomp/config/RM3E01_00/symbols.txt` | `decomp/config/RM3E01_00/symbols.txt` | RSO conversion, audits |
| `decomp/config/RM3E01_00/recovered_leaf.json` | `decomp/config/RM3E01_00/recovered_leaf.json` | audits |
