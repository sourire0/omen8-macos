#!/usr/bin/env python3
"""Extract exact candidate frontend records and routes from the original topology."""
import hashlib, json, struct, sys
from pathlib import Path
BASE=Path('mic-diagnostics')
sys.path.insert(0,str(BASE/'HPIPC4Audit'))
import validate_ipc4_dmic_candidate as a
import audit_oem_ipc4 as f
import build_dmic_operation_plan as plan
src=BASE/'upstream-ipc4-reference/sof-hda-generic-2ch-ipc4-v2.12.tplg'
fwfile=BASE/'upstream-ipc4-reference/sof-adl-ipc4-intel-signed-v2.12.ri'
fw=f.parse_firmware(fwfile.name,fwfile.read_bytes()); modules={x.module_uuid:x for x in fw.modules}
wanted={'host-copier.6.capture','pipeline.11','tdfb.11.1','drc.11.1','module-copier.12.2'}
output={'source':str(src),'source_sha256':hashlib.sha256(src.read_bytes()).hexdigest(),'firmware_sha256':hashlib.sha256(fwfile.read_bytes()).hexdigest(),'widgets':{},'routes':[]}
for typ,idx,r in a.iter_records(a.parse_blocks(src.read_bytes())):
 if typ==4:
  sink,control,source=(a.cstr(r[x:x+44]) for x in (0,44,88))
  if source in wanted and sink in wanted: output['routes'].append({'source':source,'sink':sink,'control':control,'graph_index':idx})
 if typ!=5: continue
 name=a.cstr(r[8:52])
 if name not in wanted: continue
 count,size=struct.unpack_from('<II',r,124);private=r[132:132+size]; tuples=a.parse_tuples(private)
 selected=[]
 for k,t,v in tuples:
  if k==0 and v in modules:
   m=modules[v];selected.append({'uuid':str(v),'name':m.name,'manifest_index':m.index})
 controls=[];pos=132+size
 for i in range(count):
  kind=struct.unpack_from('<I',r,pos+4)[0];base={1:360,2:240,3:1764}[kind]; extra=struct.unpack_from('<I',r,pos+base-4)[0];raw=r[pos:pos+base+extra]
  if len(raw)!=base+extra: raise ValueError('truncated control')
  controls.append({'name':a.cstr(raw[8:52]),'type':kind,'bytes':len(raw),'raw_hex':raw.hex(),'sha256':hashlib.sha256(raw).hexdigest()});pos+=len(raw)
 if pos!=len(r): raise ValueError('widget record trailing bytes')
 output['widgets'][name]={'block_index':idx,'type':struct.unpack_from('<I',r,4)[0],'stream':a.cstr(r[52:96]),'private_hex':private.hex(),'private_sha256':hashlib.sha256(private).hexdigest(),'modules':selected,'input_formats':plan.parse_audio_formats(tuples,415,plan.INPUT_FORMAT_TOKENS),'output_formats':plan.parse_audio_formats(tuples,416,plan.OUTPUT_FORMAT_TOKENS),'controls':controls}
missing=wanted-set(output['widgets'])
if missing: raise ValueError(f'missing widgets {missing}')
out=Path('generated/dmic-frontend-source-records.json');out.write_text(json.dumps(output,indent=2,ensure_ascii=False)+'\n')
for name,w in output['widgets'].items(): print(name, w['modules'], 'controls', [(c['name'],c['type'],c['bytes']) for c in w['controls']])
print('routes',output['routes']);print('output',out)
