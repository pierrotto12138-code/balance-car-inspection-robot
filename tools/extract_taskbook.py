"""Extract readable text from the submitted task book DOCX."""
import re
import zipfile

DOCX = r"D:\260921Task\docs\task\2026 测控系统综合设计任务书_1组.docx"

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
