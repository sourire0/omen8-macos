#!/usr/bin/env python3
"""Materialize the unvalidated frontend payloads for compile-only review."""
import json
from pathlib import Path
plan=json.loads(Path('generated/dmic-frontend-review-plan.json').read_text())
w=plan['widgets']
items={'TdfbInit':w['tdfb.11.1']['init_hex'],'DrcInit':w['drc.11.1']['init_hex'],'HostInitTag1':w['host-copier.6.capture']['init_hex'],'TdfbControl':w['tdfb.11.1']['controls'][0]['wire_hex'],'DrcControl':w['drc.11.1']['controls'][0]['wire_hex']}
out=['// Generated review-only payload candidates; not linked into the active driver.','#pragma once','#include <stddef.h>','#include <stdint.h>','namespace sof::ipc4::dmic::frontend_review {']
for name,value in items.items():
 data=bytes.fromhex(value);out.append(f'inline constexpr uint8_t k{name}[] = {{')
 for i in range(0,len(data),12):out.append('    '+', '.join(f'0x{x:02x}' for x in data[i:i+12])+',')
 out.append('};');out.append(f'inline constexpr size_t k{name}Bytes = sizeof(k{name});')
out.append('}');Path('generated/DmicFrontendReviewData.hpp').write_text('\n'.join(out)+'\n')
print('generated five payload arrays')
