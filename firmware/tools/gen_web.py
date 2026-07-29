#!/usr/bin/env python3
# ============================================================
# gen_web.py — genere include/web_page.h depuis web/veilleuse-config.html
#
# Appele en pre-script PlatformIO avant chaque compilation. La page
# reste un vrai fichier editable (le designer itere dessus) ; ce script
# la gzippe et l'embarque en PROGMEM.
#
# Le header n'est reecrit que si son contenu change : editer main.cpp ne
# declenche donc pas une recompilation inutile. gzip mtime=0 pour que la
# sortie soit deterministe (sinon le header change a chaque build).
#
# Sortie : PAGE_GZ (PROGMEM), PAGE_GZ_LEN, PAGE_ETAG.
# ============================================================
import gzip
import hashlib
import io
import os
import sys

try:
    Import("env")  # noqa: F821  injecte par SCons quand appele par PlatformIO
    PROJECT = env["PROJECT_DIR"]  # noqa: F821
    FAIL = lambda: env.Exit(1)  # noqa: E731,F821
except NameError:
    PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    FAIL = lambda: sys.exit(1)  # noqa: E731

SRC = os.path.join(PROJECT, "web", "veilleuse-config.html")
DST = os.path.join(PROJECT, "include", "web_page.h")


def gz(data):
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb", compresslevel=9, mtime=0) as f:
        f.write(data)
    return buf.getvalue()


if not os.path.isfile(SRC):
    sys.stderr.write("gen_web: ERREUR, %s introuvable\n" % SRC)
    FAIL()

with open(SRC, "rb") as f:
    raw = f.read()

packed = gz(raw)
etag = hashlib.md5(packed).hexdigest()[:16]

lines = [
    "// ============================================================",
    "// GENERE PAR tools/gen_web.py — NE PAS EDITER A LA MAIN.",
    "// Source : web/veilleuse-config.html (%d o) -> gzip %d o (%.1f %%)"
    % (len(raw), len(packed), 100.0 * len(packed) / len(raw)),
    "// ============================================================",
    "#pragma once",
    "#include <stdint.h>",
    "#include <pgmspace.h>",
    "",
    '#define PAGE_ETAG   "%s"' % etag,
    "#define PAGE_GZ_LEN %d" % len(packed),
    "",
    "static const uint8_t PAGE_GZ[] PROGMEM = {",
]
for i in range(0, len(packed), 16):
    lines.append("  " + "".join("0x%02x," % b for b in packed[i:i + 16]))
lines.append("};")
blob = "\n".join(lines) + "\n"

old = None
if os.path.isfile(DST):
    with open(DST, "r") as f:
        old = f.read()

if old != blob:
    if not os.path.isdir(os.path.dirname(DST)):
        os.makedirs(os.path.dirname(DST))
    with open(DST, "w") as f:
        f.write(blob)
    print("gen_web: %d o -> %d o gzip (%.1f %%), etag %s"
          % (len(raw), len(packed), 100.0 * len(packed) / len(raw), etag))
else:
    print("gen_web: web_page.h a jour (%d o gzip)" % len(packed))
