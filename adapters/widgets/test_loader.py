import os
"""Check real manifests and execute the linked native parser adapter."""
import ctypes
import sys
import json
import struct
import subprocess
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_INTR
import unicorn.arm64_const as R

HERE = Path(__file__).resolve().parent
SAMPLE = b'''<Widget><Var name="w" expression="#view_width"/>
<VariableBinders><ContentProviderBinder uri="content://test"/></VariableBinders>
<Group><Rectangle w="#w" h="#view_height" fillColor="#FF000000"/>
<Text color="#FF000000" text="Live"><Triggers><Trigger action="up"><Command target="test"/></Trigger></Triggers></Text>
</Group></Widget>'''


def semantic(node):
    return (node.tag, sorted(node.attrib.items()), (node.text or '').strip(),
            [semantic(child) for child in node])


with tempfile.TemporaryDirectory(prefix='hyperglass-xml-') as directory:
    library = Path(directory) / ('loader.dylib'if sys.platform=='darwin'else'loader.so')
    subprocess.run(['clang', '-O2', '-DHOST_TEST', '-fPIC', '-dynamiclib'if sys.platform=='darwin'else'-shared',
                    str(HERE / 'loader.c'), '-o', str(library)], check=True)
    native = ctypes.CDLL(str(library))
    native.style_xml.argtypes = [ctypes.c_char_p, ctypes.c_size_t,
                                 ctypes.POINTER(ctypes.c_size_t)]
    native.style_xml.restype = ctypes.c_void_p
    native.free.argtypes = [ctypes.c_void_p]

    def transform(data):
        length = ctypes.c_size_t()
        result = native.style_xml(data, len(data), ctypes.byref(length))
        if not result:
            return None
        try:
            return ctypes.string_at(result, length.value)
        finally:
            native.free(result)

    result = transform(SAMPLE)
    tree = ET.fromstring(result)
    assert tree.find('Layer/MaterialToken/GlassConfig').get('rustPassWindow') == 'true'
    assert tree.find('.//Rectangle').get('visibility') == '0'
    assert tree.find('.//Text').get('color') == '#EFFFFFFF'
    assert tree.find('VariableBinders/ContentProviderBinder').get('uri') == 'content://test'
    assert tree.find('.//Command').get('target') == 'test'
    photo = ET.fromstring(transform(b'<Widget><Image srcExp="@fallbackUrl" w="#view_width" h="#view_height"/>'
                                    b'<Image srcExp="@srcForDisplay" srcType="Uri" w="#view_width" h="#view_height" visibility="#img1Vis" alpha="#img1Alpha"/>'
                                    b'<Image srcExp="@cover" srcType="Uri" w="#view_width" h="#view_height"/></Widget>'))
    plate, picture, cover = photo.findall('.//Image')
    assert plate.get('visibility') == '0' and picture.get('visibility') == '#img1Vis'
    assert picture.get('alpha') == '(#img1Alpha)*0.6' and cover.get('alpha') == '(255)*0.6' and plate.get('alpha') is None
    for fake in [b'<!-- <Rectangle w="1000" h="1000"/> -->',
                 b'<![CDATA[<Rectangle w="1000" h="1000"/>]]>']:
        assert transform(b'<Widget>' + fake + b'<Text text="Clock"/></Widget>') is None
        styled = transform(SAMPLE.replace(b'<Group>', b'<Group>' + fake))
        assert fake in styled
        ET.fromstring(styled)
    dense = SAMPLE.replace(b'</Group>', b'<Rectangle/>' * 12000 + b'</Group>')
    assert len(ET.fromstring(transform(dense)).findall('.//Rectangle')) == 12001
    assert transform(b'x' * 2000001) is None
    changed = skipped = 0
    for fixture in sorted((HERE.parents[1]/'fixtures/widgets').glob('*.xml')):
        data = fixture.read_bytes()
        output = transform(data)
        if output is None:
            skipped += 1
            continue
        before, after = ET.fromstring(data), ET.fromstring(output)
        assert after.find('Layer/MaterialToken/GlassConfig') is not None
        for tag in ['Var', 'VariableBinders', 'Functions', 'Command',
                    'IntentCommand', 'Trigger', 'ExternalCommands']:
            assert sorted(json.dumps(semantic(n)) for n in before.iter(tag)) == sorted(json.dumps(semantic(n)) for n in after.iter(tag)), (fixture.name, tag)
        for node in after.iter('Text'):
            assert node.get('color') == '#EFFFFFFF'
        changed += 1
    assert changed >= 5 and skipped >= 1, (changed, skipped)
    print(f'PASS: {changed} original card manifests, {skipped} floating layouts, provider/actions, comments, CDATA and dense output')

