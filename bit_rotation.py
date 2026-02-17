# Check if Pattern A and Pattern B are bit-rotations of each other

def hex_to_bits(h):
    return bin(int(h, 16))[2:].zfill(len(h)*4)

# Spatial patterns (bytes 16-57, 42 bytes each)
spatial_A = '0008040ff010081df0201038' + '1f40207fbe8040e03f0081fe' + '7e01038007c207f80f840e07' + 'df081fefbe10'
spatial_B = '0020103fc0402077c08040e0' + '7d0081fefa010380fc0207f9' + 'f8040e001f081fe03e10381f' + '7c207fbef840'

bA = hex_to_bits(spatial_A)
bB = hex_to_bits(spatial_B)

print(f'Pattern A ({len(bA)} bits): {bA}')
print(f'Pattern B ({len(bB)} bits): {bB}')
print()

# Check left-shift rotations
print('Testing bit rotations (left-shift of Pattern A vs Pattern B):')
best_shift = 0
best_match = 0
for shift in range(1, len(bA)):
    rotated = bA[shift:] + bA[:shift]
    match = sum(1 for a, b in zip(rotated, bB) if a == b)
    if match > best_match:
        best_match = match
        best_shift = shift
    if match > len(bA) * 0.9:
        print(f'  Shift {shift}: {match}/{len(bA)} match ({100*match/len(bA):.1f}%%)')

print(f'  Best: shift={best_shift}, match={best_match}/{len(bA)} ({100*best_match/len(bA):.1f}%%)')
print()

# Also check if Pattern B = Pattern A << 2 (since 0A->28 and 27->9C look like shifts)
# 0A = 00001010, 28 = 00101000 -> left shift by 2!
# 27 = 00100111, 9C = 10011100 -> left shift by 2!
print('Checking: is byte14-15 also a 2-bit left shift?')
print(f'  0A = {bin(0x0A)[2:].zfill(8)}, 28 = {bin(0x28)[2:].zfill(8)}  -> shift 2: {bin((0x0A << 2) & 0xFF)[2:].zfill(8)} = {hex((0x0A << 2) & 0xFF)}')
print(f'  27 = {bin(0x27)[2:].zfill(8)}, 9C = {bin(0x9C)[2:].zfill(8)}  -> shift 2: {bin((0x27 << 2) & 0xFF)[2:].zfill(8)} = {hex((0x27 << 2) & 0xFF)}')
print()

# Check byte 13 patterns: 80->00 (shift by 1 or mask)
# 80=10000000, A0=10100000, E0=11100000, 00=00000000

# Let us check if the ENTIRE region bytes 13-57 is shifted by 2 bits
spatial_full_A = '800a27' + spatial_A  # bytes 13-57
spatial_full_B = '00289c' + spatial_B  # bytes 13-57
# Actually: bytes 13,14,15,16,...,57

bFA = hex_to_bits(spatial_full_A)
bFB = hex_to_bits(spatial_full_B)

print(f'Extended Pattern A (bytes 13-57, {len(bFA)} bits): {bFA[:80]}...')
print(f'Extended Pattern B (bytes 13-57, {len(bFB)} bits): {bFB[:80]}...')
print()

best_shift2 = 0
best_match2 = 0
for shift in range(1, 20):
    rotated = bFA[shift:] + bFA[:shift]
    match = sum(1 for a, b in zip(rotated, bFB) if a == b)
    pct = 100 * match / len(bFA)
    if match > best_match2:
        best_match2 = match
        best_shift2 = shift
    if pct > 80:
        print(f'  Extended shift {shift}: {match}/{len(bFA)} match ({pct:.1f}%%)')

print(f'  Best extended: shift={best_shift2}, match={best_match2}/{len(bFA)} ({100*best_match2/len(bFA):.1f}%%)')
print()

# Let me also check specifically shift=2
shift = 2
rotated = bFA[shift:] + bFA[:shift]
match = sum(1 for a, b in zip(rotated, bFB) if a == b)
print(f'Shift=2 check on bytes 13-57:')
print(f'  {match}/{len(bFA)} match ({100*match/len(bFA):.1f}%%)')
print(f'  A shifted: {rotated[:80]}...')
print(f'  B actual:  {bFB[:80]}...')
diff_positions = [i for i, (a, b) in enumerate(zip(rotated, bFB)) if a != b]
print(f'  Differences at bit positions: {diff_positions}')
print()

# Do the same for LOTR
lotr_A = 'fbef840ef810081c0fbe10381f40207f808040e0010081c07e0103fcfc0207fbe8040ff3ff081c07fe10'
lotr_B = 'efbe103be04020703ef840e07d0081fe020103800402070ff8040ff3f0081fefa0103fcffc20701ff840'

bLA = hex_to_bits(lotr_A)
bLB = hex_to_bits(lotr_B)

# Include bytes 13-15 for LOTR too
# Pattern A: byte13=80/e0/a0, bytes14-15=0a27 -> take typical: e00a27
# Pattern B: byte13=00, bytes14-15=289f -> 00289f
lotr_full_A = '800a27' + lotr_A  # typical variant
lotr_full_B = '00289f' + lotr_B

bLFA = hex_to_bits(lotr_full_A)
bLFB = hex_to_bits(lotr_full_B)

print('LOTR extended patterns (bytes 13-57):')
for shift in [1,2,3,4]:
    rotated = bLFA[shift:] + bLFA[:shift]
    match = sum(1 for a, b in zip(rotated, bLFB) if a == b)
    pct = 100 * match / len(bLFA)
    print(f'  Shift {shift}: {match}/{len(bLFA)} match ({pct:.1f}%%)')

shift = 2
rotated = bLFA[shift:] + bLFA[:shift]
diff_positions = [i for i, (a, b) in enumerate(zip(rotated, bLFB)) if a != b]
print(f'  Shift=2 diff positions: {diff_positions}')
print()

# Now check if byte 13 acts as a sub-bit offset
# 80=bit7, A0=bits 7+5, E0=bits 7+6+5, 00=none
# This might indicate: byte 13 encodes additional shift bits
# 80 = shift 0 within the sub-pattern
# A0 = shift + something
# E0 = shift + something else
# 00 = different base shift

print('Byte 13 values observed:')
print('  Spatial: 80, A0, 00 (with 0A27) and 00 (with 289C/689C)')
print('  LOTR:  80, A0, E0 (with 0A27) and 00 (with 289F/689F)')
print()
print('Byte 13 in binary:')
for v in [0x00, 0x80, 0xA0, 0xE0]:
    print(f'  0x{v:02X} = {bin(v)[2:].zfill(8)}')
