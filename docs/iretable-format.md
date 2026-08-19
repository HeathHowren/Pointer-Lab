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
| 9 | chain module | escaped text | Empty for a fixed address |
| 10 | chain module offset | hex, no `0x` | Only read if field 9 is non-empty |
| 11 | chain offsets | comma-separated hex | Only read if field 9 is non-empty |

Fields 0–8 are required; a row with fewer than nine fields is skipped. Fields
9–11 were added in version 3 and are absent in version 1 and 2 files, which
simply means every entry is a fixed address — which is all those versions could
express.

Extra fields beyond 11 are ignored, so a file written by a future version still
loads.

#### Value types

`i8`, `u8`, `i16`, `u16`, `i32`, `u32`, `i64`, `u64`, `f32`, `f64`, `bytes`.
Matching is case-insensitive on read; Pointer Lab always writes lower case.

An unrecognised type **skips the entry** rather than falling back to a default.
A wrong width would silently read and write the wrong number of bytes at that
address, which is worse than losing the row.

#### Pointer chains

When field 9 is non-empty the entry is backed by a pointer chain rather than a
fixed address. The chain is stored relative to a module so it survives ASLR:

```
entry|2|0|i32|0|Score|Stats||||helper.exe|3040|10,8
```

means: find `helper.exe` in the target, add `0x3040` to get the chain root, read
a pointer there, add `0x10`, read a pointer, add `0x8` — and that is the address.

Offsets are written in hex without `0x` and separated by commas. An empty offset
list, an empty element, or a non-hex element is malformed.

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
