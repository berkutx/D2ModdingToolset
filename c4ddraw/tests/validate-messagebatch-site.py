"""Offline PE/site audit. Never patches or loads the game; mutations stay in RAM.

The byte-comparison validator is derived from exactSites() in the current source.
It is not execution of the production install function or proof of thread ownership.
Direct branch scan checks all byte offsets, a conservative superset of instruction
boundaries, in executable sections. Indirect/runtime-generated transfers are unknown.
"""
import argparse
import hashlib
import json
import re
import struct
from datetime import datetime, timezone
from pathlib import Path

import capstone
import pefile

EXPECTED_EXE_SHA256 = '1375cdef09ec470ee64fe5693fb734d7c69fb215212311d997f792b258a642eb'
SEAM_BEGIN, SEAM_END = 0x562972, 0x562977


def parse_sites(source):
    match = re.search(r'bool exactSites\(\)\s*\{(.*?)\n\}', source, re.S)
    if not match:
        raise ValueError('Cannot identify exactSites body')
    body = match.group(1)
    sites = []
    for name in ('dispatch', 'update'):
        values = re.search(r'\b' + name + r'\[\]\s*=\s*\{([^}]+)\}', body)
        address = re.search(r'reinterpret_cast<void\*>\(0x([0-9a-fA-F]+)\),\s*' + name, body)
        if not values or not address:
            raise ValueError('Cannot parse literal site: ' + name)
        expected = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]+)', values.group(1)))
        sites.append((name, int(address.group(1), 16), expected))
    if sites[0][1] != 0x562967 or sites[0][2][:4] != bytes.fromhex('85c0740e'):
        raise ValueError('Zero-Peek TEST/JZ neighbour is not admitted')
    if sites[1][1] != 0x56299f:
        raise ValueError('Unexpected controller update site')
    return sites


def branch_candidates(pe, begin, end):
    """Find rel8/rel32 encodings targeting the range, allowing false positives."""
    result = []
    base = pe.OPTIONAL_HEADER.ImageBase
    for section in pe.sections:
        if not section.Characteristics & 0x20000000:
            continue
        data = section.get_data()
        va = base + section.VirtualAddress
        for offset, opcode in enumerate(data):
            length = delta = 0
            if opcode in (0xe8, 0xe9) and offset + 5 <= len(data):
                length = 5
                delta = struct.unpack_from('<i', data, offset + 1)[0]
            elif (opcode == 0xeb or 0x70 <= opcode <= 0x7f or 0xe0 <= opcode <= 0xe3) and offset + 2 <= len(data):
                length = 2
                delta = struct.unpack_from('<b', data, offset + 1)[0]
            elif opcode == 0x0f and offset + 6 <= len(data) and 0x80 <= data[offset + 1] <= 0x8f:
                length = 6
                delta = struct.unpack_from('<i', data, offset + 2)[0]
            if length:
                target = va + offset + length + delta
                if begin <= target < end:
                    result.append({'source': hex(va + offset), 'target': hex(target),
                                   'bytes': data[offset:offset + length].hex()})
    return result


def audit(exe, source):
    exe_bytes = exe.read_bytes()
    source_bytes = source.read_bytes()
    pe = pefile.PE(data=exe_bytes)
    base = pe.OPTIONAL_HEADER.ImageBase
    if pe.FILE_HEADER.Machine != 0x14c or base != 0x400000:
        raise ValueError('Expected the preserved x86 EXE at preferred image base')
    sites = parse_sites(source_bytes.decode('utf-8-sig'))
    def read(va, size):
        return pe.get_data(va - base, size)
    originals = {name: read(va, len(expected)) for name, va, expected in sites}
    def admitted(blocks):
        return all(blocks[name] == expected for name, _, expected in sites)
    site_results = [{'name': name, 'address': hex(va), 'length': len(expected),
                     'expected': expected.hex(), 'actual': originals[name].hex(),
                     'match': originals[name] == expected} for name, va, expected in sites]
    mutations = []
    # Mutate every admitted byte, not only the five overwritten seam bytes.
    for name, va, expected in sites:
        for index in range(len(expected)):
            changed = bytearray(originals[name])
            changed[index] ^= 1
            blocks = dict(originals)
            blocks[name] = bytes(changed)
            mutations.append({'name': name, 'address': hex(va + index),
                              'rejected': not admitted(blocks)})
    disassembler = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    instructions = [{'address': hex(i.address), 'bytes': i.bytes.hex(),
                     'mnemonic': i.mnemonic, 'operands': i.op_str}
                    for i in disassembler.disasm(read(0x562967, 0x42), 0x562967)]
    inbound = branch_candidates(pe, SEAM_BEGIN, SEAM_END)
    interior = [b for b in inbound if int(b['target'], 16) != SEAM_BEGIN]
    old_hazard = branch_candidates(pe, 0x562978, 0x56297c)
    sha256 = hashlib.sha256(exe_bytes).hexdigest()
    valid = (sha256 == EXPECTED_EXE_SHA256 and admitted(originals) and
             read(SEAM_BEGIN, 5) == bytes.fromhex('8d44241450') and
             all(m['rejected'] for m in mutations) and not interior and
             any(b['source'] == '0x562969' and b['target'] == '0x562979' for b in old_hazard))
    return {'result': 'PASS' if valid else 'FAIL', 'utc': datetime.now(timezone.utc).isoformat(),
            'exe': str(exe.resolve()), 'exe_sha256': sha256, 'exe_sha256_expected': EXPECTED_EXE_SHA256,
            'source': str(source.resolve()), 'source_sha256': hashlib.sha256(source_bytes).hexdigest(),
            'sites': site_results, 'seam': {'begin': hex(SEAM_BEGIN), 'end_exclusive': hex(SEAM_END),
                                         'bytes': read(SEAM_BEGIN, 5).hex()},
            'mutation_tests': mutations, 'mutation_count': len(mutations),
            'direct_branch_candidates_to_seam': inbound, 'direct_branch_candidates_to_interior': interior,
            'old_562977_patch_hazard': old_hazard, 'instructions': instructions,
            'limits': ['All game/source inputs read only; mutations are in-memory byte copies.',
                       'exactSites comparator model parsed from source; actual installation is not invoked.',
                       'No proof of indirect/runtime-generated branch targets or arbitrary mod hook compatibility.',
                       'Current-thread-only Detours install requires the supported native owner-UI-thread contract.',
                       'No live memory, network, thread suspend/enrollment or game process operation.']}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = audit(args.exe, args.source)
    with args.output.open('x', encoding='utf-8') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')
    print(json.dumps({k: result[k] for k in ('result', 'exe_sha256', 'sites', 'mutation_count',
                                           'direct_branch_candidates_to_interior')}, indent=2))
    raise SystemExit(0 if result['result'] == 'PASS' else 1)
