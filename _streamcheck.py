import sys, re
data = open(sys.argv[1],"rb").read()
# find each stream: /Length N >> stream
for m in re.finditer(rb'<<\s*/Length\s+(\d+)\s*>>\s*stream\r?\n', data):
    L = int(m.group(1))
    s = m.end()
    mark = b'\r\nendstream' if data[s:s+2]==b'\r\n' else b'\nendstream'
    e = data.find(mark, s)
    if e < 0:
        e = data.find(b'endstream', s)
        actual = e - s
        note = 'find first endstream'
    else:
        actual = e - s
        note = 'found \nendstream'
    print("Length=",L," actual=",actual," match=",L==actual," @",s, note)
    # print first 40 bytes
    head = data[s:s+40]
    print("   head:", head[:40])