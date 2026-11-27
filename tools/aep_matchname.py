# An aep (RIFX) with the effect's match name changed: pjef (the effect's match name, variable length) and tdmn
# (stream match names "<effect>-00NN", fixed size, zero padded). The tree is parsed and written again (sizes
# recomputed). usage: python aep_matchname.py <in.aep> <out.aep> <old> <new> [--check]
import struct
import sys

CONTAINERS = {b'RIFX', b'LIST'}


def parse(b, pos, end):
    out = []
    while pos + 8 <= end:
        cid = b[pos:pos + 4]
        size = struct.unpack('>I', b[pos + 4:pos + 8])[0]
        data_start = pos + 8
        data_end = data_start + size
        if data_end > end:
            raise ValueError(f'chunk {cid} at {pos} overruns ({data_end} > {end})')
        if cid in CONTAINERS:
            kind = b[data_start:data_start + 4]
            # btdk (a LIST holding raw data, not chunks) is kept as raw
            if kind == b'btdk':
                out.append((cid, b[data_start:data_end]))
            else:
                out.append((cid, kind, parse(b, data_start + 4, data_end)))
        else:
            out.append((cid, b[data_start:data_end]))
        pos = data_end + (size & 1)
    if pos != end:
        trailing = b[pos:end]
        out.append((None, trailing))
    return out


def write(nodes):
    parts = []
    for n in nodes:
        if n[0] is None:
            parts.append(n[1])
            continue
        if len(n) == 3:
            body = n[1] + write(n[2])
        else:
            body = n[1]
        parts.append(n[0] + struct.pack('>I', len(body)) + body + (b'\0' if len(body) & 1 else b''))
    return b''.join(parts)


def rename(nodes, old, new, stats):
    res = []
    for n in nodes:
        if n[0] is None:
            res.append(n)
        elif len(n) == 3:
            res.append((n[0], n[1], rename(n[2], old, new, stats)))
        elif n[0] == b'pjef' and n[1].rstrip(b'\0') == old:
            stats['pjef'] += 1
            res.append((n[0], new + b'\0'))
        elif n[0] == b'tdmn' and (n[1].startswith(old + b'-') or n[1].rstrip(b'\0') == old):
            name = n[1].rstrip(b'\0')
            nn = new + name[len(old):]
            assert len(nn) < len(n[1])
            stats['tdmn'] += 1
            res.append((n[0], nn + b'\0' * (len(n[1]) - len(nn))))
        else:
            if old in n[1]:
                stats['other'] += 1
                stats.setdefault('where', []).append(n[0])
            res.append(n)
    return res


src, dst, old, new = sys.argv[1], sys.argv[2], sys.argv[3].encode(), sys.argv[4].encode()
b = open(src, 'rb').read()
# the RIFX chunk, then the XMP packet (not a chunk) kept as it is
rifxEnd = 8 + struct.unpack('>I', b[4:8])[0]
tree = parse(b, 0, rifxEnd) + [(None, b[rifxEnd:])]
again = write(tree)
if again != b:
    raise SystemExit(f'round trip differs ({len(again)} vs {len(b)}): not written')
stats = {'pjef': 0, 'tdmn': 0, 'other': 0}
t2 = rename(tree, old, new, stats)
out = write(t2)
if old in out:
    raise SystemExit(f'the old name is left in {stats}: not written')
if '--check' not in sys.argv:
    open(dst, 'wb').write(out)
print(src, stats)
