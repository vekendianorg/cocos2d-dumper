# c2d — native ELF/DWARF dumper

A from-scratch C++20 ELF/DWARF dumper for large Android shared objects. It walks 20,454,580 DIEs across 1183 compilation units in a 583 MB binary
and emits a C# type dump, falling back to an explicitly-labelled inferred mode
when a binary is stripped of DWARF.

Inputs used for development and validation:

| File | What it is |
|---|---|
| `libcocos2dcpp_1.74.2.so` | 583 MB ELF64 AArch64 shared object with 500 MB of DWARF; 1183 units, 20,454,580 DIEs |
| `com.fingersoft.hcr2-*.bin` | 34 MB memory dump of the same library, **stripped**: no `.debug_info`, and a section table full of dead pointers |
| `expected_dump_format_example.c` | formatting reference only |
| `dump_1.73.cs` | output of a commercial dumper on the previous version; the format this project reproduces |

Nothing is assumed to exist in a binary because it appears in a reference dump.

## Building

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

On Termux/PRoot, CMake's host detection calls `getprop`, which is unavailable,
so pass the system explicitly:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_VERSION=24
```

`scripts/build.sh` wraps this. Note that `/storage/emulated/0` (sdcardfs) does
not carry the executable bit, so copy the binaries to a real filesystem
(`cp build/c2d /tmp/c2d`) before running them.

## Usage

```sh
c2d info  [options] <elf>     # ELF + DWARF section/capability summary
c2d units [options] <elf>     # per-unit header + DIE count
c2d scan  [options] <elf>     # walk DIEs, count tags, measure throughput
c2d dump  [options] <elf>     # print one unit's DIE tree
```

Bounding options keep exploration cheap on the 583 MB input:

```sh
--max-units N      # stop after N units
--max-dies N       # stop after N DIEs overall
--first-unit N     # start at unit index N
--unit-stride N    # visit every Nth unit (bounded sampling)
--unit N           # unit index for `dump`
--tags             # print the tag histogram
--stats            # measured timings, RSS and counters
```

Examples:

```sh
c2d info  libcocos2dcpp_1.74.2.so
c2d scan  --max-units 20 --tags --stats libcocos2dcpp_1.74.2.so   # ~0.1 s
c2d scan  --unit-stride 100 --stats libcocos2dcpp_1.74.2.so       # ~0.1 s
c2d scan  --stats libcocos2dcpp_1.74.2.so                        # full scan
c2d dump  --unit 0 --max-print 40 libcocos2dcpp_1.74.2.so
c2d emit  --stats --name libcocos2dcpp_1.74.2.so        # -> output/dump.cs
c2d emit  --stats -o /tmp/small.cs libcocos2dcpp_1.74.2.so
```

The dump is written to `output/dump.cs` by default (that directory is
git-ignored, since a full dump is a few hundred MB). `emit` options:
`-o/--out`, `--name`, `--no-pad` (skip synthesised padding
fields), `--no-methods`, `--max-lines`, `--build-units`, `--list-conflicts N`
(show same-named types whose definitions disagreed).

## Testing

```sh
./build/tests/c2d-tests            # fast suite: synthetic fixtures only
ctest --test-dir build             # adds the opt-in real-binary test
```

The default suite is fast and never touches the 583 MB file: it builds synthetic
ELFs in memory (`tests/fixture_builder.*`) so that DWARF constructs the real
target does *not* contain — DWARF64, DWARF 5 headers, `DW_FORM_implicit_const`,
truncated and reserved headers — are still covered. Real-binary tests are skipped
automatically when the input is absent and are registered as a separate CTest
target so they are never run by accident.

## Two modes

`c2d emit` picks a mode automatically (`--mode=auto|dwarf|dwarfless` overrides).

**`dwarf` — ground truth.** Types, member offsets, enums, methods and
inheritance come from `.debug_info`. Output goes to `output/dump.cs`.

**`dwarfless` — inferred, and labelled as such.** For a binary with no DWARF,
function ranges still come from `.eh_frame` and class names from RTTI. Field
offsets and field types are *not* recoverable and are not guessed, so the output
says so three times: the filename is `dump.dwarfless.cs`, a banner at the top
of the file says `NOT GROUND TRUTH`, and every record carries a `TIER:` tag so
`grep -c 'TIER:infer'` sizes the problem. `--fail-on-low-confidence` exits 3
for CI.

A memory dump has a section table full of dead pointers, so `.dynsym`, `.dynstr`
and `.eh_frame` are recovered from the program headers (`PT_DYNAMIC`,
`PT_GNU_EH_FRAME`) when the section table is unusable.

## Architecture

Strict layering; each layer depends only on the ones above it.

```
util/    ByteView, Cursor (bounds-checked reads), LEB128, endianness
diag/    logging, phase timers, counters, peak-RSS reporting
elf/     mmap access, ELF64 headers, sections, symbol tables  (no DWARF knowledge)
dwarf/   constants, section discovery, abbreviation tables,
         unit headers, streaming DIE walker                   (no type semantics)
index/   (M2) DIE indexing and reference resolution
ir/      (M4) type system intermediate representation
output/  (M5) C/C++ source generation
app/     CLI
```

### Design constraints that shaped the code

* **Scale.** The target holds 1183 compilation units and 20,454,580 DIEs, with
  nesting up to depth 27. DIEs are therefore never materialised: `UnitWalker`
  walks the byte stream with a depth counter and allocates nothing per DIE.
  Abbreviation tables are parsed lazily into a bounded LRU cache, and each
  abbreviation caches the fixed byte size of its attribute block so the common
  case is a single bounds-checked pointer add.
* **Memory.** The machine has ~1.3 GB available RAM against a 583 MB file, so
  the file is never read into a heap buffer; it is `mmap`ed read-only and
  sections are addressed in place. Peak RSS for a full traversal is ~257 MB and
  is dominated by the resident page cache of the mapped debug sections, not by
  dumper-owned data.
* **Robustness.** Every read goes through `util::Cursor`, which latches an error
  instead of reading out of bounds. Malformed input is expected from real
  binaries, so a failed parse is reported and the next unit is attempted rather
  than aborting the run.

### Two DWARF details that are easy to get wrong

Both were found by validating against the real binary, and both now have
regression tests:

1. `unit_length` does **not** include the initial length field, so a unit's
   total extent is `initial_length_size + unit_length` — 4 for DWARF32, and 12
   for DWARF64 (a 4-byte `0xffffffff` marker plus a 8-byte length). Advancing by
   the header size instead shifts every later unit and desynchronises the whole
   section.
2. `DW_FORM_udata`/`DW_FORM_sdata` are plain LEB128 *values*; treating them like
   the length-prefixed `DW_FORM_block*` family skips extra bytes and corrupts
   every subsequent DIE. `DW_FORM_implicit_const` takes its value in the
   abbreviation table and occupies zero bytes in the DIE.
3. **DWARF 4 and DWARF 5 number the same tags differently.** The target is
   DWARF 4, where `enumerator` is 0x28 (not 0x21), `subprogram` is 0x2e (not
   0x27) and `variable` is 0x34 (not 0x2e). Using the DWARF 5 table silently
   misclassifies the majority of the file; the correct values were established
   by dumping the attribute set of every distinct tag in the real binary.
