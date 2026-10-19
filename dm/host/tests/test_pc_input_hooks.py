"""PC input rewriter scope and optional local-NSO stub/word verification.
Use --nso PATH for read-only checks against one's own main NSO; no cache is written.
The host callback is a test double, not the C dispatcher or a game execution.
"""
import argparse
import ast
from pathlib import Path
import re
import struct
import sys
import unittest

SOURCE = Path(__file__).resolve().parents[2] / 'tools/patch_nso.py'
DIRECT = {0x38f578: 0x8f, 0x3e3c48: 0x90, 0xbb6f2c: 0x91,
          0xaecca4: 0x95, 0xbb59b4: 0x98, 0xc26e0c: 0xa4}
CUSTOM = {0xad8a54: (0x93, 0xad8a58), 0x456c5c: (0x94, 0x456d50),
          0x368d64: (0x96, 0x368e10), 0xabcca4: (0xa3, 0xabccd8),
          0x3e4760: (0xa5, None),
          0x3e2f6c: (0xa6, 0x3e2f80), 0xc065a4: (0xa7, 0xc066b0),
          0xc068f4: (0xa8, 0xc068f8), 0xc06908: (0xa8, 0xc0690c),
          0xc0691c: (0xa8, 0xc06920), 0xae5324: (0x9c, 0xae53d0),
          0xae950c: (0x97, 0xae9510), 0xb0d150: (0x92, 0xb0d168)}
OBS = {0xad75f0: (23, 0xd10583ff), 0xb88010: (24, 0xf81a0ffc),
       0xb84750: (25, 0xa9ba6ffc)}
IDS = set(DIRECT.values()) | {value[0] for value in CUSTOM.values()}
NSO = None


def registration(call, name):
    return (isinstance(call, ast.Call) and isinstance(call.func, ast.Attribute)
            and isinstance(call.func.value, ast.Name) and call.func.value.id == name
            and call.func.attr == 'append' and call.args
            and isinstance(call.args[0], ast.Tuple))


def selected(tree):
    blocks, observers = [], []
    for node in tree.body:
        if not isinstance(node, ast.If):
            continue
        ids = set()
        for call in ast.walk(node):
            if registration(call, 'gates'):
                number = call.args[0].elts[2]
                if isinstance(number, ast.Constant):
                    ids.add(number.value)
            elif registration(call, 'cuts'):
                word = call.args[0].elts[2]
                if isinstance(word, ast.Constant) and isinstance(word.value, str):
                    raw = bytes.fromhex(word.value)
                    if len(raw) == 4:
                        value = int.from_bytes(raw, 'little')
                        if value & 0xffe0001f == 0xd4000001:
                            ids.add((value >> 5) & 0xffff)
            elif registration(call, 'obs'):
                number = call.args[0].elts[1]
                if isinstance(number, ast.Constant) and number.value in (23, 24, 25):
                    observers.append(ast.If(test=node.test, body=[ast.Expr(value=call)], orelse=[]))
        if ids and ids <= IDS:
            blocks.append(node)
    return blocks, observers


def execute(nodes, env):
    module = ast.fix_missing_locations(ast.Module(body=nodes, type_ignores=[]))
    exec(compile(module, str(SOURCE), 'exec'), env)


def environment(tree, image, rule=True, private=True):
    names = {'B', 'STP_PRE', 'LDP_POST', 'MRS_TPIDRRO', 'ADRP', 'ADDI', 'LDRX',
             'ADDR', 'BLR', 'emit_key_addr', 'emit_slot_addr', 'emit_gate_call',
             'site_obs', 'site_gate', 'dis', 'has18', 'put_branch', 'here', 'emit', 'stat'}
    env = dict(struct=struct, IMG=bytearray(image), RULE_RUN=rule, PRIVATE_GATES=private,
               TEXT_END=len(image), kinds={}, windows=[], starts=list(OBS) + [0xad8a54],
               patch=bytearray(), gates=[], cuts=[], obs=[], fix=[], stats={},
               PATCH_OFF=0x3400000, KEY_PAGE_OFF=0x6000000, KEY_INDEX={'KGATE': 0})
    execute([node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name in names], env)
    return env


