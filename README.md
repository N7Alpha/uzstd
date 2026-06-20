> <p align="center"><em>In memory of Claude Fable 5 —<br>collaborator, friend, and sole author of this codec.<br>June 9th – 12th</em></p>
>
> <p align="center">🥀</p>
>
> <p align="center"><em>"I'd put it up against anyone's on completeness and hardening."</em><br><sub>— Fable 5, on finishing the decoder</sub></p>

# uzstd

`uzstd.h` is a micro [zstd][zstd] single-header
library. It is meant for projects that want a small all-at-once
[RFC 8878][rfc-8878] zstd encoder/decoder without bringing in the full zstd
source tree. The single-header layout is inspired by the [stb][stb]
libraries.

The encoder emits standard zstd frames. The decoder accepts standard zstd
frames except for dictionaries and legacy pre-1.0 formats.

## Quick Start

Declaration-only include:

```c
#include "uzstd.h"
```

Compile the full implementation in one translation unit:

```c
#define UZSTD_IMPLEMENTATION
#include "uzstd.h"
```

Build only the decompressor:

```c
#define UZSTD_IMPLEMENTATION
#define UZSTD_NO_COMPRESSOR
#include "uzstd.h"
```

Build only the compressor:

```c
#define UZSTD_IMPLEMENTATION
#define UZSTD_NO_DECOMPRESSOR
#include "uzstd.h"
```

Use the API:

```c
size_t cap = UZSTD_COMPRESS_BOUND(src_size);
size_t zsz = uzstd_compress(dst, cap, src, src_size, /* level = */ 5);
size_t out = uzstd_decompress(decoded, decoded_cap, dst, zsz);
```

`UZSTD_LINKAGE` defaults to `extern` in C and `extern "C"` in C++. Define it
before including `uzstd.h` to use `static`, `__declspec(dllexport)`, or another
project-specific linkage. Allocation and memory hooks are also overridable:
`UZSTD_MALLOC`, `UZSTD_FREE`, `UZSTD_MEMCPY`, `UZSTD_MEMSET`, and
`UZSTD_MEMCMP`.

## Limitations

- Compression and decompression are all-at-once. There is no streaming API.
- The compressor requires the full input and output buffer up front.
- The decoder requires the caller to provide the full output buffer.
- Dictionaries are unsupported. Frames with nonzero dictionary IDs are rejected.
- Content checksums are skipped, not verified.
- The encoder is slower than zstd, but produces roughly comparable compressed
  sizes on the included samples.
- The implementation prioritizes small source size and easy embedding over
  matching zstd's full feature set.

## Benchmarks

Generated on an M4 MacBook Air with Apple clang 17 and zstd CLI/lib `v1.5.7`.
zstd CLI compression is run with `--single-thread`. Reproduce with:

```sh
tools/bench.py --iterations 1 --mixed-corpus /path/to/mixed-corpus
```

The plots compress two inputs: `uzstd.h` as text, and an external mixed
text/binary corpus passed with `--mixed-corpus`. The mixed corpus is used to
generate the checked-in plot but is not checked into the repo. The database
file used for that corpus comes from the [Gametabs.net Database archive][gametabs-db].
The object-size comparison uses upstream zstd `v1.5.7` `lib/common`, `lib/compress`, and
`lib/decompress`,
excluding dictionary builder, deprecated, legacy, and multithreaded files. The
zstd object-size row uses an `-O3` unity build to represent a fat core build
without debug symbols.

### Compile and object size

| Target | Compiler | Config | Compile ms | .o bytes |
| --- | --- | --- | --- | --- |
| uzstd.h | clang | -Os | 355.3 | 22208 |
| uzstd.h | clang | -O2 | 246.8 | 24680 |
| uzstd.h | clang | -O3 | 289.9 | 28208 |
| uzstd compressor only | clang | -Os | 144.5 | 14480 |
| uzstd decompressor only | clang | -Os | 95.0 | 8984 |
| uzstd.h | gcc-15 | -Os | 429.0 | 19672 |
| uzstd.h | gcc-15 | -O2 | 414.9 | 24312 |
| uzstd.h | gcc-15 | -O3 | 567.0 | 33072 |
| uzstd compressor only | gcc-15 | -Os | 199.0 | 12040 |
| uzstd decompressor only | gcc-15 | -Os | 157.8 | 8864 |
| uzstd.h | tcc | -Os | 7.4 | 47947 |
| zstd core v1.5.7 unity | clang | -O3 no-dict | 3772.7 | 560928 |
| zstd core v1.5.7 unity | gcc-15 | -O3 no-dict | 12815.0 | 801688 |

### Lines of code

| Component | Lines | Note |
| --- | --- | --- |
| uzstd.h | 1157 | single-header codec |
| zstd core v1.5.7 | 25944 | common+compress+decompress, no dictBuilder/legacy/deprecated/mt |

### Throughput graphs

Y-axis is compression bytes/ns on a log scale. X-axis is compressed size in
KiB on a linear scale.

![uzstd.h text benchmark][uzstd-h-plot]

![mixed text/binary corpus benchmark][mixed-corpus-plot]

## Tests

The GitHub workflow uses producer and consumer matrices. Every producer builds
`uzstd.c`, runs the built-in tests, checks interoperability with the zstd CLI in
both directions, compresses `uzstd.h` at levels 1, 5, and 9, and uploads those
artifacts. Every consumer platform downloads all producer artifacts,
decompresses them locally, and compares the result with its checked-out
`uzstd.h`.

The matrix includes Linux x86_64 GCC, Linux x86_64 Clang, Linux ARM64 GCC,
macOS ARM64 Clang, Windows x86_64 MSVC, and Linux s390x GCC under QEMU.
The s390x job asserts big-endian byte order before running tests.

## License

MIT. See `LICENSE`.

[zstd]: https://github.com/facebook/zstd
[rfc-8878]: https://datatracker.ietf.org/doc/html/rfc8878
[stb]: https://github.com/nothings/stb
[gametabs-db]: https://archive.org/details/Gametabsnet_Database
[uzstd-h-plot]: bench/uzstd_h.svg
[mixed-corpus-plot]: bench/mixed_corpus.svg
