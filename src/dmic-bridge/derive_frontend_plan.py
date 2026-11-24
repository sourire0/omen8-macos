#!/usr/bin/env python3
"""Derive review-only IPC4 frontend payload candidates; no device access."""
import hashlib,json,struct,sys
from pathlib import Path
import build_dmic_operation_plan as p
base=Path('mic-diagnostics')
sys.path.insert(0,str(base/'HPIPC4Audit'))
import audit_oem_ipc4 as audit
source=json.loads(Path('generated/dmic-frontend-source-records.json').read_text())
fwpath=base/'upstream-ipc4-reference/sof-adl-ipc4-intel-signed-v2.12.ri'; blob=fwpath.read_bytes();fw=audit.parse_firmware(fwpath.name,blob)
modules={m.module_uuid:m for m in fw.modules};configs=p.parse_firmware_module_configs(blob,fw,audit)
result={'status':'offline candidate, not hardware-validated','pipeline':'dynamic ID required','host_stream_tag':'runtime input tag required','widgets':{},'routes':source['routes'],'requirements':['Allocate separate pipeline and module instances','Bind across pipelines using available source pin, not a fixed assumed pin','Derive resource budget and correct multi-pipeline state ordering from upstream','Confirm control defaults and selected path']}
for name in ('tdfb.11.1','drc.11.1','host-copier.6.capture'):
 w=source['widgets'][name];m=modules[w['modules'][0]['uuid']];ii,ins=p.select_format(w['input_formats'],p.STREAM_FORMAT,name,'input');oi,outs=p.select_format(w['output_formats'],p.STREAM_FORMAT,name,'output')
 pairs=configs[m.module_uuid];matching=[x for x in pairs if x['ibs']==ins['buffer_size'] and x['obs']==outs['buffer_size']];cpc=max((x['cpc'] for x in matching),default=0)
 private=bytes.fromhex(w['private_hex']);import validate_ipc4_dmic_candidate as a
 tokens={t:v for k,t,v in a.parse_tuples(private) if k in (2,4)};is_pages=tokens.get(409,0)
 keys=('sampling_frequency','bit_depth','channel_map','channel_config','interleaving_style','format_config')
 words=[cpc,ins['buffer_size'],outs['buffer_size'],is_pages]+[ins[k] for k in keys]
 data=struct.pack('<10I',*words)
 if ((m.module_type >> 8) & 0xf) == 1:
  if name != 'tdfb.11.1': raise ValueError('unreviewed extended module')
  data += struct.pack('<HH12s', 1, 1, b'')
  data += struct.pack('<8I', 0, ins['buffer_size'], *[ins[k] for k in keys])
  data += struct.pack('<8I', 0, outs['buffer_size'], *[outs[k] for k in keys])
 if name.startswith('host-copier'):
  # tag 1 template: four periods.
  data+=struct.pack('<6I',*[outs[k] for k in keys])+struct.pack('<5I',0,0x100,4*outs['buffer_size'],1,0)
 controls=[]
 for c in w['controls']:
  if c['type']!=2: continue
  raw=bytes.fromhex(c['raw_hex']);tail=raw[240:];magic,param,size,abi=struct.unpack_from('<IIII',tail)
  if magic!=0x34464f53 or len(tail)!=32+size:raise ValueError(f'{name} invalid ABI')
  # IPC4 LargeConfigSet carries the fragment bytes directly. The generic
  # control-message header (id/num_elems/reserved) is not part of a TDFB
  # algorithm blob; prepending it makes SOF interpret its reserved words as
  # sof_tdfb_config fields (including num_output_channels == 0).
  body=tail[32:]
  controls.append({'name':c['name'],'parameter_id':param,'abi':abi,'wire_hex':body.hex(),'wire_sha256':hashlib.sha256(body).hexdigest()})
 result['widgets'][name]={'firmware_module':m.name,'module_id':m.index,'input_format':ins,'output_format':outs,'is_pages':is_pages,'cpc':cpc,'matching_firmware_configs':matching,'resource_estimate':p.module_resource_pages(m.module_type,ins['buffer_size'],is_pages),'init_hex':data.hex(),'init_bytes':len(data),'controls':controls}
path=Path('generated/dmic-frontend-review-plan.json');path.write_text(json.dumps(result,indent=2)+'\n')
for k,v in result['widgets'].items(): print(k,'module',v['module_id'],'init',v['init_bytes'],'pages',v['resource_estimate']['pages'],'binary controls',[(x['name'],len(bytes.fromhex(x['wire_hex']))) for x in v['controls']])
print('output',path)