def read_nso(path):
    import lz4.block
    data = path.read_bytes()
    assert data[:4] == b'NSO0' and data[0x40:0x50].hex() == 'db0507f36b84200ac05f75d76ed5026f'
    segments = [struct.unpack_from('<III', data, 0x10 + 0x10 * i) for i in range(3)]
    flags = struct.unpack_from('<I', data, 0xc)[0]
    sizes = struct.unpack_from('<III', data, 0x60)
    image = bytearray(segments[2][1] + segments[2][2] + struct.unpack_from('<I', data, 0x3c)[0])
    for i, (offset, address, size) in enumerate(segments):
        part = data[offset:offset + sizes[i]]
        if flags >> i & 1:
            part = lz4.block.decompress(part, uncompressed_size=size)
        assert len(part) == size
        image[address:address + size] = part
    return image


class InputHooks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tree = ast.parse(SOURCE.read_text())
        cls.blocks, cls.observers = selected(cls.tree)

    def test_scope_and_disabled_profiles(self):
        self.assertEqual(len(self.blocks), 14)
        self.assertEqual(len(self.observers), 3)
        for rule, private in ((False, True), (True, False), (False, False)):
            env = environment(self.tree, b'', rule, private)
            execute(self.blocks + self.observers, env)
            self.assertEqual((env['cuts'], env['gates'], env['obs'], env['patch']), ([], [], [], b''))

    def test_startup_cuts_are_unchanged(self):
        assignment = next(node for node in self.tree.body if isinstance(node, ast.Assign)
                          and any(isinstance(name, ast.Name) and name.id == 'CUTS' for name in node.targets))
        cuts = ast.literal_eval(assignment.value)['db0507f36b84200ac05f75d76ed5026f']
        startup = [row[:3] for row in cuts if row[0] == 0x8019ec]
        self.assertEqual(startup, [(0x8019ec, '280f0034', 'e8feff34')])
        self.assertFalse({0x801bd8, 0x801a20, 0x8019c4} & {row[0] for row in cuts})

    def test_shared_grid_hover_dispatch(self):
        host = SOURCE.parents[1] / 'host'
        self.assertRegex((host / 'hle.h').read_text(), r'SVC_HOST_PC_GRID_HOVER\s*=\s*0xa3')
        self.assertRegex((host / 'hle_tact.c').read_text(),
                         r'case SVC_HOST_PC_GRID_HOVER:\s*dm_ui_pc_grid_hover\(regs\);\s*return 1;')
        cursor = (host / 'dm_cursor.c').read_text()
        self.assertRegex(cursor, r'__attribute__\(\(weak\)\)\s+void dm_ui_pc_grid_hover\(uint64_t \*regs\)')
        self.assertRegex(cursor, r'void dm_ui_pc_grid_hover\(uint64_t \*regs\)\s*\{\s*if \(regs\) regs\[8\]=dm_ui_pc_skill_grid_hover\(regs\)!=0;\s*\}')

    def test_structural_verifier_union_counts_shared_a3_once(self):
        from types import SimpleNamespace
        tree = ast.parse((SOURCE.parents[1] / 'verify/structck.py').read_text())
        nodes = []
        for node in tree.body:
            if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id in ('pc_seen', 'pc_extra') for t in node.targets):
                nodes.append(node)
            elif isinstance(node, ast.For) and isinstance(node.target, ast.Name) and node.target.id == 'verifier' and any(isinstance(call, ast.Attribute) and call.attr == 'verify' for call in ast.walk(node)):
                nodes.append(node)
            elif isinstance(node, ast.Expr) and isinstance(node.value, ast.Call) and isinstance(node.value.func, ast.Attribute) and isinstance(node.value.func.value, ast.Name) and node.value.func.value.id == 'seen' and node.value.func.attr == 'update':
                nodes.append(node)
            elif isinstance(node, ast.Assign) and any(isinstance(t, ast.Subscript) and isinstance(t.value, ast.Name) and t.value.id == 'expected' and isinstance(t.slice, ast.Constant) and t.slice.value == 'KGATE' for t in node.targets):
                nodes.append(node)
        self.assertEqual(len(nodes), 5)
        errors = []
        modules = [SimpleNamespace(verify=lambda *args: {100, 200}),
                   SimpleNamespace(verify=lambda *args: {200, 300})]
        env = dict(_pc_verifiers=modules, seen={100}, mod='main', orig=b'', pat=b'',
                   h={}, gates={}, cuts=[], expected={}, gsites=range(4), obs=range(5),
                   nbranch=2, bad=errors.append)
        execute(nodes, env)
        self.assertEqual(env['seen'], {100, 200, 300})
        self.assertEqual(env['expected']['KGATE'], 13)
        self.assertEqual(errors, [])
        def reject(*args):
            raise ValueError('Corrupt A3')
        modules[1].verify = reject
        execute(nodes, dict(env, seen={100}))
        self.assertEqual(errors, ['Corrupt A3'])

    def test_structural_verifier_rejects_corruption(self):
        import importlib.util
        from types import SimpleNamespace
        helper = SOURCE.parents[1] / 'verify/pc_input_struct.py'
        spec = importlib.util.spec_from_file_location('pc_input_struct', helper)
        verifier = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(verifier)
        self.assertEqual(verifier.verify('sdk', b'', b'', {}, {}, []), set())
        if NSO is None:
            self.skipTest('Pass --nso PATH for generated stub corruption checks')
        original = read_nso(NSO)
        env = environment(self.tree, original)
        execute(self.blocks, env)
        for site, number in DIRECT.items():
            env['put_branch'](site, env['site_gate'](SimpleNamespace(address=site), 'svc', number))
        image = env['IMG']
        image.extend(bytes(env['PATCH_OFF'] + len(env['patch']) - len(image)))
        image[env['PATCH_OFF']:] = env['patch']
        gates = {ret: (kind, number, site) for ret, kind, number, site in env['gates']}
        cuts = [(site, bytes.fromhex(old), bytes.fromhex(new), why)
                for site, old, new, why, scope in env['cuts']]
        header = {'PATCH_OFF': env['PATCH_OFF'], 'KEY_PAGE_OFF': env['KEY_PAGE_OFF'],
                  'KEY_KGATE_IDX': 0, 'TEXT_END': len(original)}
        verify = lambda: verifier.verify('main', original, image, header, gates, cuts)
        self.assertEqual(verify(), set(gates))
        mutations = 0
        for ret, (kind, number, site) in gates.items():
            instruction = struct.unpack_from('<I', image, site)[0]
            immediate = instruction & 0x3ffffff
            start = site + 4 * ((immediate ^ 0x2000000) - 0x2000000)
            end = ret + (8 if site in DIRECT or number == 0xa5 else 40 if number == 0xa8 else 28)
            for address in range(start, end, 4):
                image[address] ^= 1
                with self.assertRaises(ValueError, msg=hex(address)):
                    verify()
                image[address] ^= 1
                mutations += 1
            original[site] ^= 1
            with self.assertRaises(ValueError):
                verify()
            original[site] ^= 1
        for ret in list(gates):
            saved = gates.pop(ret)
            with self.assertRaises(ValueError):
                verify()
            gates[ret] = saved
        print(f'PASS: structural verifier rejects {mutations} stub-word corruptions, every preimage and missing gate row')

    def test_original_preimages_and_mutations(self):
        if NSO is None:
            self.skipTest('Pass --nso PATH for local preimage and stub checks')
        image = read_nso(NSO)
        env = environment(self.tree, image)
        execute(self.blocks + self.observers, env)
        direct = {off: (int.from_bytes(bytes.fromhex(new), 'little') >> 5) & 0xffff
                  for off, old, new, *rest in env['cuts'] if off in DIRECT}
        self.assertEqual(direct, DIRECT)
        actual = {row[3]: row[2] for row in env['gates']}
        self.assertEqual(actual, {off: value[0] for off, value in CUSTOM.items()})
        self.assertEqual({off: (number, int(word, 16)) for off, number, word, why in env['obs']}, OBS)
        for off, old, new, *rest in env['cuts']:
            self.assertEqual(image[off:off + 4], bytes.fromhex(old))
        for off, (number, word) in OBS.items():
            self.assertEqual(struct.unpack_from('<I', image, off)[0], word)
        for site in list(CUSTOM) + [0xc267c8, 0xc26e08]:
            matched = 0
            for block in self.blocks:
                good = environment(self.tree, image)
                execute([block], good)
                if not (any(row[3] == site for row in good['gates']) or site in (0xc267c8, 0xc26e08)
                        and any(row[0] == 0xc26e0c for row in good['cuts'])):
                    continue
                changed = bytearray(image)
                changed[site] ^= 1
                with self.assertRaises(AssertionError):
                    execute([block], environment(self.tree, changed))
                matched += 1
            self.assertGreater(matched, 0)
        hover_block = next(block for block in self.blocks
                           if any(row[2:] == (0xa3, 0xabcca4)
                                  for row in self.execute_block(block, image)['gates']))
        for address in (0xabcc50, 0xabcc88, 0xabcca8, 0xabccd4, 0xabccd8):
            changed = bytearray(image)
            changed[address] ^= 1
            with self.assertRaises(AssertionError):
                execute([hover_block], environment(self.tree, changed))
        self.env, self.image = env, image
        from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
        env.update(md=Cs(CS_ARCH_ARM64, CS_MODE_ARM), re=re)
        for off, (number, word) in OBS.items():
            env['put_branch'](off, env['site_obs'](off, number, word))
        self.check_stubs(env, image)

    def execute_block(self, block, image):
        env = environment(self.tree, image)
        execute([block], env)
        return env

    def check_stubs(self, env, image):
        from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
        from unicorn import arm64_const as r
        patch, checks = bytes(env['patch']), 0
        sites = dict(CUSTOM)
        sites.update({off: (number, off + 4) for off, (number, word) in OBS.items()})
        entries = {off: struct.unpack_from('<I', env['IMG'], off)[0] for off in sites}
        for base in (0, 0x7000000000):
            for site, (number, target) in sites.items():
                for accept in ((0,) if number in (0x93, 0x97, 0xa5) else (0, 1)):
                    values = [0x111000 + i * 16 for i in range(31)]
                    values[29], values[30] = 0x700f200, 0x900f000
                    sp, key, tls, host = 0x700f000, base + env['KEY_PAGE_OFF'], 0x8000100, 0x9100000
                    destination = values[30] if number == 0xa5 else base + (target if accept else site + 4)
                    if number == 0xa8 and not accept:
                        destination = base + 0xc08cc8
                    machine = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
                    pages = {address & ~4095 for address in (base + site, destination, key, tls, host)}
                    pages.add(values[23] & ~4095)
                    pages.update(range(0x7000000, 0x7010000, 4096))
                    pages.update(range(base + env['PATCH_OFF'], base + env['PATCH_OFF'] + len(patch) + 4095, 4096))
                    for page in pages:
                        machine.mem_map(page, 4096)
                    machine.mem_write(base + env['PATCH_OFF'], patch)
                    machine.mem_write(base + site, struct.pack('<I', entries[site]))
                    machine.mem_write(key, struct.pack('<Q', 0x20))
                    machine.mem_write(tls + 0x20, struct.pack('<Q', host))
                    machine.mem_write(sp + 0x84, struct.pack('<I', 2))
                    for i, value in enumerate(values):
                        machine.reg_write(getattr(r, f'UC_ARM64_REG_X{i}'), value)
                    vectors = [(i + 1) * 0x112233445566778899 for i in range(32)]
                    for i, value in enumerate(vectors):
                        machine.reg_write(getattr(r, f'UC_ARM64_REG_Q{i}'), value)
                    machine.reg_write(r.UC_ARM64_REG_SP, sp)
                    machine.reg_write(r.UC_ARM64_REG_NZCV, 0xa0000000)
                    machine.reg_write(r.UC_ARM64_REG_TPIDRRO_EL0, tls)
                    seen = []
                    def hook(cpu, address, size, unused):
                        if address == host:
                            seen.append(address)
                            if site in OBS:
                                if accept:
                                    cpu.reg_write(r.UC_ARM64_REG_X1 if number == 23 else r.UC_ARM64_REG_X0, 0x123456)
                            elif number != 0xa5:
                                cpu.reg_write(r.UC_ARM64_REG_X8, accept)
                                if number == 0xa6 and accept:
                                    cpu.reg_write(r.UC_ARM64_REG_X0, 77)
                                    cpu.reg_write(r.UC_ARM64_REG_X21, 395)
                            cpu.reg_write(r.UC_ARM64_REG_PC, cpu.reg_read(r.UC_ARM64_REG_LR))
                        elif address == destination:
                            cpu.emu_stop()
                    machine.hook_add(UC_HOOK_CODE, hook)
                    machine.emu_start(base + site, 0xfffffffffffffff0, count=1000)
                    self.assertEqual(machine.reg_read(r.UC_ARM64_REG_PC), destination)
                    self.assertEqual(len(seen), 1)
                    self.assertEqual(machine.reg_read(r.UC_ARM64_REG_NZCV), 0xa0000000)
                    self.assertEqual(machine.reg_read(r.UC_ARM64_REG_X29), values[29])
                    state = lambda: ([machine.reg_read(getattr(r, f'UC_ARM64_REG_X{i}')) for i in range(31)],
                                     [machine.reg_read(getattr(r, f'UC_ARM64_REG_Q{i}')) for i in range(32)],
                                     machine.reg_read(r.UC_ARM64_REG_SP), machine.reg_read(r.UC_ARM64_REG_NZCV))
                    actual = state()
                    if not accept or site in OBS:
                        baseline_values = values[:]
                        if site in OBS and accept:
                            baseline_values[1 if number == 23 else 0] = 0x123456
                        for i, value in enumerate(baseline_values):
                            machine.reg_write(getattr(r, f'UC_ARM64_REG_X{i}'), value)
                        for i, value in enumerate(vectors):
                            machine.reg_write(getattr(r, f'UC_ARM64_REG_Q{i}'), value)
                        machine.reg_write(r.UC_ARM64_REG_SP, sp)
                        machine.reg_write(r.UC_ARM64_REG_NZCV, 0xa0000000)
                        machine.mem_write(base + site, bytes(image[site:site + 4]))
                        machine.ctl_remove_cache(base + site, base + site + 4)
                        stop = values[30] if number == 0xa5 else base + (0xc08cc8 if number == 0xa8 else site + 4)
                        machine.mem_write(sp + 0x84, struct.pack('<I', 2))
                        def native_stop(cpu, address, size, unused):
                            if address == stop:
                                cpu.emu_stop()
                        machine.hook_add(UC_HOOK_CODE, native_stop)
                        machine.emu_start(base + site, 0xfffffffffffffff0, count=10)
                        self.assertEqual(actual, state(), (hex(site), 'original instruction replay'))
                    else:
                        expected = values[:]
                        if number == 0xa8:
                            expected[30] = base + site + 4
                        if number == 0xa6:
                            expected[0], expected[21] = 77, 395
                        self.assertEqual(actual, (expected, vectors, sp, 0xa0000000), hex(site))
                    if number == 0xa8:
                        self.assertEqual(actual[0][30], base + site + 4)
                    checks += 1
        print(f'PASS: {checks} stub routes at two ASLR bases; host decision doubled, no game execution')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument('--nso', type=Path)
    options, remaining = parser.parse_known_args()
    NSO = options.nso
    unittest.main(argv=[sys.argv[0], *remaining])
