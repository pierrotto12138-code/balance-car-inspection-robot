"""清除嘉立创EDA专业版某个工程的 V3 物化缓存。

背景（见 docs/design_hardware/参考01工程现状.md 第七轮）：

EDA Pro 打开 `.eprj2` 时会把它转成 V3 日志格式并缓存到
    %LOCALAPPDATA%\\LCEDA-Pro\\cache.x64.3\\Service Worker\\CacheStorage\\<store>\\
缓存键名 = project_<sha256(工程绝对路径 utf-8)>_v6 与 localProject_<...>_v1。
之后每次打开都直接读这份缓存，**所以改 .eprj2 文件不生效**。

用法（务必先完全退出 EDA）：
    python clear_eda_cache.py --project "<仓库根>\\hardware\\参考01.eprj2"
    python clear_eda_cache.py --project "..." --list      # 只看不删
    python clear_eda_cache.py --project "..." --restore    # 从最近备份还原

只做文件系统操作，不连接数据库。
"""
import argparse
import hashlib
import os
import shutil
import sys
import time

STORE_ROOT = os.path.join(
    os.environ.get("LOCALAPPDATA", ""),
    "LCEDA-Pro", "cache.x64.3", "Service Worker", "CacheStorage",
)


def cache_keys(project_path):
    """返回该工程对应的两条缓存键名前缀。"""
    abspath = os.path.abspath(project_path)
    digest = hashlib.sha256(abspath.encode("utf-8")).hexdigest()
    return digest, ("project_%s_v6" % digest, "localProject_%s_v1" % digest)


def is_eda_running():
    """粗查 EDA 是否在运行（只看进程名，不杀进程）。"""
    try:
        import subprocess
        raw = subprocess.run(["tasklist"], capture_output=True, timeout=30).stdout
    except Exception:
        return None
    text = raw.decode("utf-8", "replace").lower()
    if "lceda" not in text:
        # 中文 Windows 的 tasklist 输出是 GBK
        text = raw.decode("gbk", "replace").lower()
    return "lceda-pro.exe" in text or "lceda-pro" in text


def find_entries(digest):
    """在 CacheStorage 下找出属于该工程的条目目录与分片文件。"""
    found = []
    if not os.path.isdir(STORE_ROOT):
        return found
    for store in os.listdir(STORE_ROOT):
        sdir = os.path.join(STORE_ROOT, store)
        if not os.path.isdir(sdir):
            continue
        for entry in os.listdir(sdir):
            edir = os.path.join(sdir, entry)
            if not os.path.isdir(edir):
                continue
            hits = []
            for root, dirs, files in os.walk(edir):
                for f in files:
                    if digest in f:
                        hits.append(os.path.join(root, f))
            if hits:
                found.append((edir, hits))
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--project", required=True, help="工程文件绝对路径（.eprj2/.eprj3）")
    ap.add_argument("--list", action="store_true", help="只列出，不删除")
    ap.add_argument("--restore", action="store_true", help="从最近一次备份还原")
    ap.add_argument("--backup-root", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "_cache_backup"))
    args = ap.parse_args()

    project = os.path.abspath(args.project)
    if not os.path.exists(project):
        print("工程不存在: %s" % project)
        return 2

    digest, keys = cache_keys(project)
    print("工程   : %s" % project)
    print("sha256 : %s" % digest)
    print("缓存键 : %s" % ", ".join(keys))
    print("缓存根 : %s" % STORE_ROOT)
    print("EDA    : %s" % ("运行中 ⚠ 请先完全退出" if is_eda_running() else "未运行 ✓"))
    print()

    if args.restore:
        stamp_dirs = []
        if os.path.isdir(args.backup_root):
            for d in os.listdir(args.backup_root):
                full = os.path.join(args.backup_root, d)
                if os.path.isdir(full) and d.startswith(digest[:12]):
                    stamp_dirs.append(full)
        if not stamp_dirs:
            print("没有找到该工程的备份（%s）" % args.backup_root)
            return 1
        src = sorted(stamp_dirs)[-1]
        entries = find_entries(digest)
        if entries:
            print("目标缓存仍在，先删除再还原会更干净：")
            for edir, _ in entries:
                print("   %s" % edir)
        for name in os.listdir(src):
            sp = os.path.join(src, name)
            if os.path.isdir(sp):
                store = os.path.basename(os.path.dirname(entries[0][0])) if entries else None
                if store:
                    dst = os.path.join(STORE_ROOT, store, name)
                    if os.path.exists(dst):
                        shutil.rmtree(dst)
                    shutil.copytree(sp, dst)
                    print("已还原 %s -> %s" % (name, store))
        return 0

    entries = find_entries(digest)
    if not entries:
        print("未找到该工程的缓存条目（可能已清除，或从未打开过）")
        return 0

    total = 0
    for edir, hits in entries:
        size = sum(os.path.getsize(h) for h in hits if os.path.exists(h))
        total += size
        print("命中条目: %s" % edir)
        print("           %d 个分片, %.1f KB" % (len(hits), size / 1024.0))
    print("\n合计 %.1f KB" % (total / 1024.0))

    if args.list:
        print("\n（--list 模式，未删除）")
        return 0

    if is_eda_running():
        print("\n⚠ EDA 仍在运行，缓存会被立刻重建。请先在 EDA 里完全关闭，再执行本脚本。")
        return 1

    stamp = time.strftime("%Y%m%d-%H%M%S")
    bdir = os.path.join(args.backup_root, "%s_%s" % (digest[:12], stamp))
    os.makedirs(bdir, exist_ok=True)
    for edir, _ in entries:
        shutil.copytree(edir, os.path.join(bdir, os.path.basename(edir)))
    print("\n已备份到: %s" % bdir)

    for edir, _ in entries:
        shutil.rmtree(edir)
        print("已删除  : %s" % edir)

    print("\n完成。现在可以打开 EDA，工程将从干净的源文件冷启动物化。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
