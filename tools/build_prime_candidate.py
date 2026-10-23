#!/usr/bin/env python3
"""Link a private candidate without replacing the executable under manual QA.

Derives only the EXE link edge from the configured Ninja graph. Native objects
and static libraries rebuild normally; assets and running-process files are not
copied. The canonical build graph is checked unchanged afterwards. Promotion is
a separate action after the old process exits.
"""
import hashlib
import os
from pathlib import Path
import re
import subprocess

ROOT=Path(__file__).resolve().parents[2]

def main():
    build=ROOT/'recomp/build/RM3E01_00_runtime_link_probe'
    cache=(build/'CMakeCache.txt').read_text(encoding='utf-8')
    cflags=re.search(r'^CMAKE_C_FLAGS_RELEASE:STRING=(.*)$',cache,re.MULTILINE)
    optimization=re.findall(r'(?:^|\s)(-O(?:[0123sz]|fast|g))(?=\s|$)',cflags.group(1)) if cflags else []
    if not optimization or optimization[-1] in ('-O0','-Og'):
        raise RuntimeError('Release C dependencies have no optimization; reconfigure with -DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG"')
    graph=build/'build.ninja'
    data=graph.read_bytes();text=data.decode('utf-8').replace('\r\n','\n')
    pattern=r'^build WiiCompiled\.exe: [^\n]+\n(?:  [^\n]*\n)+'
    matches=list(re.finditer(pattern,text,re.MULTILINE))
    if len(matches)!=1:raise RuntimeError('Expected one canonical EXE link edge')
    match=matches[0];edge=match.group()
    if edge.count('  TARGET_FILE = WiiCompiled.exe\n')!=1:
        raise RuntimeError('Unexpected target-file definition')
    candidate=edge.replace('WiiCompiled.exe','WiiCompiled.candidate.exe')
    candidate=candidate.replace('libWiiCompiled.dll.a','libWiiCompiled.candidate.dll.a')
    candidate=candidate.replace('WiiCompiled.rsp','WiiCompiled.candidate.rsp')
    candidate,count=re.subn(r'^  POST_BUILD = [^\n]*$', '  POST_BUILD = cd .',candidate,flags=re.MULTILINE)
    if count!=1:raise RuntimeError('Expected one asset-copy post-build command')
    derived=build/'build.prime-candidate.ninja'
    derived.write_text(text[:match.start()]+candidate+text[match.end():],encoding='utf-8',newline='')
    ninja=ROOT/'recomp/.tools/portable-tools/Ninja/ninja.exe'
    temporary=ROOT/'recomp/.tools/compiler-temp'
    temporary.mkdir(exist_ok=True)
    environment=os.environ.copy()
    environment['TEMP']=str(temporary);environment['TMP']=str(temporary)
    # CMake may retain short tool names (notably windres) for dependencies.
    # Resolve them from the same pinned toolchain as the compiler.
    environment['PATH']=str(ROOT/'recomp/.tools/portable-tools/llvm-mingw/bin')+os.pathsep+environment.get('PATH','')
    result=subprocess.run([str(ninja),'-f',derived.name,'WiiCompiled.candidate.exe','-j','1'],cwd=build,env=environment)
    if graph.read_bytes()!=data:raise RuntimeError('Canonical build graph changed during candidate build; revalidate before promotion')
    if result.returncode:raise SystemExit(result.returncode)
    output=build/'WiiCompiled.candidate.exe'
    print('Candidate SHA256:',hashlib.sha256(output.read_bytes()).hexdigest())
    print('Canonical executable preserved; candidate has not been launched or promoted.')

if __name__=='__main__':main()
