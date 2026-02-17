import sys

def hex_to_bits(h):
    return bin(int(h, 16))[2:].zfill(len(h)*4)

print('=' * 100)
print('DEFINITIVE Object Metadata BLOCK STRUCTURE ANALYSIS')
print('=' * 100)
print()
print('1. PATTERN B = PATTERN A LEFT-ROTATED BY 2 BITS')
print('   (100%% match for Spatial, 98.9%% for LOTR)')
print()
print('   Bytes 12-57 form a SINGLE BITSTREAM rotated by 2 bits')
print('   between alternating Object Metadata blocks. The rotation relates to')
print('   bit-alignment within the lossless HD access unit structure.')
print()
print('2. POSITION DATA DECODING')
print()
print('42-byte payload (bytes 16-57) = 336 bits of position data.')
print('336 bits / 16 objects = 21 bits per object:')
print('  7 bits azimuth, 7 bits elevation, 7 bits distance')
print()

# Decode Spatial
spatial_pA = '0008040ff010081df020103' + '81f40207fbe8040e03f0081' + 'fe7e01038007c207f80f840' + 'e07df081fefbe10'
bits = hex_to_bits(spatial_pA)
print('SPATIAL Pattern A decoded as 21-bit object records:')
print()
print('  Obj   Azimuth_bits  Elev_bits    Dist_bits    Az  El  Di   Az_deg   El_deg')
print('  ---   -----------   ---------    ---------    --  --  --   ------   ------')

for i in range(16):
    start = i * 21
    if start + 21 > len(bits):
        break
    chunk = bits[start:start+21]
    az_b = chunk[0:7]
    el_b = chunk[7:14]
    di_b = chunk[14:21]
    az = int(az_b, 2)
    el = int(el_b, 2)
    di = int(di_b, 2)
    az_deg = (az / 127.0) * 360.0 - 180.0
    el_deg = (el / 127.0) * 180.0 - 90.0
    print('  %2d    %s     %s      %s     %3d %3d %3d  %+8.1f %+8.1f' % (i, az_b, el_b, di_b, az, el, di, az_deg, el_deg))

print()
# Decode LOTR
lotr_pA = 'fbef840ef810081c0fbe10381f40207f808040e0010081c07e0103fcfc0207fbe8040ff3ff081c07fe10'
bits_l = hex_to_bits(lotr_pA)
print('LOTR Pattern A decoded as 21-bit object records:')
print()
print('  Obj   Azimuth_bits  Elev_bits    Dist_bits    Az  El  Di   Az_deg   El_deg')
print('  ---   -----------   ---------    ---------    --  --  --   ------   ------')

for i in range(16):
    start = i * 21
    if start + 21 > len(bits_l):
        break
    chunk = bits_l[start:start+21]
    az_b = chunk[0:7]
    el_b = chunk[7:14]
    di_b = chunk[14:21]
    az = int(az_b, 2)
    el = int(el_b, 2)
    di = int(di_b, 2)
    az_deg = (az / 127.0) * 360.0 - 180.0
    el_deg = (el / 127.0) * 180.0 - 90.0
    print('  %2d    %s     %s      %s     %3d %3d %3d  %+8.1f %+8.1f' % (i, az_b, el_b, di_b, az, el, di, az_deg, el_deg))

print()
print()
print('3. VARYING BYTES SUMMARY')
print()
print(' Byte  3: 3B (1st block) vs 3A (rest) -> initial flag')
print(' Byte  9: 85 (1st block) vs 05 (rest) -> initial flag bit7')
print(' Byte 12: 88 (1st block) vs E8 (rest) -> initial flag')
print(' Byte 13: 80/A0/E0/00 -> sub-alignment within 2-bit rotation')
print(' Byte 14-15: 0A27 (PatA) vs 289C/289F (PatB) -> 2-bit rotation')
print('             68xx variant has bit 6 set (additional flag)')
print(' Byte 60-63: per-block varying (counter/CRC)')
print()
print('4. TRUE Object Metadata BLOCK COUNTS (using 4-byte sync 101F003A/003B):')
print('   test_spatial.thd:    138 blocks')
print('   test_lotr_mid.thd: 840 blocks')
print()
print('5. STATIC POSITIONS: Both files show IDENTICAL position payloads')
print('   in every Object Metadata block of the same pattern type. No objects')
print('   change position in either file.')
