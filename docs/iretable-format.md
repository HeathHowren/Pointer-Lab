# The `.iretable` project format

A Pointer Lab project file records an address list and which process it was last
used against. It is a plain UTF-8 text file, one record per line, deliberately
simple enough to read and hand-edit in any text editor.

The format is implemented in [`src/storage/ProjectStore.cpp`](../src/storage/ProjectStore.cpp).

## Overall shape

```
IRETABLE 3
pid|4812
process|helper.exe
bitness|x64
entry|1|7ff6a1c02040|i32|1|Player health|Stats|F1|64000000|||
entry|2|0|i32|0|Score|Stats||||helper.exe|3040|10,8
```

- **Line 1 is the header** and must be exactly `IRETABLE 1`, `IRETABLE 2` or
  `IRETABLE 3`. Anything else is refused as "not a Pointer Lab project file, or
  written by a newer version". A UTF-8 byte-order mark before the header is
  tolerated, because text editors and PowerShell add one without asking.
- Every later line is a record whose type is the text before the first `|`.
- Line endings may be LF or CRLF.
- Blank lines are ignored.
- Unrecognised record types are counted as skipped and ignored, so a newer
  version's extra records do not make an older file unreadable.

Pointer Lab always writes the current version (3). It reads all three.

## Escaping

Fields are separated by `|`. Inside a field:

| Sequence | Means |
| --- | --- |
| `\|` | a literal `\|` |
| `\\` | a literal backslash |
| `\n` | a line feed |
| `\r` | a carriage return |

A backslash before anything else is dropped and the following character is taken
literally. Splitting happens before unescaping, so an escaped `\|` never ends a
field.

## Records

### `pid`

```
pid|<decimal process id>
```

The process id Pointer Lab was last attached to. Advisory only — it is used to
preselect a process in the list, never to attach automatically, since the id
will usually belong to something else by the time the file is opened again. An
unparseable value is skipped rather than failing the load.

### `process`

```
process|<process name>
```

The executable name of the last target, for display.

### `bitness`

```
bitness|x86
bitness|x64
```

Pointer width of the process this table was built against. Loading a table
against a target of the other bitness raises a warning: pointer chains survive a
restart, because they are stored as `module+offset`, but they do not survive a
change of architecture — every embedded pointer in the target's structures
changes size, so the offsets no longer name the fields they were measured
against. The chain still resolves; it resolves to the wrong place.

Absent from files written before this record existed, in which case `x64` is
assumed — every such file was written by a build that could only attach to
64-bit targets.

Note that this record was added **without** a format version bump, deliberately.
Unrecognised record types have always been skipped rather than rejected, so a
2.1.0 build reads a file containing this line and simply ignores it; writing
`IRETABLE 4` would instead have made it refuse the table outright.

### `symbol`

```
symbol|health|game.exe+0x4A2C10
symbol|loadlib|kernel32.LoadLibraryW
```

A user-defined name, stored as the **expression** that produced it rather than
as the address that expression resolved to. This is the whole point: an address
recorded in one run names nothing in the next, because ASLR has moved the
module, whereas `game.exe+0x4A2C10` names the same thing every time.

On load each expression is re-resolved against the currently attached target. A
symbol whose module is not loaded is reported and dropped rather than restored
to a stale address; the count is shown and each failure is named in the log.

Symbols defined against a bare address, with no expression behind them, are not
saved at all, for the same reason.

Added without a format version bump, exactly as `bitness` was and for the same
reason.

### `script`

```
script|infinite health|[ENABLE]\naobscanmodule(INJECT, game.exe, 89 46 04)\n...
```

One auto-assembler script: a name and its source. The source is a single field
however long it is, because the escaping turns each newline into `\n` — so a
script is always exactly one line of the file.

Scripts are **always loaded switched off**, and the enabled flag is deliberately
not stored. At load time there is no target and nothing has been patched; a
script that claimed to be on would be offering to undo changes that were never
made. It also means the reader gets to look at a script before it runs, which is
the right default for a file that may have arrived from someone else.

A script with an empty source is kept if it has a name — that is a script
someone started and has not written yet, and silently dropping it would be worse
than carrying a blank one. A record with neither is ignored.

Added without a format version bump, exactly as `bitness` and `symbol` were and
for the same reason.

### `struct` and `field`

```
struct|Player
field|0|u64|0|vtable
field|8|f32|0|health
field|10|bytes|c|name
```

A structure definition: a `struct` header naming it, then one `field` line per
member, in offset order. Fields belong to the `struct` record above them, and a
`field` with no `struct` above it is **ignored with a warning** rather than
attached to whatever comes next — a layout in the wrong structure is worse than
a missing one.

Field columns, in order: offset (hex, no `0x`), value type, length (decimal),
name. The length is meaningful only for the variable-width types — `bytes`,
`str` and `wstr` — and is where their width comes from; for everything else it
is written as `0` and the width comes from the type. A field that ends up with
no width at all is skipped.

