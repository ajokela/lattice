# Crystal-by-Reference Stage 6 benchmark results (LAT-459)

- Machine: Darwin 25.5.0 (arm64), Apple M3 Max, 64 GB RAM
- Commit: afcf0fd (Thu Jun 11 23:22:58 2026 -0500), plus the then-uncommitted Stage 6 benchmark suite in the working tree
- Best of 3 runs per cell; times are each benchmark's self-reported elapsed (Lattice `time()`, ms resolution).
- `shared` = normal crystal-by-reference; `force-copy` = `LATTICE_FORCE_COPY=1` differential oracle (every alias is a deep clone). Shared cells at 0–1 ms are at clock resolution, so those ratios are lower bounds.

## Main suite

| workload | backend | shared (ms) | force-copy (ms) | speedup |
|---|---|---:|---:|---:|
| read | stack-vm | 3 | 3 | 1.0x |
| read | tree-walk | 13571 | 27204 | 2.0x |
| read | regvm | 2 | 2 | 1.0x |
| alias | stack-vm | 1 | 1252 | 1252.0x |
| alias | tree-walk | 554 | 3429 | 6.2x |
| alias | regvm | 0 | 1256 | 1256.0x (lower bound) |
| arg-pass | stack-vm | 41 | 243 | 5.9x |
| arg-pass | tree-walk | 111 | 534 | 4.8x |
| arg-pass | regvm | 40 | 457 | 11.4x |
| channel | stack-vm | 12 | 110 | 9.2x |
| channel | tree-walk | 12 | 161 | 13.4x |
| channel | regvm | 11 | 159 | 14.5x |
| spawn | stack-vm | 2 | 6 | 3.0x |
| spawn | tree-walk | 9 | 1243 | 138.1x |
| spawn | regvm | 5 | 10 | 2.0x |
| freeze-loop | stack-vm | 270 | 255 | 0.9x |
| freeze-loop | tree-walk | 309 | 375 | 1.2x |
| freeze-loop | regvm | 314 | 318 | 1.0x |

## Threshold sweep (`value_worth_regionizing`, src/value.c)

20k cycles at 8 aliases / 5k cycles at 64 aliases; each cycle = clone fresh value + `fix` (freeze) + N aliased reads. String cutoff `REGION_SHARE_MIN_STR_LEN` = 32; arrays always regionize (no element-count cutoff exists).

### Strings (bytes)

| size | aliases | stack-vm shared | stack-vm force-copy | tree-walk shared | tree-walk force-copy | regvm shared | regvm force-copy |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 8 | 8 | 60 | 61 | 126 | 132 | 67 | 63 |
| 16 | 8 | 54 | 55 | 128 | 132 | 67 | 70 |
| 24 | 8 | 51 | 60 | 121 | 134 | 67 | 71 |
| 32 | 8 | 60 | 60 | 123 | 137 | 66 | 74 |
| 40 | 8 | 61 | 62 | 123 | 133 | 61 | 75 |
| 48 | 8 | 59 | 63 | 122 | 136 | 62 | 75 |
| 64 | 8 | 60 | 62 | 122 | 137 | 68 | 79 |
| 128 | 8 | 61 | 62 | 123 | 136 | 71 | 77 |
| 256 | 8 | 63 | 68 | 124 | 143 | 71 | 85 |
| 8 | 64 | 31 | 41 | 137 | 157 | 38 | 43 |
| 16 | 64 | 32 | 43 | 137 | 159 | 38 | 46 |
| 24 | 64 | 32 | 45 | 138 | 160 | 37 | 46 |
| 32 | 64 | 31 | 49 | 137 | 162 | 38 | 49 |
| 40 | 64 | 32 | 52 | 136 | 159 | 39 | 53 |
| 48 | 64 | 32 | 54 | 136 | 161 | 38 | 57 |
| 64 | 64 | 32 | 57 | 138 | 162 | 38 | 59 |
| 128 | 64 | 31 | 49 | 139 | 164 | 37 | 59 |
| 256 | 64 | 33 | 58 | 138 | 173 | 39 | 70 |

### Arrays (elements)

| size | aliases | stack-vm shared | stack-vm force-copy | tree-walk shared | tree-walk force-copy | regvm shared | regvm force-copy |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 8 | 59 | 67 | 120 | 133 | 68 | 74 |
| 1 | 8 | 54 | 71 | 118 | 135 | 69 | 77 |
| 2 | 8 | 57 | 75 | 120 | 138 | 69 | 81 |
| 4 | 8 | 63 | 79 | 120 | 138 | 71 | 85 |
| 8 | 8 | 62 | 79 | 125 | 155 | 73 | 100 |
| 16 | 8 | 65 | 100 | 128 | 169 | 75 | 119 |
| 32 | 8 | 72 | 137 | 133 | 203 | 84 | 175 |
| 64 | 8 | 84 | 207 | 146 | 275 | 99 | 286 |
| 0 | 64 | 32 | 47 | 132 | 156 | 37 | 52 |
| 1 | 64 | 32 | 51 | 132 | 162 | 37 | 60 |
| 2 | 64 | 32 | 53 | 133 | 163 | 36 | 64 |
| 4 | 64 | 33 | 63 | 133 | 171 | 38 | 72 |
| 8 | 64 | 33 | 75 | 134 | 188 | 38 | 99 |
| 16 | 64 | 33 | 101 | 135 | 214 | 38 | 136 |
| 32 | 64 | 35 | 162 | 137 | 272 | 42 | 224 |
| 64 | 64 | 37 | 281 | 138 | 394 | 46 | 404 |

