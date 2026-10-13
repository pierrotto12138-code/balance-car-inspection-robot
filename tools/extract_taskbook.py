"""把一份 DOCX（例如任务书）里的可见文本抽出来，便于检索与引用。

用法：
    python tools/extract_taskbook.py <某个.docx>

说明：任务书原件**不在本仓库**（含组员学号等个人信息，按仓库约定不入库），
所以这个脚本要求显式给出路径；也可以放到自己本地任意位置。
"""
import sys
import re
import zipfile

if len(sys.argv) < 2:
    print(__doc__)
    raise SystemExit(2)

DOCX = sys.argv[1]

with zipfile.ZipFile(DOCX) as z:
    xml = z.read("word/document.xml").decode("utf-8", "replace")

# 按段落切分，逐段取 w:t 文本
paras = re.findall(r"<w:p[ >].*?</w:p>", xml, re.S)
print("段落数: %d" % len(paras))
print("=" * 80)
out = []
for p in paras:
    # 表格单元格里的段落会以 | 分隔
    texts = re.findall(r"<w:t[^>]*>(.*?)</w:t>", p, re.S)
    line = "".join(texts)
    line = (line.replace("&amp;", "&").replace("&lt;", "<")
                .replace("&gt;", ">").replace("&quot;", '"'))
    out.append(line)

# 去掉连续空行
prev_blank = False
for line in out:
    blank = not line.strip()
    if blank and prev_blank:
        continue
    prev_blank = blank
    print(line)
