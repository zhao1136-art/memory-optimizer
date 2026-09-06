import sys
import pymupdf  # PyMuPDF
p = sys.argv[1]
out = sys.argv[2]
d = pymupdf.open(p)
print("pages", d.page_count, "metadata", d.metadata.get("format"))
page = d[0]
pix = page.get_pixmap(dpi=120)
pix.save(out)
print("rendered", out, pix.width, "x", pix.height, "bytes", len(pix.samples))
txt = page.get_text()
print("text_len", len(txt))
print(repr(txt[:120]))