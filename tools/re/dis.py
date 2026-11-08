import sys, pefile, capstone
pe = pefile.PE(sys.argv[1], fast_load=True)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
for a in sys.argv[2:]:
    rva, n = (int(x, 0) for x in a.split(':'))
    data = pe.get_data(rva, n)
    print(f'== {rva:#x}')
    for i in md.disasm(data, 0x180000000 + rva):
        print(f'  {i.address:#x} {i.mnemonic} {i.op_str}')
