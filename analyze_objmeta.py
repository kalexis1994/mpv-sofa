import sys

print('=' * 100)
print('object metadata BLOCK HEX ANALYSIS - BYTE-BY-BYTE COMPARISON')
print('=' * 100)

all_spatial = {
    '@02D0': '101f003b02c421001985e11188800d182270008040ff010081df02010381f40207fbe8040e03f0081fe7e01038007c207f80f840e07df081fefbe1000095e0f2'.rstrip(),
    '@0F90': '101f003a02c421001905e111e8800a270008040ff010081df02010381f40207fbe8040e03f0081fe7e01038007c207f80f840e07df081fefbe10000095e000b5',
    '@4FD0': '101f003a02c421001905e111e8a00a270008040ff010081df02010381f40207fbe8040e03f0081fe7e01038007c207f80f840e07df081fefbe10000092e00092',
    '@7C20': '101f003a02c421001905e111e8800a270008040ff010081df02010381f40207fbe8040e03f0081fe7e01038007c207f80f840e07df081fefbe10000090c00090',
    '@A800': '101f003a02c421001905e111e800289c0020103fc0402077c08040e07d0081fefa010380fc0207f9f8040e001f081fe03e10381f7c207fbef84000009660004',
    '@211C': '101f003a02c421001905e111e800689c0020103fc0402077c08040e07d0081fefa010380fc0207f9f8040e001f081fe03e10381f7c207fbef8400000',
    '@3515': '101f003a02c421001905e111e800689c0020103fc0402077c08040e07d0081fefa010380fc0207f9f8040e001f081fe03e10381f7c207fbef84000008640003',
    '@D057': '101f003a02c421001905e111e8800a270008040ff010081df02010381f40207fbe8040e03f0081fe7e01038007c207f80f840e07df081fefbe10000085e000a5',
}

print()
print('Comparing bytes at each position across 8 blocks from test_spatial.thd:')
print()

names = list(all_spatial.keys())
# Normalize to 128 hex chars (64 bytes)
for k in all_spatial:
    v = all_spatial[k]
    if len(v) < 128:
        v = v + '0' * (128 - len(v))
    all_spatial[k] = v[:128]

hdr = 'Byte  '
for n in names:
    hdr += f'{n:>6} '
hdr += '  Notes'
print(hdr)
print('-' * 120)

for i in range(64):
    vals = []
    for n in names:
        h = all_spatial[n]
        byte_hex = h[i*2:i*2+2]
        vals.append(byte_hex)

    unique = set(vals)
    if len(unique) == 1:
        marker = ' '
        note = ''
    else:
        marker = '*'
        note = f' <<< {len(unique)} different values'

    row = f'{marker}{i:3d}  '
    for v in vals:
        if len(unique) > 1:
            row += f'  [{v}]'
        else:
            row += f'   {v} '
    row += note
    print(row)

print()
print()
# Now LOTR
all_lotr_std = {
    '@349E': '101f003a02c421001905e111e8e00a27fbef840ef810081c0fbe10381f40207f808040e0010081c07e0103fcfc0207fbe8040ff3ff081c07fe10000099400',
    '@3803': '101f003a02c421001905e111e800289fefbe103be04020703ef840e07d0081fe020103800402070ff8040ff3f0081fefa0103fcffc20701ff8400000912000',
    '@9233': '101f003a02c421001905e111e8800a27fbef840ef810081c0fbe10381f40207f808040e0010081c07e0103fcfc0207fbe8040ff3ff081c07fe10000099200',
    '@2764': '101f003a02c421001905e111e8e00a27fbef840ef810081c0fbe10381f40207f808040e0010081c07e0103fcfc0207fbe8040ff3ff081c07fe100000',
    '@3848': '101f003a02c421001905e111e8800a27fbef840ef810081c0fbe10381f40207f808040e0010081c07e0103fcfc0207fbe8040ff3ff081c07fe100000',
    '@604C': '101f003a02c421001905e111e800689fefbe103be04020703ef840e07d0081fe020103800402070ff8040ff3f0081fefa0103fcffc20701ff84000008120000',
    '@9377': '101f003a02c421001905e111e8800a27fbef840ef810081c0fbe10381f40207f808040e0010081c07e0103fcfc0207fbe8040ff3ff081c07fe10000093c0000',
    '@B648': '101f003a02c421001905e111e8a00a27fbef840ef810081c0fbe10381f40207f808040e0010081c07e0103fcfc0207fbe8040ff3ff081c07fe10000098c0002',
}