Offsets are written as the **unsigned bit pattern**, the same convention as
pointer-chain offsets, so a negative offset round-trips exactly. Negative
offsets are legal and ordinary: where an object starts is a guess, and
discovering that the real one begins four bytes earlier should not mean
renumbering everything below it.

Fields could have been packed into the header line. They are not, because a
layout is the record in this file most likely to be read and edited by hand, and
one field per line is what makes that bearable.

The **addresses** a structure was last laid over are deliberately not saved. A
layout is knowledge about the game and outlives every run; the address of one
particular enemy does not survive the next respawn.

Structure ids are assigned when the file is read rather than stored, because
nothing outside a single run refers to one.

Added without a format version bump, exactly as `bitness`, `symbol` and `script`
were and for the same reason.

### `entry`

One address-list row. Fields in order:

| # | Field | Format | Notes |
| --- | --- | --- | --- |
| 0 | record type | `entry` | |
| 1 | id | decimal | Stable within a file |
| 2 | address | hex, no `0x` | The last resolved address |
| 3 | type | name | See [value types](#value-types) |
| 4 | frozen | `1` or `0` | Any other text reads as `0` |
| 5 | description | escaped text | |
| 6 | group | escaped text | |
| 7 | hotkey | escaped text | e.g. `F1`; empty for none |
| 8 | frozen value | hex bytes | The value written while frozen |
| 9 | chain module | escaped text | Empty for a fixed address, or for a chain with an absolute base |
| 10 | chain module offset | hex, no `0x` | Absolute base when field 9 is empty |
| 11 | chain offsets | comma-separated hex | May be empty: see below |

Fields 0–8 are required; a row with fewer than nine fields is skipped. Fields
9–11 were added in version 3 and are absent in version 1 and 2 files, which
simply means every entry is a fixed address — which is all those versions could
express.

Extra fields beyond 11 are ignored, so a file written by a future version still
loads.

#### Value types

`i8`, `u8`, `i16`, `u16`, `i32`, `u32`, `i64`, `u64`, `f32`, `f64`, `bytes`,
`str`, `wstr`. Matching is case-insensitive on read; Pointer Lab always writes
lower case.

`str` is one byte per character and `wstr` is UTF-16, two bytes per character —
what Windows calls "Unicode". Both are variable length, like `bytes`: the width
comes from the value, not from the type.

An unrecognised type **skips the entry** rather than falling back to a default.
A wrong width would silently read and write the wrong number of bytes at that
address, which is worse than losing the row.

#### Pointer chains

An entry is backed by a pointer chain, rather than a fixed address, when field 9
is non-empty **or** field 10 is non-zero. A chain produced by the pointer scanner
is always module-rooted; a manually entered one need not be. The module-rooted
form is stored relative to its module so it survives ASLR:

```
entry|2|0|i32|0|Score|Stats||||helper.exe|3040|10,8
```

means: find `helper.exe` in the target, add `0x3040` to get the chain root, read
a pointer there, add `0x10`, read a pointer, add `0x8` — and that is the address.

Offsets are written in hex without `0x` and separated by commas, as the unsigned
bit pattern — a manually entered chain may step backwards through a structure,
and a negative offset written with a minus sign would only round-trip by
accident. An empty element or a non-hex element is malformed.

An **empty** offset list is not malformed. It means "the base itself, with no
dereferencing", which is how a static address is written down:

```
entry|3|0|i32|0|Ammo|Stats||||helper.exe|3040|
```

is `helper.exe+0x3040`, re-resolved every run. Manual entry produces these; the
pointer scanner never does.

When field 9 is **empty** and field 10 is non-zero, field 10 is an absolute base
rather than a module offset. Only manual entry produces such a chain, and it does
not survive a restart — the address list marks it amber to say so.

A malformed chain costs the **chain**, not the entry: the row is kept as a fixed
address at field 2 and a warning is logged. That is deliberate — the address is
still probably useful, and dropping the row entirely would lose the user's
description and grouping too.

Chain-backed entries load with their resolved flag cleared, because the address
in field 2 was resolved in a process that has since exited. A background pass
(about twice a second) re-resolves them against the current target; until it
does, the address list shows `unresolved`, and if the module is not present it
shows `<chain broken>`.

## Error handling

`load()` fails outright only for an unreadable file or a bad header. Everything
else degrades:

- A malformed `pid` is skipped.
- A row with too few fields, an unparseable id or address, or an unknown value
  type is skipped.
- A malformed pointer chain downgrades that row to a fixed address.

The count of skipped lines is written to the log with the file name, and each
skipped-for-cause row logs its line number.

`save()` fails if the file cannot be opened or if the write does not flush
cleanly — a full or read-only disk is reported rather than assumed successful.

## Where files live

- The autosaved session is `%LOCALAPPDATA%\PointerLab\session.iretable`. It is
  written on exit and loaded on start.
- Named projects go wherever you save them, via File → Save As.
