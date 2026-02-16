import struct, sys, os

sys.stdout.reconfigure(encoding='utf-8')

# Load dump path from .env file or command line
dump_path = None
if len(sys.argv) > 1:
    dump_path = sys.argv[1]
else:
    env_file = os.path.join(os.path.dirname(os.path.abspath(__file__)), '.env')
    if os.path.exists(env_file):
        with open(env_file) as ef:
            for line in ef:
                line = line.strip()
                if line.startswith('DUMP_PATH='):
                    dump_path = line.split('=', 1)[1].strip().strip('"').strip("'")
                    break
    if not dump_path:
        print("Usage: python analyze_dump.py <path_to_dmp>")
        print("   or set DUMP_PATH in .env")
        sys.exit(1)

with open(dump_path, 'rb') as f:
    f.seek(32)
    exception_rva = 0
    module_rva = 0
    for i in range(15):
        st, ds, rva = struct.unpack('<III', f.read(12))
        if st == 6: exception_rva = rva
        if st == 4: module_rva = rva

    f.seek(exception_rva)
    thread_id = struct.unpack('<I', f.read(4))[0]
    f.read(4)
    exc_code = struct.unpack('<I', f.read(4))[0]
    f.read(4)
    f.read(8)
    exc_addr = struct.unpack('<Q', f.read(8))[0]
    print("Exception 0x{:08x} at 0x{:016x} (thread {})".format(exc_code, exc_addr, thread_id))

    f.seek(module_rva)
    num_modules = struct.unpack('<I', f.read(4))[0]
    modules = []
    for m in range(num_modules):
        base = struct.unpack('<Q', f.read(8))[0]
        size = struct.unpack('<I', f.read(4))[0]
        f.read(4)
        f.read(4)
        name_rva = struct.unpack('<I', f.read(4))[0]
        f.read(84)  # VersionInfo(52) + CvRecord(8) + MiscRecord(8) + Reserved(16)
        cur = f.tell()
        try:
            f.seek(name_rva)
            nlen = struct.unpack('<I', f.read(4))[0]
            raw = f.read(nlen)
            name = raw.decode('utf-16-le', errors='replace')
        except:
            name = "<unknown>"
        f.seek(cur)
        modules.append((base, size, name))

    sep = os.sep
    for base, size, name in modules:
        if base <= exc_addr < base + size:
            offset = exc_addr - base
            short = name.split(sep)[-1] if sep in name else name
            print("Crash in: {} at offset 0x{:x}".format(short, offset))
            break

    print("\nCrash module:")
    for base, size, name in modules:
        if base <= exc_addr < base + size:
            short = name.split(sep)[-1] if sep in name else name
            print("  0x{:016x} +0x{:06x} {} (offset 0x{:x})".format(base, size, short, exc_addr - base))
            break

    print("\nAll modules:")
    for base, size, name in sorted(modules):
        short = name.split(sep)[-1] if sep in name else name
        print("  0x{:016x} +0x{:06x} {}".format(base, size, short))
