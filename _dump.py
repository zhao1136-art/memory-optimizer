from pypdf import PdfReader
r = PdfReader(r'g:\内存优化\ncyh\report.pdf', strict=True)
root = r.trailer['/Root'].get_object()
# walk pages to get font/ToUnicode
for pgref in root['/Pages']['/Kids']:
    pg = pgref.get_object()
    res = pg['/Resources'].get_object() if '/Resources' in pg else {}
    fonts = res.get('/Font', {}).get_object() if '/Font' in res else {}
    for name, fref in fonts.items():
        f = fref.get_object()
        print("FONT", name, dict(f))
        if '/ToUnicode' in f:
            tu = f['/ToUnicode']
            data = tu.get_data()
            print("TOC length actual:", len(data))
            print("TOC data start:", repr(data[:500]))