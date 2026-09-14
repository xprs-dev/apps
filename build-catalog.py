#!/usr/bin/env python3
"""Build catalog.json, the list of apps published on https://xprs.dev/apps.

    ./build-catalog.py                # writes catalog.json
    ./build-catalog.py --check        # exits 1 if catalog.json is stale

The catalog is one entry per app, newest packaged version only, with the
text, icon, screenshots and the download that a store needs to show it.
binaries/index.json stays what it is (every version ever packaged, six
fields) and the in-app store keeps reading that one for now; this file is
for the page at xprs.dev/apps and for the store that will read it later.

Sources, per folder named in catalog.list:

    <name>/manifest.json               id, version, title, description, summary,
                                       icon, color, tags, platforms, permissions
    <name>/store/description.json      descriptions per language, changelog,
                                       screenshots (optional)
    <name>/store/screenshots/*.png     screenshots (optional), sorted by name;
                                       the caption is the file name without the
                                       numeric prefix and extension
    binaries/<name>/<name>-<ver>.wapp  the package for the manifest version;
                                       size and sha256 are read from it

The package must exist: a catalog that points at a file that is not there
is worse than no catalog. Run ./build-archive.sh <name> first.

Every path in the output is relative to "base", so the page and a store
resolve them the same way: base + file. Python 3 standard library only.
"""
import glob
import hashlib
import json
import os
import re
import sys
from datetime import datetime, timezone

ROOT = os.path.dirname(os.path.abspath(__file__))
LIST = os.path.join(ROOT, "catalog.list")
OUT = os.path.join(ROOT, "catalog.json")
BASE = "https://xprs.dev/apps/"
SOURCE = "https://github.com/xprs-dev/apps/tree/main/"
SCHEMA = "xprs.apps.catalog/1"
IMAGE_EXT = (".png", ".jpg", ".jpeg", ".webp")


def fail(msg):
    sys.stderr.write("build-catalog: " + msg + "\n")
    sys.exit(1)


def read_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def read_list(path):
    if not os.path.isfile(path):
        fail("no catalog.list beside this script")
    names = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if line:
                names.append(line)
    return names


def caption_of(filename):
    stem = os.path.splitext(os.path.basename(filename))[0]
    stem = re.sub(r"^\d+[-_ ]*", "", stem)
    return stem.replace("_", " ").replace("-", " ").strip() or stem


def screenshots_of(name, store):
    """store/screenshots/* on disk first, then any listed in
    description.json that is not already there. Paths relative to base."""
    seen = []
    out = []

    def add(rel, caption=None):
        if rel in seen:
            return
        seen.append(rel)
        out.append({"file": rel, "caption": caption or caption_of(rel)})

    shots_dir = os.path.join(ROOT, name, "store", "screenshots")
    for p in sorted(glob.glob(os.path.join(shots_dir, "*"))):
        if p.lower().endswith(IMAGE_EXT):
            add(name + "/store/screenshots/" + os.path.basename(p))
    for item in (store or {}).get("screenshots") or []:
        if isinstance(item, str):
            rel, cap = item, None
        elif isinstance(item, dict):
            rel, cap = item.get("file") or item.get("path"), item.get("caption")
        else:
            continue
        if not rel:
            continue
        rel = rel.lstrip("/")
        if not rel.startswith(name + "/"):
            rel = name + "/" + rel
        if not os.path.isfile(os.path.join(ROOT, rel)):
            fail("%s: screenshot listed in store/description.json is missing: %s"
                 % (name, rel))
        add(rel, cap)
    return out


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def entry_for(name):
    folder = os.path.join(ROOT, name)
    mpath = os.path.join(folder, "manifest.json")
    if not os.path.isfile(mpath):
        fail("%s: no manifest.json (is the name in catalog.list a folder here?)" % name)
    m = read_json(mpath)
    for key in ("id", "version", "title"):
        if not m.get(key):
            fail("%s: manifest.json has no %s" % (name, key))
    version = str(m["version"])

    pkg_rel = "binaries/%s/%s-%s.wapp" % (name, name, version)
    pkg = os.path.join(ROOT, pkg_rel)
    if not os.path.isfile(pkg):
        fail("%s: %s is not built; run ./build-archive.sh %s first" % (name, pkg_rel, name))

    store = None
    spath = os.path.join(folder, "store", "description.json")
    if os.path.isfile(spath):
        store = read_json(spath)

    descriptions = (store or {}).get("descriptions")
    if not isinstance(descriptions, dict) or not descriptions:
        descriptions = {"en": {
            "title": m["title"],
            "summary": m.get("description") or "",
            "body": m.get("summary") or "",
        }}

    icon = m.get("icon")
    if icon:
        icon = name + "/" + icon.lstrip("/")
        if not os.path.isfile(os.path.join(ROOT, icon)):
            fail("%s: icon named in manifest.json is missing: %s" % (name, icon))

    return {
        "name": name,
        "id": m["id"],
        "version": version,
        "kind": m.get("kind") or "app",
        "title": m["title"],
        "description": m.get("description") or "",
        "summary": m.get("summary") or "",
        "descriptions": descriptions,
        "icon": icon,
        "color": m.get("color"),
        "tags": list(m.get("tags") or []),
        "platforms": list(m.get("platforms") or []),
        "permissions": list(m.get("permissions") or []),
        "screenshots": screenshots_of(name, store),
        "file": pkg_rel,
        "size": os.path.getsize(pkg),
        "sha256": sha256_of(pkg),
        "changelog": (store or {}).get("changelog"),
        "source_url": SOURCE + name,
    }


def build():
    names = read_list(LIST)
    if not names:
        fail("catalog.list names no app")
    apps = [entry_for(n) for n in names]
    return {
        "schema": SCHEMA,
        "generated": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "base": BASE,
        "apps": apps,
    }


def same_apart_from_time(a, b):
    a = dict(a)
    b = dict(b)
    a.pop("generated", None)
    b.pop("generated", None)
    return a == b


def main(argv):
    doc = build()
    if "--check" in argv:
        if not os.path.isfile(OUT):
            fail("catalog.json does not exist; run ./build-catalog.py")
        if not same_apart_from_time(read_json(OUT), doc):
            fail("catalog.json is stale; run ./build-catalog.py")
        print("catalog.json is current (%d apps)" % len(doc["apps"]))
        return 0
    with open(OUT, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=2, ensure_ascii=False)
        f.write("\n")
    for app in doc["apps"]:
        print("  %-16s %-8s %s (%d bytes, %d screenshots)" % (
            app["name"], app["version"], app["file"], app["size"],
            len(app["screenshots"])))
    print("wrote catalog.json (%d apps)" % len(doc["apps"]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
