"""清理 .eprj3 工程里残留的注释文字（还原到参考01的既定干净状态）。

删除内容（均已验证：同段内无任何其它行引用其 id）：
  1) 原理图 SCH_PAGE 段里的 TEXT 记录          —— 可见的模块注释文字
  2) PCB 段里含中文的 STRING 记录              —— 可见的宋体丝印（含校名）
  3) PCB 里的整个 FONT 文档（DOCHEAD + FONT）  —— 上述中文的矢量字形资源
  4) PCB 段里 layerId=9 的 ATTR 记录           —— KiCad 库元数据
                                                 （ki_description / ki_keywords /
                                                  Name / sheetfile / sheetname /
                                                  Attribute1-3，全部 valueVisible=false）

保留：引脚名/网络名丝印（PA2、PB3、5V、GND、3V3、LED_xx）、位号、
      极性 +/－、全部 COMPONENT/WIRE/PAD/NET。

所有文件写入前都会做“包含性校验”：目标路径必须位于允许目录内，否则拒绝。

用法：
    python clean_eprj3.py --project "<工程文件夹>"            # 预演（默认，不改文件）
    python clean_eprj3.py --project "<工程文件夹>" --apply    # 实际执行（先备份）
"""
import argparse
import collections
import json
import re
import sys
import time
from pathlib import Path

CJK = re.compile(r"[\u4e00-\u9fff]")
SOURCE_SUFFIXES = (".esch2", ".epcb2", ".epan2")


def contained(base: Path, target: Path) -> Path:
    """Return target only if it resolves inside base; otherwise refuse.

    Path.relative_to raises ValueError when target is not under base, which
    is what blocks any "../" escape.
    """
    base_r = base.resolve()
    target_r = target.resolve()
    target_r.relative_to(base_r)
    return target_r


def parse(path: Path):
    raw = path.read_text(encoding="utf-8", errors="replace")
    crlf = "\r\n" in raw
    recs = []
    for idx, s in enumerate(raw.split("\n")):
        bare = s.rstrip("\r")
        if not bare.strip():
            recs.append((idx, None, None, bare))
            continue
        head, sep, rest = bare.partition("||")
        payload = rest[:-1] if rest.endswith("|") else rest
        try:
            h = json.loads(head)
        except Exception:
            h = None
        try:
            p = json.loads(payload) if payload else None
        except Exception:
            p = payload
        recs.append((idx, h, p, bare))
    return recs, crlf


def sections(recs):
    cur = "?"
    m = {}
    for idx, h, p, s in recs:
        if h and h.get("type") == "DOCHEAD" and isinstance(p, dict):
            cur = p.get("docType", "?")
        m[idx] = cur
    return m


def classify(recs):
    sec = sections(recs)
    drop = {}
    for idx, h, p, s in recs:
        if not h:
            continue
        t = h.get("type")
        st = sec[idx]
        if t == "TEXT" and st == "SCH_PAGE":
            drop[idx] = "SCH TEXT (可见注释)"
        elif t == "FONT":
            drop[idx] = "FONT 字形资源"
        elif t == "DOCHEAD" and isinstance(p, dict) and p.get("docType") == "FONT":
            drop[idx] = "FONT 文档头"
        elif t == "STRING" and st == "PCB" and isinstance(p, dict) \
                and CJK.search(str(p.get("text", ""))):
            drop[idx] = "PCB 中文丝印"
        elif t == "ATTR" and st == "PCB" and isinstance(p, dict) \
                and p.get("layerId") == 9:
            drop[idx] = "layer9 KiCad 元数据"
    return drop


def report(path: Path, label):
    recs, crlf = parse(path)
    drop = classify(recs)
    print("\n" + "=" * 78)
    print("%s  %s" % (label, path.name))
    print("=" * 78)
    print("  换行符: %s" % ("CRLF" if crlf else "LF"))
    print("  待删除 %d 行:" % len(drop))
    for r, n in collections.Counter(drop.values()).most_common():
        print("     %-26s %d" % (r, n))
    kept = collections.Counter()
    for idx, h, p, s in recs:
        if idx in drop or not h:
            continue
        kept[h.get("type")] += 1
    print("  删除后各类型计数（前 22）:")
    for k, v in kept.most_common(22):
        print("     %-22s %d" % (k, v))
    return recs, crlf, drop


def apply_cleanup(source: Path, project_root: Path, backup_root: Path,
                  recs, crlf, drop):
    """Back up then rewrite. Both destination paths are containment-checked."""
    project_root = project_root.resolve()
    backup_root = backup_root.resolve()
    backup_root.mkdir(parents=True, exist_ok=True)

    src_checked = contained(project_root, source)
    dst_checked = contained(backup_root, backup_root / source.name)
    dst_checked.write_bytes(src_checked.read_bytes())

    keep = [s for idx, h, p, s in recs if idx not in drop]
    nl = "\r\n" if crlf else "\n"
    src_checked.write_text(nl.join(keep), encoding="utf-8", newline="")
    return dst_checked, len(keep)


def collect_sources(root: Path):
    out = []
    for p in root.rglob("*"):
        if p.is_file() and p.name.endswith(SOURCE_SUFFIXES):
            out.append(p)
    out.sort()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--project", required=True)
    ap.add_argument("--apply", action="store_true")
    args = ap.parse_args()

    root = Path(args.project).resolve()
    if not root.is_dir():
        print("工程目录不存在: %s" % root)
        return 2

    targets = collect_sources(root)
    if not targets:
        print("没找到 .esch2/.epcb2/.epan2 源文件")
        return 2
    for t in targets:
        contained(root, t)          # 逐个确认在工程目录内

    plans = []
    for t in targets:
        recs, crlf, drop = report(t, "扫描")
        plans.append((t, recs, crlf, drop))

    total = sum(len(d) for _, _, _, d in plans)
    print("\n总计待删除 %d 行" % total)

    if not args.apply:
        print("\n（预演模式，未修改任何文件。加 --apply 执行。）")
        return 0

    stamp = time.strftime("%Y%m%d-%H%M%S")
    backup_root = root.parent / ("_eprj3_backup_%s_%s" % (root.name, stamp))
    print("\n备份到: %s" % backup_root)
    for t, recs, crlf, drop in plans:
        if not drop:
            print("  跳过（无需清理）: %s" % t.name)
            continue
        dst, kept = apply_cleanup(t, root, backup_root, recs, crlf, drop)
        print("  已清理 %-30s 删 %d 行, 留 %d 行  → 备份 %s"
              % (t.name, len(drop), kept, dst.name))
    print("\n完成。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
