"""Serial import of every frozen genuine shader for the original NVN resolver.

Only uses the existing remapper/official trim-capabilities/validator. A PAUSE file
in output prevents starting the next module, so application builds take priority.
"""
import argparse, hashlib, json, pathlib, sys, time
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parent.parent/'harmony-shaders'))
import shader_bundle
from pack_game_shaders import pack

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    for name in ('source','output','validator','optimizer'):ap.add_argument('--'+name,type=pathlib.Path,required=True)
    args=ap.parse_args();repo=pathlib.Path(__file__).resolve().parents[2]
    if args.output.resolve().is_relative_to(repo) or args.output.resolve().is_relative_to(args.source.resolve()):raise ValueError('output must be external and separate from original material')
    args.output.mkdir(parents=True,exist_ok=False);derived=args.output/'derived';library=args.output/'library';derived.mkdir();library.mkdir()
    rows=shader_bundle.rows(args.source);report={'total':len(rows),'passed':[],'failures':[],'device_validation':'NOT_RUN'};start=time.monotonic()
    for i,row in enumerate(rows):
        while (args.output/'PAUSE').exists():time.sleep(.25)
        try:
            reflection=shader_bundle.export(args.source,row,derived,args.validator,args.optimizer,True)
            key,data=pack(derived/(row['key']+'.vk.json'));(library/(key+'.xgs')).write_bytes(data)
            report['passed'].append({'key':key,'stage':row['stage'],'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()})
        except Exception as exc:
            report['failures'].append({'key':row['key'],'error':str(exc)[:4096]})
        if (i+1)%50==0 or i+1==len(rows):
            report['processed']=i+1;report['seconds']=round(time.monotonic()-start,2)
            (args.output/'progress.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
            print(json.dumps({'processed':i+1,'total':len(rows),'passed':len(report['passed']),'failed':len(report['failures']),'seconds':report['seconds']}),flush=True)
    (library/'manifest.json').write_text(json.dumps(report['passed'],indent=2)+'\n',encoding='utf-8')
    (args.output/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    return bool(report['failures'])
if __name__=='__main__':sys.exit(main())
