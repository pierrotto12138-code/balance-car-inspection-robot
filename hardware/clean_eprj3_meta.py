"""补齐清理：删除 .eprj3 里不可见的库元数据残留。

规则（每条均已核验）：

PCB 文件
  A) 段内 layerId=9 的 ATTR 记录            —— 已在上一步删除
  B) DEVICE 段的 META.attributes 里的噪声键  —— sheetfile / sheetname /
                                              ki_description / ki_keywords / Users
                                              （删除的是字典里的键，不删记录）
  C) COMPONENT 段的 attrs 里
     key 形如 AttributeN 且 value == "REF**" —— KiCad 位号占位符
     ★ 但 value 为 "+" / "-" 的极性标注必须保留（valueVisible=True，是可见丝印）

原理图文件：已干净，无需处理。

用法：
    python clean_eprj3_meta.py --project "<工程文件夹>"           # 预演
    python clean_eprj3_meta.py --project "<工程文件夹>" --apply   # 执行（先备份）
"""
import argparse
import collections
import json
import re
import shutil
import sys
import time
from pathlib import Path

SOURCE_SUFFIXES = (".esch2", ".epcb2", ".epan2")

# 只删这些键；Name / Symbol / Footprint / Designator / Value / Description 等保留
NOISE_KEYS = {"sheetfile", "sheetname", "ki_description", "ki_keywords", "Users"}
ATTR_KEY_RE = re.compile(r"^Attribute\d+$")
PLACEHOLDER = "REF**"


def contained(base: Path, target: Path) -> Path:
    base_r = base.resolve()
    tr = target.resolve()
    tr.relative_to(base_r)          # 越界会抛 ValueError
    return tr


def parse(path: Path):
    raw = path.read_text(encoding="utf-8", errors="replace")
    crlf = "\r\n" in raw
    recs = []
    for s in raw.split("\n"):
        bare = s.rstrip("\r")
        if not bare.strip():
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
        recs.append([h, p, bare, []])
    return recs, crlf


def sections(recs):
    cur = "?"
    out = []
    for h, p, s, _ in recs:
        if h and h.get("type") == "DOCHEAD" and isinstance(p, dict):
            cur = p.get("docType", "?")
        out.append(cur)
    return out


def plan(path: Path):
    """Return list of (index, kind, detail) for records needing changes."""
    recs, crlf = parse(path)
    sec = sections(recs)
    changes = []
    for i, (h, p, s, _) in enumerate(recs):
        if not h or not isinstance(p, dict):
            continue
        t = h.get("type")
        if t == "META" and sec[i] == "DEVICE" and isinstance(p.get("attributes"), dict):
            drop = [k for k in p["attributes"] if k in NOISE_KEYS]
            if drop:
                changes.append((i, "META.attributes", sorted(drop)))
        elif t == "COMPONENT" and sec[i] == "PCB" and isinstance(p.get("attrs"), dict):
            drop = [k for k, v in p["attrs"].items()
                    if ATTR_KEY_RE.match(str(k)) and str(v) == PLACEHOLDER]
            if drop:
                changes.append((i, "COMPONENT.attrs", sorted(drop)))
    return recs, crlf, changes


def render(h, p, orig):
    """Rebuild the line, preserving the original header text verbatim."""
    head = orig.partition("||")[0]
    return "%s||%s|" % (head, json.dumps(p, ensure_ascii=False, separators=(",", ":")))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--project", required=True)
    ap.add_argument("--apply", action="store_true")
    args = ap.parse_args()

    root = Path(args.project).resolve()
    if not root.is_dir():
        print("工程目录不存在: %s" % root)
        return 2

    targets = sorted(p for p in root.rglob("*")
                     if p.is_file() and p.name.endswith(SOURCE_SUFFIXES))
    for t in targets:
        contained(root, t)
    if not targets:
        print("没找到源文件")
        return 2

    all_plans = []
    for t in targets:
        recs, crlf, changes = plan(t)
        print("\n" + "=" * 76)
        print("扫描  %s" % t.name)
        print("=" * 76)
        agg = collections.Counter(k for _, k, _ in changes)
        print("  待改记录数: %d" % len(changes))
        for k, n in agg.most_common():
            print("     %-20s %d" % (k, n))
        keydetail = collections.Counter()
        for _, k, d in changes:
            for kk in d:
                keydetail[(k, kk)] += 1
        for (k, kk), n in keydetail.most_common(12):
            print("       · %s / %s  x%d" % (k, kk, n))
        all_plans.append((t, recs, crlf, changes))

    total = sum(len(c) for _, _, _, c in all_plans)
    print("\n合计待改 %d 条记录" % total)
    if not args.apply:
        print("\n（预演模式。加 --apply 执行。）")
        return 0

    stamp = time.strftime("%Y%m%d-%H%M%S")
    bdir = root.parent / ("_eprj3_meta_backup_%s_%s" % (root.name, stamp))
    bdir.mkdir(parents=True, exist_ok=True)
    print("\n备份到: %s" % bdir)

    for t, recs, crlf, changes in all_plans:
        if not changes:
            print("  跳过（无需清理）: %s" % t.name)
            continue
        src = contained(root, t)
        dst = contained(bdir, bdir / t.name)
        shutil.copy2(src, dst)

        idx_map = {i: (k, d) for i, k, d in changes}
        out_lines = []
        for i, (h, p, s, _) in enumerate(recs):
            if i in idx_map:
                kind, drops = idx_map[i]
                if kind == "META.attributes":
                    for k in drops:
                        p["attributes"].pop(k, None)
                elif kind == "COMPONENT.attrs":
                    for k in drops:
                        p["attrs"].pop(k, None)
                out_lines.append(render(h, p, s))
            else:
                out_lines.append(s)
        nl = "\r\n" if crlf else "\n"
        src.write_text(nl.join(out_lines) + nl, encoding="utf-8", newline="")
        print("  已清理 %-28s 改 %d 条  → 备份 %s" % (t.name, len(changes), dst.name))

    print("\n完成。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
