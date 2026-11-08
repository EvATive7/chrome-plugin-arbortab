import sys, pefile
pe = pefile.PE(sys.argv[1], fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_DEBUG']])
for d in pe.DIRECTORY_ENTRY_DEBUG:
    e = d.entry
    if hasattr(e, 'Signature_String'):
        print(e.PdbFileName.decode().strip('\0'), e.Signature_String + format(e.Age, 'X'))
