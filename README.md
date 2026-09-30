# Stellar — native ELF/DWARF dumper

[![CI](https://github.com/vekendianorg/stellar/actions/workflows/ci.yml/badge.svg)](https://github.com/vekendianorg/stellar/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

Upstream: <https://github.com/vekendianorg/stellar>

A from-scratch C++20 ELF/DWARF dumper for large shared objects. It reads the
debug info directly, reconstructs the C++ type model, and writes an
IL2CPP-dumper-style C# dump. Binaries with no debug info are handled by a
separate mode whose output is explicitly labelled as inferred.

The output format follows a commercial dumper's layout. Nothing is assumed to
exist in a binary merely because it appears in a reference dump.

## Building

CMake 3.20+ and a C++20 compiler. No third-party dependencies.

```sh
cmake --preset release
cmake --build --preset release
ctest --preset default
```

Or, from any POSIX shell:

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
exist, so it cannot determine the system version and aborts. Supplying
`CMAKE_SYSTEM_NAME` and `CMAKE_SYSTEM_VERSION` skips that path. It is harmless on
native Termux, so `scripts/build.sh` applies it automatically whenever it detects
a Termux install prefix. `STELLAR_FORCE_TERMUX=0|1` overrides the detection.

### Binaries

Some filesystems — notably Android's `sdcardfs` — do not carry the executable
bit, so a binary written there cannot be run in place. `scripts/build.sh` copies
`stellar` and `stellar-tests` to `$STELLAR_RUN_DIR` (default `$TMPDIR/stellar-run`) and marks them
executable.

### Progress

Long stages update a single terminal line in place, so a multi-minute run never
looks like a hang:

```
[ 24%] Units: 283/1183 | DIEs: 4,908,043 | Types: 715,300 | Stage: DWARF
[100%] Units: 1183/1183 | DIEs: 20,454,580 | Lines: 6,383,926 | Stage: Writing dump
stellar: done (dwarf) libfoo.so in 0.0s -- 1183 units, 20,454,580 DIEs, ... -> 0 lines, 0.0 MB
```

| Option | Effect |
|---|---|
| `--progress auto` | *(default)* draw only when stderr is a terminal |
| `--progress always` | draw even when redirected |
| `--progress never` | disable |
| `--progress-interval MS` | minimum gap between redraws (default 80) |

`auto` is the default so a redirected log does not fill up with carriage
returns. The reporter is deliberately off the critical path, because its
counters sit inside the DIE loop: a disabled update is a single predictable
branch, and when enabled the clock is read once per ~2000 updates rather than
per update, with rendering reusing one buffer.

## Usage

```sh
stellar info  [options] <elf>     # ELF + DWARF section/capability summary
stellar units [options] <elf>     # per-unit header + DIE count
stellar scan  [options] <elf>     # walk DIEs, count tags, measure throughput
stellar dump  [options] <elf>     # print one unit's DIE tree
stellar emit  [options] <elf>     # build the model and write the dump
```

Bounding options keep exploration cheap on a large input:

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
stellar info  libfoo.so
stellar scan  --max-units 20 --tags --stats libfoo.so    # bounded
stellar scan  --unit-stride 100 --stats libfoo.so        # every 100th unit
stellar scan  --stats libfoo.so                         # full scan
stellar dump  --unit 0 --max-print 40 libfoo.so
stellar emit  --stats libfoo.so                         # -> output/dump.cs
sh scripts/emit.sh libfoo.so                        # same, via the helper
```

The dump is written to `output/dump.cs` by default. That directory is
git-ignored, since a full dump is a few hundred MB. `emit` options:
`-o/--out`, `--name`, `--no-pad` (skip synthesised padding fields),
`--no-methods`, `--max-lines`, `--build-units`, `--list-conflicts N` (show
same-named types whose definitions disagreed).

## Testing

```sh
sh scripts/test.sh                 # fast cases, synthetic fixtures only
sh scripts/test.sh --real          # the opt-in real-binary cases
ctest --preset default             # same, via CTest
```

The default suite is fast and hermetic: it builds synthetic ELFs in memory
(`tests/fixture_builder.*`), so DWARF constructs a given target may not contain
— DWARF64, DWARF 5 headers, `DW_FORM_implicit_const`, truncated and reserved
headers — are still covered without shipping a large sample.

The real-binary cases need a large ELF and are **skipped unless you point at
one**:

```sh
STELLAR_REAL_BINARY=/path/to/liblarge.so sh scripts/test.sh --real
```

`STELLAR_REAL_BINARY` accepts a file or a directory containing it. There is
deliberately no default path: a path baked into the repository would only be
valid on one machine and would skip everywhere else for the wrong reason.

## Two modes

`stellar emit` picks a mode automatically (`--mode=auto|dwarf|dwarfless` overrides).

**`dwarf` — ground truth.** Types, member offsets, enums, methods and
inheritance come from `.debug_info`. Output goes to `output/dump.cs`.

**`dwarfless` — inferred, and labelled as such.** For a binary with no DWARF,
function ranges still come from `.eh_frame` and class names from RTTI. Field
offsets and field types are *not* recoverable and are not guessed, so the output
says so three times: the filename is `dump.dwarfless.cs`, a banner at the top of
the file says `NOT GROUND TRUTH`, and every record carries a `TIER:` tag so
`grep -c 'TIER:infer'` sizes the problem. `--fail-on-low-confidence` exits 3 for
CI.

A library dumped from memory has a section table full of dead pointers, so
`.dynsym`, `.dynstr` and `.eh_frame` are recovered from the program headers
(`PT_DYNAMIC`, `PT_GNU_EH_FRAME`) when the section table is unusable.

## Output shape

```c#
// Namespace:
public enum AccountType // TypeDefIndex: 3 Size: 0x4 UnderlyingType: int
{
    ACCOUNT_FINGERSOFT = 0,
}

// Namespace:
public class Foo : Base // TypeDefIndex: 845 Size: 0x320 Confidence: exact
{
    // Bases: Base @ 0x0 public
    // Fields
    public List<Item> m_items; // 0x18
    public string m_name; // 0x30

    // Methods

    // RVA: 0x1535394 Offset: 0x1535394 VA: 0x1535394
    public virtual void init() { }
}
```

Fields are ordered by offset, methods by address. Every method is preceded by a
location comment; when the address could not be recovered it reads
`// RVA: unavailable Offset: unavailable VA: unavailable`, so a reader can
always tell "no code" from "not looked up". A parameter whose name DWARF does
not carry is emitted positionally as `arg0`, `arg1`, and the line above it says
so.

Types are normalised for a managed reader: `std::vector<T>` becomes `List<T>`,
`std::basic_string` becomes `string`, smart pointers become their pointee, and
vendor standard-library namespaces are dropped. Compiler and ABI artefacts
(`_vptr$Class`, generated `k<Field>FieldNumber` constants, mangled lambdas and
sort helpers, thunks) are not emitted. A type's own *identity* is left alone:
a class genuinely called `std::__ndk1::vector<...>` is not renamed to
`List<...>`, because collapsing it would merge two distinct types.

## Architecture

Strict layering; each layer depends only on the ones above it.

```
util/    ByteView, Cursor (bounds-checked reads), LEB128, endianness
diag/    logging, progress reporting, phase timers, counters, peak RSS
elf/     mmap access, ELF64 headers, sections, symbol tables  (no DWARF knowledge)
dwarf/   constants, section discovery, abbreviation tables, unit headers,
         streaming DIE walker, .eh_frame                  (no type semantics)
ir/      type-system intermediate representation and model builder
output/  C# dump generation
app/     CLI
```

### Design constraints that shaped the code

* **Scale.** A large library holds millions of DIEs nested tens of levels deep.
  DIEs are therefore never materialised: `UnitWalker` walks the byte stream with
  a depth counter and allocates nothing per DIE. Abbreviation tables are parsed
  lazily into a bounded LRU cache, and each abbreviation caches the fixed byte
  size of its attribute block, so the common case is one bounds-checked pointer
  add.
* **Memory.** The input is not read into a heap buffer; it is `mmap`ed read-only
  and sections are addressed in place. Peak RSS for a full traversal is dominated
  by the resident page cache of the mapped debug sections rather than by
  dumper-owned data, which is why it tracks the size of the debug info rather
  than the number of DIEs.
* **Robustness.** Every read goes through `util::Cursor`, which latches an error
  instead of reading out of bounds. Malformed input is expected from real
  binaries, so a failed parse is reported and the next unit is attempted rather
  than aborting the run.

### DWARF details that are easy to get wrong

All three were found empirically, and each has a regression test:

1. `unit_length` does **not** include the initial length field, so a unit's
   total extent is `initial_length_size + unit_length` — 4 for DWARF32, and 12
   for DWARF64 (a 4-byte `0xffffffff` marker plus an 8-byte length). Advancing by
   the header size instead shifts every later unit and desynchronises the whole
   section.
2. `DW_FORM_udata`/`DW_FORM_sdata` are plain LEB128 *values*; treating them like
   the length-prefixed `DW_FORM_block*` family skips extra bytes and corrupts
   every subsequent DIE. `DW_FORM_implicit_const` takes its value in the
   abbreviation table and occupies zero bytes in the DIE.
3. **DWARF 4 and DWARF 5 number the same tags differently.** For a DWARF 4
   producer, `enumerator` is 0x28 (not 0x21), `subprogram` is 0x2e (not 0x27) and
   `variable` is 0x34 (not 0x2e). Using the DWARF 5 table silently
   misclassifies most of the file. The values in
   `include/stellar/dwarf/constants.h` were established by dumping the attribute
   set of every distinct tag the input actually contains, rather than trusting
   either spec from memory.
