import sys, traceback
from pypdf import PdfReader
p = sys.argv[1]
try:
    r = PdfReader(p, strict=True)
    print("PAGES", len(r.pages))
    for i, pg in enumerate(r.pages):
        try:
            t = pg.extract_text()
            print(f"--- page{i} text len={len(t or '')!r}")
            if t:
                print(repr(t[:200]))
        except Exception as ex:
            print(f"page{i} extract ERR: {type(ex).__name__}: {ex}")
    print("OK: reader opened successfully")
except Exception as ex:
    print("FATAL:", type(ex).__name__, str(ex))
    traceback.print_exc()