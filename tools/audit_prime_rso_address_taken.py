#!/usr/bin/env python3
"""Check code addresses materialized by the active translated RSO bodies.

This gate is deliberately narrower than complete indirect-call coverage:
data tables, unknown dispatch inputs and unexecuted code need separate audits.
"""
import argparse
import hashlib
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
MODULES=('rhbm','Tweaks','ScriptGUIScreenFrontEndUI','ScriptWorldTeleporter',
         'ScriptAchievement','ScriptContextSensitiveActivator','ScriptGrapplePoint',
         'ScriptMultiModelActor','ScriptContextSensitiveAction','ScriptContextActionButtonPressing','Friendly','GunTurret')


def audit():
    results=[];missing=[]
    for name in MODULES:
        directory=ROOT/'recomp/.tools/rso-runtime'/name
        module=json.loads((directory/'module.json').read_text())
        translation=json.loads((directory/'functions/translation.json').read_text())
        original=ROOT/f'recomp/.tools/disc-extraction-rm3e01/files/RSO/Rev2 Production/RSO_{name}.rso'
        assert hashlib.sha256(original.read_bytes()).hexdigest()==module['original_sha256']
        entries={r['entry'] for r in translation['functions']}
        assert len(entries)==len(translation['functions'])
        covered=set()
        for function in translation['functions']:
            source=directory/'functions'/function['path']
            assert hashlib.sha256(source.read_bytes()).hexdigest()==function['sha256']
            assert not covered.intersection(function['instructionAddresses']),name
            covered.update(function['instructionAddresses'])
        base=module['sections'][1]['nominal'];references=[]
        for row in module['relocations']['internal']:
            if row['source_section']!=1 or row['index']!=1 or row['type'] not in (4,6):continue
            source=base+(row['source_section_offset']&~3)
            if source not in covered:continue
            target=base+row['target_offset']
            entry={'module':name,'source':hex(source),'target':hex(target),'relocation_type':row['type'],
                   'registered_entry':target in entries,'inside_translated_body':target in covered}
            references.append(entry)
            if target not in entries:missing.append(entry)
        results.append({'module':name,'functions':len(entries),'materialized_code_references':references})
    return {'result':'PASS' if not missing else 'FAIL missing materialized code entries',
            'revision':'RM3E01_00','modules':results,'missing':missing,
            'total_functions':sum(r['functions'] for r in results),
            'scope':'Current body code-address HA/LO references only; not complete RSO recovery, full indirect-call coverage or runtime stability.'}


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();report=audit()
    args.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'result':report['result'],'functions':report['total_functions'],'missing':report['missing']},indent=2))
    raise SystemExit(bool(report['missing']))
