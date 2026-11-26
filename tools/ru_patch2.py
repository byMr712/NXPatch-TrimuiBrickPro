"""Universal NX Redux string-localization patcher (ET_EXEC, AArch64).

Relocates selected string literals into a code cave appended to the RX LOAD
segment and rewrites every reference to them:
  * ADRP+ADD pairs in .text (and extra code ranges, e.g. injected trampolines)
  * 8-byte absolute pointers in allocatable data sections
  * raw literals at explicit addresses (e.g. a copy inside injected code)

Usage: py ru_patch2.py <in.elf> <out.elf> <mapping.json>
"""

import json
import struct
import sys

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from elftools.elf.elffile import ELFFile
from elftools.elf.sections import Section

MAP = None


def align_up(n, a):
    return (n + a - 1) & ~(a - 1)


def main():
    src, dst, mapfile = sys.argv[1], sys.argv[2], sys.argv[3]
    cfg = json.load(open(mapfile, encoding='utf-8'))
    mapping = cfg['strings']
    raw_lits = cfg.get('raw', [])            # [{"addr": 0x..., "text": "..."}]
    extra_ranges = cfg.get('code_ranges', [])  # [[0x..., 0x...], ...]

    data = bytearray(open(src, 'rb').read())
    ef = ELFFile(open(src, 'rb'))

    e_phoff = ef.header['e_phoff']
    e_phentsize = ef.header['e_phentsize']
    e_phnum = ef.header['e_phnum']
    e_shoff = ef.header['e_shoff']
    e_shentsize = ef.header['e_shentsize']
    e_shnum = ef.header['e_shnum']

    phdrs = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        vals = struct.unpack_from('<IIQQQQQQ', data, off)
        phdrs.append(dict(off=off, type=vals[0], flags=vals[1], offset=vals[2], vaddr=vals[3],
                          paddr=vals[4], filesz=vals[5], memsz=vals[6], align=vals[7]))

    loads = [p for p in phdrs if p['type'] == 1]
    rx = next(p for p in loads if p['flags'] & 1)
    rw = next(p for p in loads if p['flags'] & 2 and p is not rx)

    ins_at = rx['offset'] + rx['filesz']
    cave_vaddr = rx['vaddr'] + rx['filesz']
    rw_page = rw['vaddr'] & ~0xFFF
    max_cave = rw_page - align_up(cave_vaddr, 0x1000)
    assert max_cave >= 0x2000, f'no room for a cave: {max_cave:#x}'
    max_cave = min(max_cave, 0xF000)

    text = ef.get_section_by_name('.text')
    ro = ef.get_section_by_name('.rodata')
    text_addr, text_data, text_off = text['sh_addr'], text.data(), text['sh_offset']
    ro_addr, ro_data = ro['sh_addr'], ro.data()
    ro_end = ro_addr + len(ro_data)

    rel = ef.get_section_by_name('.rela.dyn')
    if rel:
        for r in rel.iter_relocations():
            assert not (ro_addr <= r['r_offset'] < ro_end), 'relocation inside .rodata!'

    def cstr(vaddr):
        if not (ro_addr <= vaddr < ro_end):
            return None
        off = vaddr - ro_addr
        end = ro_data.find(b'\0', off)
        if end <= off:
            return None
        return ro_data[off:end].decode('latin1')

    def find_literals(s):
        pat = s.encode('utf-8')
        out, start = [], 0
        while True:
            i = ro_data.find(pat + b'\0', start)
            if i < 0:
                break
            if i == 0 or ro_data[i - 1] == 0:
                out.append(ro_addr + i)
            start = i + 1
        return out

    # ---- cave
    cave = bytearray()
    new_addr = {}

    def put(s):
        while len(cave) % 4:
            cave.append(0)
        v = cave_vaddr + len(cave)
        cave.extend(s.encode('utf-8') + b'\0')
        return v

    for en, ru in mapping.items():
        new_addr[en] = put(ru)
    raw_new = []
    for item in raw_lits:
        raw_new.append((int(item['addr'], 0) if isinstance(item['addr'], str) else item['addr'], put(item['text'])))

    assert len(cave) <= max_cave, f'cave overflow {len(cave)} > {max_cave}'
    insert_size = align_up(len(cave), 0x10000)
    # RX may only grow up to the page where the RW segment starts
    cave_ext = min(insert_size, rw_page - cave_vaddr)
    assert cave_vaddr + cave_ext >= cave_vaddr + len(cave), 'cave does not fit'

    # ---- decode code
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True
    md.skipdata = True
    ranges = [(text_addr, text_addr + len(text_data))]
    ranges += [(int(a, 0), int(b, 0)) for a, b in extra_ranges]

    def insns_of(a, b):
        if a == text_addr:
            return list(md.disasm(text_data, a))
        fo = None
        for p in loads:
            if p['vaddr'] <= a < p['vaddr'] + p['filesz']:
                fo = p['offset'] + (a - p['vaddr'])
                break
        if fo is None:
            return []
        return list(md.disasm(bytes(data[fo:fo + (b - a)]), a))

    pairs = {}   # target -> [(adrp_addr, [add_addr, ...])]
    ins_by_addr = {}
    for a, b in ranges:
        ins = insns_of(a, b)
        for x in ins:
            ins_by_addr[x.address] = x
        for n, i in enumerate(ins):
            if i.mnemonic != 'adrp':
                continue
            rd = i.operands[0].reg
            value = i.operands[1].imm
            adds = []
            # only the ADDs that immediately follow the ADRP belong to it
            for k in range(1, 4):
                if n + k >= len(ins):
                    break
                j = ins[n + k]
                if (j.mnemonic == 'add' and len(j.operands) == 3 and
                        j.operands[0].reg == rd and j.operands[1].reg == rd and
                        (not j.operands[2].shift or j.operands[2].shift.value == 0)):
                    value += j.operands[2].imm
                    adds.append(j.address)
                    continue
                break
            if adds:
                pairs.setdefault(value, []).append((i.address, list(adds)))

    def file_off(va):
        for p in loads:
            if p['vaddr'] <= va < p['vaddr'] + p['filesz']:
                return p['offset'] + (va - p['vaddr'])
        raise KeyError(hex(va))

    # ---- patch
    report = []

    def patch_site(site, new, tag):
        n_ptr = n_pair = 0
        # pointers
        pat = struct.pack('<Q', site)
        for sec in ef.iter_sections():
            if not isinstance(sec, Section) or not (sec['sh_flags'] & 0x2) or sec['sh_type'] == 8:
                continue
            if sec['sh_addr'] == text_addr:
                continue
            blob = sec.data()
            base = sec['sh_addr']
            start = 0
            while True:
                i = blob.find(pat, start)
                if i < 0:
                    break
                if (base + i) % 8 == 0:
                    struct.pack_into('<Q', data, sec['sh_offset'] + i, new)
                    n_ptr += 1
                start = i + 1
        # adrp+add
        for (pa, adds) in pairs.get(site, []):
            if len(adds) != 1:
                print(f'  !! skip chained ADRP {pa:#x} ({len(adds)} ADDs) for {tag!r}')
                continue
            aa = adds[0]
            ai = ins_by_addr[pa]
            add_ins = ins_by_addr[aa]
            rdname = ai.reg_name(ai.operands[0].reg)
            rd = 31 if rdname == 'sp' else int(rdname[1:])
            imm12 = add_ins.operands[2].imm
            shift = add_ins.operands[2].shift.value if add_ins.operands[2].shift else 0
            assert shift == 0 and 0 <= imm12 < 0x1000
            delta = (new >> 12) - (pa >> 12)
            assert -(1 << 20) <= delta < (1 << 20)
            new_adrp = 0x90000000 | ((delta & 3) << 29) | (((delta >> 2) & 0x7FFFF) << 5) | rd
            new_add = 0x91000000 | ((new & 0xFFF) << 10) | (rd << 5) | rd
            fo = file_off(pa)
            struct.pack_into('<I', data, fo, new_adrp)
            struct.pack_into('<I', data, fo + 4, new_add)
            n_pair += 1
        return n_ptr, n_pair

    missing = []
    for en, ru in mapping.items():
        lits = find_literals(en)
        if not lits:
            missing.append(en)
            continue
        tot_ptr = tot_pair = 0
        for site in lits:
            n_ptr, n_pair = patch_site(site, new_addr[en], en)
            tot_ptr += n_ptr
            tot_pair += n_pair
            report.append((en, ru, hex(site), hex(new_addr[en]), n_ptr, n_pair))
        if tot_ptr + tot_pair == 0:
            print(f'  !! no refs for {en!r}')

    for site, new in raw_new:
        n_ptr, n_pair = patch_site(site, new, hex(site))
        report.append((f'raw {site:#x}', '', hex(site), hex(new), n_ptr, n_pair))

    if missing:
        print('MISSING literals (not in .rodata):')
        for m in missing:
            print('   ', repr(m))

    # ---- insert
    block = bytes(cave) + b'\0' * (insert_size - len(cave))
    data[ins_at:ins_at] = block
    for p in phdrs:
        if p is rx:
            struct.pack_into('<QQ', data, p['off'] + 32, p['filesz'] + cave_ext, p['memsz'] + cave_ext)
        elif p['type'] != 0 and p['offset'] >= ins_at and p['filesz'] > 0:
            struct.pack_into('<Q', data, p['off'] + 8, p['offset'] + insert_size)

    new_shoff = e_shoff + insert_size
    struct.pack_into('<Q', data, 0x28, new_shoff)
    for i in range(e_shnum):
        off = new_shoff + i * e_shentsize
        sh_offset = struct.unpack_from('<Q', data, off + 24)[0]
        if sh_offset >= ins_at:
            struct.pack_into('<Q', data, off + 24, sh_offset + insert_size)

    open(dst, 'wb').write(data)
    print(f'patched -> {dst}  {len(data)} bytes  cave {len(cave)}/{max_cave}  ins {insert_size:#x}')
    for row in report:
        print('  {!r:42} -> {!r:40} {} -> {}  ptr={} pair={}'.format(*row))


if __name__ == '__main__':
    main()
