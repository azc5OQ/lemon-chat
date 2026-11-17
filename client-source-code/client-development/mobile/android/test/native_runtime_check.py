"""compare the packaged runtime with the verified SDK after symbol stripping"""
import hashlib
import struct
from pathlib import Path


def elf_image(data):
    if data[:4] != b'\x7fELF' or data[5] != 1 or data[4] not in (1, 2):
        raise AssertionError('expected a little endian ELF library')
    wide = data[4] == 2
    header = struct.unpack_from('<HHIQQQIHHHHHH' if wide else '<HHIIIIIHHHHHH', data, 16)
    machine = header[1]
    program_offset, section_offset = header[4:6]
    program_size, program_count, section_size, section_count, names_index = header[8:13]
    section_format = '<IIQQQQIIQQ' if wide else '<IIIIIIIIII'
    required_section_size = struct.calcsize(section_format)
    assert section_size == required_section_size and section_count > 0, 'invalid ELF section table'
    assert section_offset + section_size * section_count <= len(data), 'truncated ELF section table'
    assert names_index < section_count, 'invalid ELF section names'
    sections = [struct.unpack_from(section_format, data, section_offset + index * section_size) for index in range(section_count)]
    names = sections[names_index]
    strings = data[names[4]:names[4] + names[5]]
    allocated = {}
    content = {}
    for section in sections:
        name_offset, kind, flags, address, offset, size = section[:6]
        assert name_offset < len(strings), 'invalid ELF section name'
        terminator = strings.find(b'\0', name_offset)
        assert terminator >= 0, 'unterminated ELF section name'
        name = strings[name_offset:terminator].decode('ascii')
        if kind != 8:
            assert offset + size <= len(data), 'truncated ELF section ' + name
            raw = memoryview(data)[offset:offset + size]
            if name in ('.dynstr', '.symtab'):
                content[name] = bytes(raw)
        else:
            raw = b''
        if flags & 2:
            allocated[name] = (kind, flags, address, size, hashlib.sha256(raw).hexdigest())
    program_format = '<IIQQQQQQ' if wide else '<IIIIIIII'
    assert program_size == struct.calcsize(program_format), 'invalid ELF program table'
    assert program_offset + program_size * program_count <= len(data), 'truncated ELF program table'
    alignments = []
    for index in range(program_count):
        entry = struct.unpack_from(program_format, data, program_offset + index * program_size)
        if entry[0] == 1:
            alignments.append(entry[7])
    assert alignments, 'ELF library has no loadable segments'
    return machine, alignments, allocated, content


def verify_native_runtime(archive, apk, runtime_directory, abis):
    expected = {'lib/' + abi + '/' + library for abi in abis for library in ('libnode.so', 'liblemonchatnode.so', 'libc++_shared.so')}
    actual = {name for name in archive.namelist() if name.startswith('lib/') and name.endswith('.so')}
    assert actual == expected, 'APK native libraries do not match the selected ABIs'
    machines = {'arm64-v8a': 183, 'armeabi-v7a': 40, 'x86_64': 62}
    report = []
    with Path(apk).open('rb') as raw_apk:
        for abi in abis:
            for library in ('libnode.so', 'liblemonchatnode.so', 'libc++_shared.so'):
                name = 'lib/' + abi + '/' + library
                packaged = archive.read(name)
                machine, alignments, sections, content = elf_image(packaged)
                assert machine == machines[abi], 'wrong ELF machine in ' + name
                if abi != 'armeabi-v7a':
                    assert min(alignments) >= 16384, 'library is not aligned for 16 KiB pages: ' + name
                info = archive.getinfo(name)
                if info.compress_type == 0 and abi != 'armeabi-v7a':
                    raw_apk.seek(info.header_offset)
                    local_header = raw_apk.read(30)
                    name_size, extra_size = struct.unpack_from('<HH', local_header, 26)
                    assert (info.header_offset + 30 + name_size + extra_size) % 16384 == 0, 'APK library offset is not aligned for 16 KiB pages: ' + name
                if library == 'libnode.so':
                    source = (Path(runtime_directory) / 'jniLibs' / abi / library).read_bytes()
                    source_machine, source_alignments, source_sections, source_content = elf_image(source)
                    assert source_machine == machine and source_alignments == alignments, 'stripping changed runtime load layout'
                    assert source_sections == sections, 'stripping changed a runtime section or dynamic exports'
                    assert b'_ZN4node5StartEiPPc\0' in content['.dynstr'], 'node::Start is absent'
                    assert '.symtab' not in content, 'APK still includes the non-runtime symbol table'
                    assert len(packaged) <= len(source), 'packaged runtime unexpectedly grew'
                    report.append(abi + ': ' + str(len(source)) + ' -> ' + str(len(packaged)) + ' bytes')
    return report
