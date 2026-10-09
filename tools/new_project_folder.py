# -*- coding: utf-8 -*-
"""把板 '改' 复制成一个新的文件夹格式工程：26测控系统设计"""
import io, os, json, shutil, datetime

SRC = r'C:\Users\Pierrot\Documents\LCEDA-Pro\projects\参考01_copy'
DST = r'C:\Users\Pierrot\Documents\LCEDA-Pro\projects\26测控系统设计'
NAME = '26测控系统设计'

print('=== 1) 建目录 ===')
os.makedirs(DST, exist_ok=True)
os.makedirs(os.path.join(DST, 'sch'), exist_ok=True)
os.makedirs(os.path.join(DST, 'pcb'), exist_ok=True)
print('   ', DST)

print()
print('=== 2) 复制 原理图(Schematic2) + PCB(PCB2) ===')
jobs = []
sd = os.path.join(SRC, 'sch', 'Schematic2')
if os.path.isdir(sd):
    for f in os.listdir(sd):
        jobs.append((os.path.join(sd, f), os.path.join(DST, 'sch', 'Schematic2', f)))
jobs.append((os.path.join(SRC, 'pcb', 'PCB2.epcb2'), os.path.join(DST, 'pcb', 'PCB2.epcb2')))
for s, d in jobs:
    if not os.path.exists(s):
        print('   源缺失:', s); continue
    os.makedirs(os.path.dirname(d), exist_ok=True)
    shutil.copy2(s, d)
    print('   %8.1f KB -> %s' % (os.path.getsize(d) / 1024, d.replace(DST, '')))

print()
print('=== 3) 生成 .eprj3（只保留板 "改"）===')
src_p = os.path.join(SRC, '参考01_copy.eprj3')
proj = json.load(io.open(src_p, encoding='utf-8'))

BOARD = 'b46a7b3bb4340c01'          # 改
SCH = 'f5ad88eeac89943d'            # Schematic2
SHEET = 'c405558037e65ebd'          # P1
PCB = '07ed4147375824d3'            # PCB2

p = proj['profile']
p['boards'] = {BOARD: p['boards'][BOARD]}
p['schematics'] = {SCH: p['schematics'][SCH]}
p['sheets'] = {SHEET: p['sheets'][SHEET]}
p['pcbs'] = {PCB: p['pcbs'][PCB]}
p['panels'] = {}
p['blockSymbols'] = {}
p['simSchematics'] = {}
p['simulations'] = {}
proj['name'] = NAME
proj['content'] = ''
proj['default_sheet'] = '626b2d91c91eb6dc'
proj['config'] = {"defaultSheet": "626b2d91c91eb6dc", "settings": {}}
# 新工程 uuid（沿用 createProject 生成的那个）
proj['_uuid'] = 'd9780e7b97dc16dd784e94562a04b2dbc057cd3ae4cdeff2b2f7c42ccfc4b991'

dst_p = os.path.join(DST, NAME + '.eprj3')
io.open(dst_p, 'w', encoding='utf-8').write(json.dumps(proj, ensure_ascii=False, indent=2))
print('   写入:', dst_p)

print()
print('=== 4) 复核 ===')
for root, dirs, files in os.walk(DST):
    for f in files:
        fp = os.path.join(root, f)
        print('   %8.1f KB  %s' % (os.path.getsize(fp) / 1024, fp.replace(DST, '')))
print()
print('   boards    :', ', '.join(v.get('title', '?') for v in proj['profile']['boards'].values()))
print('   schematics:', ', '.join(v.get('name', '?') for v in proj['profile']['schematics'].values()))
print('   pcbs      :', ', '.join(v.get('title', '?') for v in proj['profile']['pcbs'].values()))