# Execute the real ARM64 hook, syscall scope, malloc/free and original prologue.
linked = ELFFile(open(HERE / 'loader.elf', 'rb'))
symbols = {s.name: s['st_value'] for s in linked.get_section_by_name('.symtab').iter_symbols()}
source = ELFFile(open(Path(os.environ.get('HYPERGLASS_REFERENCE_DIR',HERE.parents[1]/'fixtures/reference')) / 'launcher-801027722/librust_maml_sdk.so', 'rb'))
registers = [getattr(R, f'UC_ARM64_REG_X{i}') for i in range(31)]


def raw(address, length):
    for segment in source.iter_segments():
        if segment['p_type'] == 'PT_LOAD' and segment['p_vaddr'] <= address and address + length <= segment['p_vaddr'] + segment['p_filesz']:
            source.stream.seek(segment['p_offset'] + address - segment['p_vaddr'])
            return source.stream.read(length)
    raise AssertionError(hex(address))


for process, data, allocate in [('com.miui.home', SAMPLE, True),
                                ('com.miui.home:other', SAMPLE, True),
                                ('com.miui.home', b'<Widget><Text text="Clock"/></Widget>', True),
                                ('com.miui.home', SAMPLE, False)]:
    emulator = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    pages = set()

    def put(address, content):
        for page in range(address & -4096, (address + len(content) + 4095) & -4096, 4096):
            if page not in pages:
                emulator.mem_map(page, 4096)
                pages.add(page)
        emulator.mem_write(address, content)

    for section in linked.iter_sections():
        if section['sh_flags'] & 2 and section['sh_size']:
            put(section['sh_addr'], section.data())
    for address in [0x649d10, 0x6499f0, 0x5182ec, 0x700000]:
        put(address, bytes.fromhex('c0035fd6'))
    put(0x5e7230, raw(0x5e7230, 28))
    put(0x1000000, data + b'\0')
    put(0x1100000, b'\0' * 0x10000)
    put(0x1200000, b'\0' * 0x10000)
    emulator.reg_write(R.UC_ARM64_REG_SP, 0x110f000)
    for index, register in enumerate(registers):
        emulator.reg_write(register, 0x900000 + index * 16)
    emulator.reg_write(registers[0], 0x120f000)
    emulator.reg_write(registers[1], 0x1000000)
    emulator.reg_write(registers[2], len(data))
    emulator.reg_write(registers[30], 0x700000)
    saved = [emulator.reg_read(register) for register in registers]
    calls, parsed = [], []

    def code(u, address, size, unused):
        if address == 0x649d10:
            calls.append('malloc')
            u.reg_write(registers[0], 0x1200000 if allocate else 0)
            u.reg_write(R.UC_ARM64_REG_PC, u.reg_read(registers[30]))
        elif address == 0x6499f0:
            assert u.reg_read(registers[0]) == 0x1200000
            calls.append('free')
            u.reg_write(R.UC_ARM64_REG_PC, u.reg_read(registers[30]))
        elif address == 0x5182ec:
            assert u.reg_read(registers[0]) == 0x120f000
            parsed.append(bytes(u.mem_read(u.reg_read(registers[1]), u.reg_read(registers[2]))))
            stack = u.reg_read(R.UC_ARM64_REG_SP)
            frame, ret = struct.unpack('<QQ', u.mem_read(stack, 16))
            u.reg_write(registers[29], frame)
            u.reg_write(R.UC_ARM64_REG_SP, stack + 96)
            u.reg_write(R.UC_ARM64_REG_PC, ret)

    def syscall(u, interrupt, unused):
        number = u.reg_read(registers[8])
        if number == 56:
            assert bytes(u.mem_read(u.reg_read(registers[1]), 19)).startswith(b'/proc/self/cmdline')
            value = 9
        elif number == 63:
            content = process.encode() + b'\0'
            u.mem_write(u.reg_read(registers[1]), content)
            value = len(content)
        else:
            assert number == 57
            value = 0
        u.reg_write(registers[0], value)

    emulator.hook_add(UC_HOOK_CODE, code)
    emulator.hook_add(UC_HOOK_INTR, syscall)
    emulator.emu_start(symbols['parse_hook'], 0x700000, count=2000000)
    assert emulator.reg_read(R.UC_ARM64_REG_PC) == 0x700000
    assert emulator.reg_read(R.UC_ARM64_REG_SP) == 0x110f000
    assert all(emulator.reg_read(registers[i]) == saved[i] for i in range(19, 30))
    assert len(parsed) == 1
    changed = process == 'com.miui.home' and data == SAMPLE and allocate
    assert (b'MaterialToken' in parsed[0]) == changed
    if not changed:
        assert parsed[0] == data
    assert ('free' in calls) == changed
print('PASS: linked ARM64 ABI, process scope, allocation failure, parser prologue and synchronous buffer lifetime')
