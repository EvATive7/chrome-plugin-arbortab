import struct, sys, pefile

# Build the symbol-server key from the raw CodeView (RSDS) record rather than pefile's
# Signature_String, whose meaning changed between pefile releases (newer ones already append
# the Age, so adding it again produced a 34-character key that the symbol server 404s on).
pe = pefile.PE(sys.argv[1], fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_DEBUG']])
for d in pe.DIRECTORY_ENTRY_DEBUG:
    raw = pe.get_data(d.struct.AddressOfRawData, d.struct.SizeOfData)
    if raw[:4] != b'RSDS':
        continue
    data1, data2, data3 = struct.unpack_from('<IHH', raw, 4)
    data4 = raw[12:20]
    age, = struct.unpack_from('<I', raw, 20)
    name = raw[24:].split(b'\0', 1)[0].decode(errors='replace')
    print(name, f'{data1:08X}{data2:04X}{data3:04X}{data4.hex().upper()}{age:X}')
    break
else:
    sys.exit('no CodeView (RSDS) debug record')
