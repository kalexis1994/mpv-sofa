#ifndef WASM_ENDIAN_SHIM_H
#define WASM_ENDIAN_SHIM_H
/* Force-included before any libmysofa source. Pre-defines portable_endian.h's
 * include guard so its unsupported-platform body never compiles, and supplies
 * the byte-order macros directly. WebAssembly is always little-endian. */
#define PORTABLE_ENDIAN_H__
#include <stdint.h>
#define htobe16(x) __builtin_bswap16(x)
#define htole16(x) (x)
#define be16toh(x) __builtin_bswap16(x)
#define le16toh(x) (x)
#define htobe32(x) __builtin_bswap32(x)
#define htole32(x) (x)
#define be32toh(x) __builtin_bswap32(x)
#define le32toh(x) (x)
#define htobe64(x) __builtin_bswap64(x)
#define htole64(x) (x)
#define be64toh(x) __builtin_bswap64(x)
#define le64toh(x) (x)
#endif
