# c2d — native ELF/DWARF dumper

A from-scratch C++20 ELF/DWARF dumper for large Android shared objects. It walks 20,454,580 DIEs across 1183 compilation units in a 583 MB binary
and emits a C# type dump, falling back to an explicitly-labelled inferred mode
when a binary is stripped of DWARF.

Inputs used for development and validation:

| File | What it is |
|---|---|
| `libcocos2dcpp_1.74.2.so` | 583 MB ELF64 AArch64 shared object with 500 MB of DWARF; 1183 units, 20,454,580 DIEs |
| `com.fingersoft.hcr2-*.bin` | 34 MB memory dump of the same library, **stripped**: no `.debug_info`, and a section table full of dead pointers |
| `dump_1.73.cs` | output of a commercial dumper on the previous version; the format this project reproduces |

Nothing is assumed to exist in a binary because it appears in a reference dump.

## Building

CMake 3.20+ and a C++20 compiler. No third-party dependencies.

```sh
cmake --preset release
cmake --build --preset release
ctest --preset default
```

Or, on any POSIX shell:

```sh
sh scripts/build.sh
sh scripts/test.sh
```

| Preset | Use |
|---|---|
| `release` / `debug` | Linux, macOS, MSVC, Ninja, MSYS2 — the normal case |
| `termux` | Termux, native or proot (see below) |

Windows: use a Developer Command Prompt with the Visual Studio generator, or
Ninja from MSYS2.

### Why the `termux` preset exists

Under proot, CMake's host detection shells out to `getprop`, which does not
exist, so it cannot determine the system version and aborts with
`file failed to open for reading: /include/android/api-level.h`. Supplying
`CMAKE_SYSTEM_NAME` and `CMAKE_SYSTEM_VERSION` skips that path. It is harmless
on native Termux, so `scripts/build.sh` applies it automatically whenever it
detects the Termux install prefix. `C2D_FORCE_TERMUX=0|1` overrides the
detection.

### Binaries

Some filesystems — notably Android's `sdcardfs` — do not carry the executable
bit, so a binary written there cannot be run in place. `scripts/build.sh`
copies `c2d` and `c2d-tests` to `$C2D_RUN_DIR` (default `$TMPDIR/c2d-run`) and
marks them executable.

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
c2d info  libfoo.so
c2d scan  --max-units 20 --tags --stats libfoo.so    # bounded, ~0.1 s
c2d scan  --unit-stride 100 --stats libfoo.so        # every 100th unit
c2d scan  --stats libfoo.so                         # full scan
c2d dump  --unit 0 --max-print 40 libfoo.so
c2d emit  --stats libfoo.so                         # -> output/dump.cs
sh scripts/emit.sh libfoo.so                        # same, via the helper
```

The dump is written to `output/dump.cs` by default (that directory is
git-ignored, since a full dump is a few hundred MB). `emit` options:
`-o/--out`, `--name`, `--no-pad` (skip synthesised padding
fields), `--no-methods`, `--max-lines`, `--build-units`, `--list-conflicts N`
(show same-named types whose definitions disagreed).

## Testing

```sh
sh scripts/test.sh                 # 30 fast cases, synthetic fixtures only
sh scripts/test.sh --real          # the opt-in real-binary cases
ctest --preset default             # same, via CTest
```

The default suite is fast and hermetic: it builds synthetic ELFs in memory
(`tests/fixture_builder.*`), so DWARF constructs the real target does *not*
contain — DWARF64, DWARF 5 headers, `DW_FORM_implicit_const`, truncated and
reserved headers — are still covered without shipping a 583 MB sample.

The real-binary cases need a large ELF and are **skipped unless you point at
one**:

```sh
C2D_REAL_BINARY=/path/to/libcocos2dcpp_1.74.2.so sh scripts/test.sh --real
```

`C2D_REAL_BINARY` accepts a file or a directory containing it. There is
deliberately no default path: a path baked into the repository would only be
valid on one machine and would skip everywhere else for the wrong reason.

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