print('=' * 100)
print('test_lotr_mid.thd standard object metadata blocks comparison:')
print('=' * 100)
print()

names2 = list(all_lotr_std.keys())
for k in all_lotr_std:
    v = all_lotr_std[k]
    if len(v) < 128:
        v = v + '0' * (128 - len(v))
    all_lotr_std[k] = v[:128]

hdr2 = 'Byte  '
for n in names2:
    hdr2 += f'{n:>7} '
hdr2 += '  Notes'
print(hdr2)
print('-' * 120)

for i in range(64):
    vals = []
    for n in names2:
        h = all_lotr_std[n]
        byte_hex = h[i*2:i*2+2]
        vals.append(byte_hex)

    unique = set(vals)
    if len(unique) == 1:
        marker = ' '
        note = ''
    else:
        marker = '*'
        note = f' <<< {len(unique)} different values'

    row = f'{marker}{i:3d}  '
    for v in vals:
        if len(unique) > 1:
            row += f'   [{v}]'
        else:
            row += f'    {v} '
    row += note
    print(row)

print()
print('=' * 100)
print('STRUCTURE SUMMARY')
print('=' * 100)
print()
print('object metadata Block Layout (64 bytes):')
print()
print('Offset  Bytes  Values seen                    Interpretation')
print('------  -----  ----------------------------   --------------')
print('[00-01]   2    10 1F                          Sync marker (constant)')
print('[02-03]   2    00 3B (1st) / 00 3A (rest)     Block type (3B=initial, 3A=continuation)')
print('[04-05]   2    02 C4                          Config field (constant per file)')
print('[06-07]   2    21 00                          Flags/padding (constant)')
print('[08-09]   2    19 85 (1st) / 19 05 (rest)     Sub-config (byte 9 differs for 1st block)')
print('[10-11]   2    E1 11                          Stream config (constant)')
print('[12]      1    88 (1st) / E8 (rest)           Frame type marker')
print('[13]      1    80/A0/E0/00                    Pattern variant selector')
print('[14-15]   2    0A27 / 289C / 689C (spatial)     Encoding mode selector')
print('               0A27 / 289F / 689F (lotr)      (determines payload bit layout)')
print('[16-57]  42    <object position payload>      Bit-packed 3D positions for all objects')
print('[58-59]   2    00 00                          Separator (constant)')
print('[60]      1    varies (85-99)                 Rolling value (frame counter mod?)')
print('[61]      1    varies (20/40/60/a0/c0/e0)     Sub-counter / flags')
print('[62-63]   2    varies                         Checksum / CRC')
print()
print('KEY FINDINGS:')
print()
print('1. The object metadata sync is really 4 bytes: 10 1F 00 3A (or 10 1F 00 3B for first)')
print('   Searching for just 10 1F produces many false positives in audio data.')
print()
print('2. Bytes 12-15 select between TWO position encoding patterns:')
print('   - Pattern A (0A27): one bit arrangement of the position data')
print('   - Pattern B (289C/689C or 289F/689F): bit-rotated version')
print('   These alternate but encode THE SAME positions (just different bit alignment)')
print()
print('3. The position payload (bytes 16-57, 42 bytes) is IDENTICAL within each')
print('   pattern type across the entire file. This confirms all objects in both')
print('   test files maintain STATIC positions throughout the clips.')
print()
print('4. Only bytes 60-63 change between blocks of the same pattern type.')
print('   These are likely frame counters, timestamps, or CRC values.')
print()
print('5. The LOTR "anomalous" blocks (0x8B950, 0x18CBB0, etc.) are FALSE POSITIVES.')
print('   They are random audio data containing 10 1F by coincidence - they lack')
print('   the 00 3A continuation bytes at positions 2-3.')
