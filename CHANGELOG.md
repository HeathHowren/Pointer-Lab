# Changelog

All notable changes to Pointer Lab are recorded here. This project follows
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

- **An MCP server, so an AI agent can drive a session.** 76 tools over the
  [Model Context Protocol](https://modelcontextprotocol.io) — attach, scan,
  read and write memory, resolve symbols and pointer chains, dissect structures,
  disassemble and assemble, set breakpoints, run the access watch, patch code,
  allocate and inject, drive the speed hack, save and load projects, and move
  the window. Documented per tool in [docs/mcp-api.md](docs/mcp-api.md).

  It runs **inside the application**, holding the same `RuntimeServices` the
  window does, which is the whole point of it. A process the agent attaches to is
  the one on screen; a scan it starts fills the Scanner panel and can be watched
  running; an address it finds appears in the address list a frame later. The
  alternative — a separate headless process — gives an agent its own private
  session and leaves the person next to it with no way to see what happened. This
  way there is one session, and the MCP panel's request log says what was done to
  it.

  Off until you start it, in **View → MCP Server**. `--mcp [port]` starts it at
  launch for someone who runs Pointer Lab in order to hand it to an agent. The
  panel hands over a ready-made `claude mcp add` command, because assembling the
  bearer-token header by hand is where this goes wrong.

  **The tool names are frozen for the life of 3.x**, joining the panel titles,
  menu paths and Lua function names. They are added after 3.0.0 shipped and so
  fall outside the letter of that promise, which is why this says so explicitly:
  a tool name is what an agent's prompt and a saved workflow are written against,
  and that is the case the promise exists for. Where a tool does the same job as
  a Lua function it carries the same name, so the two surfaces can be read as one
  vocabulary rather than two.

- **`UiCommands` can now run arbitrary work on the UI thread.**
  `runOnUiThread`, `saveProject` and `loadProject` join the existing window
  commands, over the same request queue Lua automation already used. This is what
  keeps the UI thread the single mutator of engine state while tool calls arrive
  on a socket thread, and it is available to any future automation surface rather
  than being private to MCP.

### Security

Read this before starting the server.

**Starting it is the only thing you will be asked.** Everywhere else in Pointer
Lab, anything that allocates in, injects into, patches or detaches from a live
process asks first. The MCP server does not. While it is running, a client
holding the token can read and write the target's memory, patch its code, set
breakpoints, allocate, load a DLL into it and start threads in it, with no
further confirmation. The decision is made once, at Start, and it covers
everything that follows.

This is a deliberate departure from this project's own rule that destructive
actions are confirmed. An agent that has to stop for a dialog every few calls is
not usable for the work this is for, and a confirmation prompt that a person
learns to click through is worse than no prompt at all — it looks like a control
and functions as a delay. So the consent is moved to one place where it is
actually read, rather than spread over a hundred places where it would not be.

What that is worth relies on the rest being true, so: the server binds
`127.0.0.1` and nothing else, and requires a bearer token — 32 hex characters
from `BCryptGenRandom`, regenerated on every start, never written to disk,
compared in constant time. Every request is logged to the panel and to
`engine.log`. That is access control over the transport. It decides *who* may
call, not *what* they may do, and it is not a substitute for the confirmations
it replaces.

Two consequences that are easy to miss:

- Nothing an agent does is undone when it disconnects. Patches stay applied,
  allocations stay allocated, injected libraries stay loaded, frozen values stay
  frozen.
- The server is not stopped by detaching, by loading a project, or by the target
  exiting. Stop it when you are done.

### Changed

- `saveProjectTo` and `loadProjectFrom` return `infra::Result<void>` rather than
  `bool`, so a caller that is not the UI — an MCP tool, a script — gets the same
  sentence the toast shows instead of having to invent one.

## [3.0.0] — 2026-08-25

The release that makes Pointer Lab a complete tool rather than a capable one:
32-bit targets, find-what-writes, an auto-assembler, a structure dissector, a
speed hack, trainer export, a tutorial to practise all of it on, and a way to
capture the figures that document it.

**A major version, and the reason is 32-bit targets.** Everything above the raw
read behaved differently for a WOW64 process and none of it said so, which is a
behaviour change rather than an addition: code that appeared to work now works,
and code that appeared to work *and did the wrong thing* now does the right one.

Nothing in the file formats is breaking. `.iretable` project files are still
**format version 3**, because unrecognised record types have always been skipped
rather than rejected and every record added this release is a new type. Settings
still live in their own file.

### The panel names are now a promise

The panel titles and menu paths in this release will not be renamed within the
3.x series. Anything written against them — a book, a course, a capture script,
a note to yourself — can quote them and stay correct until 4.0.

This is a real constraint accepted on purpose. Writing that says "open the
Access Watch panel" is worthless the moment the panel is called something else,
and the person who finds out is a reader following an instruction that no longer
matches what is in front of them. A better name for a panel is not worth that;
it can wait for a major version, where a changed name is expected and looked for.

What is covered: the titles of every panel, the top-level menus and the items in
them, and the names of the Lua functions. What is not: the arrangement of
controls inside a panel, wording of explanatory text, colours, and anything
marked as new in this changelog after 3.0.0 ships.

### Added

- **32-bit (WOW64) target support.** Pointer Lab is still a 64-bit process, but
  it now attaches to 32-bit targets and treats them as such throughout, rather
  than refusing injection and quietly misbehaving everywhere else.

  Attaching and scanning a WOW64 target already worked, because
  `ReadProcessMemory` does not care about the target's width. Everything built
  on top of it did, and every one of the failures was silent:

  - The pointer scanner strode 8 bytes at a time, reading each pair of 32-bit
    pointers as one 64-bit pointer. The scan ran to completion and reported no
    chains found.
  - `resolveChain` read `sizeof(std::uintptr_t)` bytes per hop, so the first hop
    produced an address spliced from two unrelated pointers and every chain
    "broke" at step one.
  - The disassembler was pinned to `ZYDIS_MACHINE_MODE_LONG_64` and the
    assembler to `KS_MODE_64`. Neither errors on 32-bit code — most byte
    sequences decode as *something* in both modes — so the listing was
    plausible and wrong. In the other direction, `inc eax` assembles to one byte
    in 32-bit and two in 64-bit, because `0x40` became the REX prefix.
  - The debug pump called `GetThreadContext`, which for a WOW64 thread returns
    the **64-bit** context describing Windows' emulation layer in
    `wow64cpu.dll` rather than the code the target actually wrote. It now uses
    `Wow64Get/SetThreadContext` for such targets, for breakpoint rewinding, the
    trap flag and the debug registers alike.

  Target width is settled once at attach and shown as a badge in the command
  bar, because a DLL you inject has to match it.

- **Export resolution out of the target.** A new `engine_symbols/ExportResolver`
  parses a module's PE export directory from the target's own memory. This is
  what makes injection into a 32-bit process possible: `GetProcAddress` answers
  about *our* address space, and a WOW64 target has an entirely different
  kernel32 mapped. Forwarder chains are followed, which is not optional —
  `kernel32!LoadLibraryW` forwards to KERNELBASE on every modern Windows, and an
  RVA that lands inside the export directory is a `"DLL.Function"` string rather
  than code. A resolver that missed that would hand the remote thread an address
  inside a string table.

  No DbgHelp, no PDBs and no symbol server: an export directory is present in
  every PE by construction.

- **A 32-bit test helper.** `tests/helper/main.cpp` is now also built for Win32,
  via an `ExternalProject` because MSVC's architecture comes from the generator
  rather than a target property. Eleven new cases in `tests/test_wow64.cpp`
  cover recognition, scanning, chain resolution, pointer width, legacy-mode
  disassembly, per-bitness assembly and export resolution. Optional with
  `-DPOINTERLAB_BUILD_HELPER32=OFF`, in which case those cases `SKIP` rather
  than pass, so a missing fixture cannot be mistaken for coverage.

- **"Find out what writes to this address."** The mechanism — a hardware data
  breakpoint — was already there. What was missing is everything that makes it
  answerable: hits arrived as individual rate-limited notifications that
  scrolled past faster than anyone could read, and the register state was
  buried behind a popup on the breakpoint row.

  The new Access Watch panel aggregates every hit by the instruction
  responsible, busiest first, and offers each one to the disassembler or
  replaces it with nops in place. Two details it gets right:

  - A data breakpoint traps *after* the access completes, so the instruction
    pointer the CPU reports names the instruction *after* the one that touched
    the address. x86 cannot be disassembled backwards, so the accessing
    instruction is identified by decoding from every start position within 24
    bytes and letting each one vote for the boundary it finds; the boundary with
    the most votes wins, which works because x86 is self-synchronising. When
    nothing lands on the trap address, the panel says the
    instruction could not be identified and disables the NOP button, rather than
    naming the wrong instruction confidently and sending the reader to patch
    innocent code.
  - Each captured register is interpreted against the live target: pointing into
    a module (named as `module+offset`, i.e. static), pointing into mapped
    memory, or — the one that matters — sitting a short way below the watched
    address, in which case it is called out as the probable base of the
    structure containing the value. That is the step from "I found my health" to
    "I found the player object".

  Resolution is cached per instruction, so a watch on something inside a render
  loop resolves two or three sites and then costs a map lookup per hit. Distinct
  sites are capped at 256, and reaching the cap is reported rather than
  silently truncating the list.

- **A patch list, so patching is no longer one-way.** Every byte Pointer Lab
  writes into a target's code — an assembler patch, or "replace with code that
  does nothing" from the Access Watch — is recorded in a new `engine_patch`
  registry together with the bytes it replaced, and the new Patches panel turns
  each one on and off with a tick box.

  This was the largest hole in the tool for anyone learning with it. Nopping an
  instruction took one click; putting it back took restarting the target, so the
  natural experiment — change it, see what breaks, change it back — cost a
  reload every time.

  Three behaviours are deliberate:

  - **Overlapping patches are refused.** A second patch inside the first would
    capture the first one's *replacement* bytes as its "original", and disabling
    the two in the wrong order would leave a mixture that was never real code.
    Refusing is the only answer that keeps every recorded original genuinely
    original.
  - **Drift is reported.** If the bytes at a patch are not what the registry
    last wrote there — the target rewriting its own code, an anti-tamper check,
    a write that went around the registry — the row is flagged, because a tick
    box that claims a state the memory does not have is worse than no tick box.
  - **Detaching forgets rather than restores.** A patch someone left applied may
    be the entire point of the session, so it stays applied; what is not
    optional is saying so, and the detach confirmation now names how many
    patches are about to become permanent.

- **Six more scan modes, and text search.** The scanner had six modes and no way
  to search for a string, which left several of the most common questions
  unaskable.

  - **Increased by** and **decreased by** take an exact delta. "I lost exactly 7
    health" is far more selective than "it went down", and usually finishes a
    search in one step where the loose form takes four. Integer deltas wrap with
    their type deliberately: a `u8` going 3 → 253 really did decrease by 6, and
    rejecting that would drop the entry the user is hunting.
  - **Same as first scan** compares against the first scan of the run rather
    than the previous one. A value that changed and came back is *unchanged*
    from the first scan and *changed* from the one before it; those are
    different questions, and only one of them was answerable before.
    `ScanResult` gained a `first` field to carry it, which is why this could not
    be expressed as a variation on Unchanged.
  - **Value between**, **bigger than** and **smaller than** test the value as it
    stands, so unlike the relative modes they filter on a *first* scan with
    nothing to compare against. Bounds given the wrong way round are ordered
    rather than refused: "between 200 and 100" is a typo with exactly one
    plausible meaning.
  - **`str` and `wstr`** search text, one byte and two bytes per character. The
    second is what Windows means by "Unicode" and is how almost every player
    name and chat line in a Windows game is actually stored, so a scanner
    without it finds nothing and gives no hint why. Neither searches for a
    terminator, because a name in a game's memory is usually a fixed buffer with
    junk after the text. Case folding is optional and covers A–Z only; folding
    the rest correctly needs the Unicode tables and a locale, and a scanner that
    folded some characters and not others without saying which would be worse
    than one that is clear about where it stops.

  The modes that order values are not offered for text or byte patterns at all,
  rather than offered and always returning nothing.

- **Static addresses are marked.** A scan result or address-list row that falls
  inside a loaded module image is shown in green, with its `module+offset` on
  hover. This is the check that separates an address worth writing down from one
  that is wherever the allocator happened to put it this launch, and it was
  previously only discoverable by opening the Modules panel and comparing ranges
  by eye.

- **Address expressions everywhere.** Every address box now accepts
  `client.dll+0x4A2C10`, `kernel32.LoadLibraryW+0x10`, a module name on its own,
  or a symbol you have defined, each followed by any number of `+`/`-` offsets —
  the memory viewer, the disassembler, the breakpoint panel, the pointer scanner
  and the address list alike. A new Symbols panel defines and lists them.

  Symbols are saved in the project file as the **expression**, not the address it
  produced, and re-resolved against whatever is attached on load. An address
  recorded in one run names nothing in the next.

- **Manual pointer-chain entry.** The address list's editor gained a Pointer
  tick box: the address field becomes the chain's base and an offsets field says
  how to walk from it, with the resolved address and the value at it shown live
  while you type. A chain that is one offset wrong resolves to a plausible
  address holding a plausible number, so being able to see what it lands on
  before committing is the difference between finding the mistake now and finding
  it after an hour.

  A base inside a module is recorded as `module+offset` automatically, which is
  what makes the chain survive a restart. A base that is not gets recorded
  absolute and marked amber, in the editor and on the row, rather than silently
  producing a chain that works today and not tomorrow.

- **A navigable hex editor.** The memory viewer was a fixed 16–4096 byte
  read-only window with a blind patch box. It now scrolls by row, by page and by
  wheel, edits a byte in place when you click it, follows a byte as a pointer,
  and highlights bytes that changed since the last frame.

  Scrolling *backwards* from a known address until the start of a structure
  appears is one of the most useful things a person can do with a hex editor,
  and until now it was a hex-arithmetic exercise: work out the address you
  wanted and retype it. The changed-byte highlight is the other half — watching
  a structure repaint itself finds a field without scanning for it at all.

- **A structure dissector.** A named layout — offset, type, name — laid over
  several addresses at once, with an auto-guess pass that fills it in from what
  is actually at those addresses. Reachable as "Dissect this" from the address
  list, the scan results and the hex editor.

  The comparison is the point, not the display. A game does not have one player,
  it has an array of them: put two instances side by side and the fields that
  read the same in both are padding or shared state, while the ones that differ
  are what actually describes an object. Those rows are greyed rather than
  hidden, so the eye lands on the ones worth naming.

  The guess calls a slot a pointer when its value at every address is either
  null or inside a committed region and at least one is non-null — otherwise
  every run of zeroes in an object would be full of pointers. It calls a slot a
  float when the bytes decode to a finite, non-denormal value between 1e-6 and
  1e9, which works because the two readings barely overlap: a small integer
  reads as a denormal around 1e-43, and an ordinary float reads as an integer in
  the hundreds of millions. Eight-byte slots are only considered at eight-byte
  alignment; looking anyway finds "pointers" straddling two unrelated fields.

  Negative offsets are legal, because where an object starts is a guess and
  finding that the real one begins earlier should not mean renumbering
  everything below it. Overlapping fields are refused, for the same reason
  overlapping patches are.

- **An auto-assembler.** A script with an `[ENABLE]` section that patches the
  target and a `[DISABLE]` section that puts it back, in the tradition the
  literature teaches, with `aobscanmodule`, `alloc`, `label`, `registersymbol`,
  `unregistersymbol`, `dealloc`, `define`, `assert` and `db`/`dw`/`dd`/`dq`.
  Three templates — AOB injection, code cave, full injection — are one button
  away, and the Access Watch offers "Script" on any row so an injection starts
  from the instruction you just found, with the address, the bytes and the module
  already filled in.

  The point is not convenience. A code injection performed by hand is a sequence
  of steps that exist only while you remember them; the same injection written
  down is one artefact that can be read, checked before it runs, shared, and
  re-run after the target restarts and everything has moved. Four decisions
  follow from taking that seriously:

  - **`check` compiles without writing.** Directives run, patterns are scanned
    for, asserts are evaluated, the layout is computed — and nothing is written
    or allocated. Reading a script critically before running it is only possible
    if you can see what it worked out.
  - **`aobscanmodule` refuses a pattern that matches twice.** Taking the first
    match produces a script that works today and patches something unrelated
    after the next update.
  - **`assert` is in every generated template.** A script written against one
    build names addresses that mean something else in the next, and without the
    check the first thing it does after an update is overwrite the wrong
    instruction.
  - **A failed run rolls back.** Patches applied before the failure are removed
    in reverse and allocations are freed, so a script never leaves a target half
    injected. `[DISABLE]` restores the patches first and frees the cave second:
    a thread already on its way into an unmapped cave lands in nothing.

  `alloc(name, size, near)` allocates within ±2 GB of the hint by walking free
  regions with `VirtualQueryEx`, and **fails** rather than falling back to a
  distant block — a five-byte `jmp` encodes a signed 32-bit displacement, so a
  cave out of that range cannot be reached at all.

  Scripts are saved in the project file and always load switched off.

- **Scripted figure capture.** Six new Lua functions — `screenshot(path)`,
  `select_panel(name)`, `set_layout("default")`, `set_window_size(w, h)`,
  `wait_frames(n)` and `quit()` — plus a `--script <file.lua>` command-line flag
  that runs a script once the window is up. `scripts/capture-figures.lua` uses
  them to write one PNG per panel.

  A figure captured by hand is correct on the day it was taken and silently
  wrong afterwards: a panel gets renamed, a control moves, and nobody finds out
  until a reader follows an instruction that no longer matches the picture
  beside it. A capture script is re-run on every release, and a panel that no
  longer answers to its name fails the run.

  Two details are what make a capture reproducible rather than merely
  automatic. The window is given an exact *client* size, so figures taken a year
  apart are the same number of pixels and the frame around them — which differs
  between Windows versions — is not in the picture. And every call blocks until
  the UI thread has done the work, with `wait_frames` on top of that, because
  opening a panel takes effect on the next frame and its docking settles on the
  one after: a capture that followed immediately would catch the layout
  mid-move.

  What is captured is the window's own back buffer, not the screen, so nothing
  behind the window and no notification that happens to appear can end up in a
  figure. The cost is that a panel dragged out into its own OS window will not
  appear, which is what `set_layout("default")` is for.

  `screenshot` is the one exception to the script sandbox, which removes `io`
  entirely. It writes one file, of one format, holding a picture of this
  program's own window; it is not a way back to arbitrary writes.

- **`PointerLabTutorial.exe` — a practice target that ships with the tool.** Nine
  gated lessons, in both x86 and x64 builds, each one revealing the password for
  the next so nothing has to be repeated: attach and write · exact value scan ·
  unknown initial value with increased/decreased · float and double · find out
  what writes · a pointer · a multi-level pointer · code injection · shared code.

  It exists because every other thing to practise on belongs to somebody else. A
  reader's first lesson should not depend on a third-party download that has
  changed since it was written about, and the answer should be checkable by
  something other than the reader's own confidence.

  The checks are arranged so that the plausible-but-wrong technique fails, which
  is most of what makes them worth doing:

  - Step 5 calls the writing code and reads back immediately, so freezing the
    value passes nothing and only removing the instruction does.
  - Steps 6 and 7 move the object and scramble it before checking, so an address
    found by scanning is pointing at freed memory by then and only an entry that
    re-resolves its chain survives.
  - Step 9 damages both objects directly and reads back immediately, so neither
    a frozen value nor a NOP passes — only code that looks at which object is
    being written to.

  The values live in one allocation with the individual values a page apart
  rather than in adjacent globals, because adjacent globals hand over the next
  step's answer in the same scan result. The writer functions are `noinline` and
  the link runs with `/OPT:NOICF`, so two steps cannot end up sharing one
  instruction.

- **A speed hack.** A payload DLL — `PointerLabSpeed64.dll` and
  `PointerLabSpeed32.dll`, shipped beside the executable — redirects the four
  clocks a Windows game can ask: `QueryPerformanceCounter`, `GetTickCount`,
  `GetTickCount64` and `timeGetTime`. Presets from 0.1x to 5x, a logarithmic
  slider, and "Remove the hook" behind a confirmation.

  A game does not measure time; it asks Windows what time it is and multiplies
  everything it does that frame by how much has passed. So this is not a hack on
  the game at all, and it needs to know nothing about the game — which is why one
  payload works everywhere the technique works.

  - **The clock is rebased, not multiplied.** `real * scale` makes the clock jump
    by hours the instant the rate changes, and jump *backwards* when it is
    lowered. A game that sees time go backwards does not slow down; it divides by
    a negative delta and detonates. Each clock keeps the real and fake values from
    when the rate last changed and answers
    `fake = baseFake + (real - baseReal) * scale`, re-reading both bases on every
    change, so the reported clock is continuous and monotonic across every one.
  - **Import tables are patched, not instructions.** Every
    `call [__imp_QueryPerformanceCounter]` reads its destination from a table in
    the game's own image; writing a different address there redirects all of them
    at once, and the undo is writing the old value back. No length decoder, no
    trampoline, and nothing rewritten while another thread is executing it.
  - **The modules that implement the clocks are skipped**, which is a correctness
    requirement rather than caution: kernel32 is largely stubs that reach the
    implementation in kernelbase *through kernel32's own import table*, so
    patching it sends the hook's call to the real function straight back into the
    hook and the target dies of a stack overflow on the first frame that asks the
    time.
  - **A hook that found nothing to hook says so.** `hookedImports` is reported in
    the panel, and zero after a successful injection means this target does not
    ask for the time through its import table — it resolved the address at
    runtime, or it uses a clock this hook does not cover. That is a real limit of
    the technique and it is stated rather than left looking like a bug.
  - **A speed outside 0.05x–20x is refused, not clamped.** A silently clamped
    request for 1000x looks exactly like a hook that is installed and doing
    nothing.

- **Trainer export.** The address list can be written out as a small CMake
  project — `main.cpp`, `CMakeLists.txt`, `README.md` — for a standalone external
  trainer with hotkey toggles.

  It generates **source, not an executable**, and that is the point. A generated
  binary is a black box: it works, and you have learned nothing about why.
  Generated source is the same trainer with its reasoning visible — how a process
  is found by name, how a module base is looked up, how a pointer chain is walked
  one dereference at a time. It is also the difference between a program you have
  read and a program of unknown provenance you are about to point at your own
  machine.

  Module-rooted entries are emitted as `module+offset` plus their offset chain
  and survive a restart. Absolute ones are emitted too and labelled as a hazard,
  in the source and again in the README, because they are valid only for the run
  they were found in. Entries with no frozen value have nothing to write and are
  listed under "Not exported" rather than dropped. The architecture is written
  into both the CMake file and the build instructions: a trainer built for the
  wrong width reads two pointers as one, every chain breaks at the first hop, and
  nothing reports an error.

### Changed

- `domain::RegisterContext` records the bitness it was captured with, and the
  breakpoint panel names registers from it — `EAX`/`EIP` for a 32-bit thread,
  with `r8`–`r15` hidden rather than shown as eight zeroes they do not have.
- A pointer chain with **no offsets** is now legal, and means "the base itself,
  with no dereferencing" — which is how a static address is written down.
  Manual entry produces these; the pointer scanner never does. A chain with an
  **absolute base** and no module name is legal too, for the same reason, and is
  marked as not surviving a restart wherever it appears.
- `.iretable` files gain `struct` and `field` records, holding structure
  layouts. The addresses a layout was last applied to are deliberately not
  saved: a layout is knowledge about the game and outlives every run, while the
  address of one particular enemy does not survive the next respawn.
- `.iretable` files gain a `script` record, holding an auto-assembler script's
  name and source. The source is one field however long it is, because the
  escaping already turns newlines into `\n`. The enabled flag is deliberately not
  stored: at load time nothing has been patched, so a script claiming to be on
  would offer to undo changes that were never made.
- Detaching now switches off every enabled script first, which is the opposite of
  what it does with patches, and the asymmetry is deliberate. A patch is one run
  of bytes with one undo record, so leaving it applied is a coherent state. A
  script's changes are a set of patches plus memory it allocated plus symbols it
  published, and only the script knows how to take them apart — once the handle
  closes, nothing can.
- `.iretable` files gain a `symbol` record, and the chain fields now distinguish
  "no chain" from "a chain whose base is absolute". Chain offsets are written as
  the unsigned bit pattern rather than as a signed value, because a manual chain
  may step backwards through a structure and a minus sign only round-tripped by
  accident.
- The 32-bit sub-builds (the speed payload, the tutorial and the test helper) are
  marked `BUILD_ALWAYS`. `ExternalProject` stamps its build step on first success
  and has no idea the shared source changed, so a 32-bit binary could sit months
  behind the 64-bit one built from the same file — which is the worst version of
  that bug, because the two are supposed to be identical and the difference only
  appears in the targets nobody tests.
- Detaching also puts the clocks back and forgets where the payload's control
  block lived, for the same reason chains are re-resolved rather than reused:
  those addresses name nothing in the next process.
- `.iretable` files gain a `bitness` record. **The format version is still 3**,
  deliberately: unrecognised record types have always been skipped rather than
  rejected, so a 2.1.0 build reads a file containing it and ignores the line,
  whereas `IRETABLE 4` would make it refuse the whole table. Loading a table
  against a target of the other bitness now warns, because pointer chains
  survive a restart but not a change of architecture — the offsets were measured
  against a struct layout in which every embedded pointer was a different size,
  so the chain still resolves, to the wrong field.

### Fixed

- **Typing a value into the address-list editor and pressing Apply changed
  nothing in the target until freeze was toggled.** Apply stored the value as
  the entry's freeze value and stopped there, and the freeze loop only writes
  entries that are frozen — so on an unfrozen entry the number sat in the list
  looking applied while the target still held the old one. Apply (and Add) now
  write the value to the target immediately, the same way the row's Write
  button does, and report the failure if the write does not land.

- **Pointer Lab now scales to the display it is on.** `resources/app.manifest`
  has declared per-monitor-v2 DPI awareness since 1.0, which is a promise to
  Windows that the application will do its own scaling — and nothing did. On a
  150% display every control was laid out at 96 DPI, so the window opened two
  thirds the intended size with text drawn at full size inside it, and dragging
  it to a second monitor at a different scale changed nothing. Fonts and window
  rectangles now follow the monitor, the style metrics are rebuilt at the new
  scale on `WM_DPICHANGED`, and the fixed widths inside panels — table columns,
  combo boxes, the hex viewer's ASCII gutter — scale with them.

  The style is rebuilt from its unscaled values on every change rather than
  scaled in place: 5 pixels of padding truncates to 6 at 125%, and scaling that
  back down does not return 5, so two trips across a monitor boundary left the
  layout visibly drifted.

- **Editors kept state from the row you were last looking at.** Several panels
  held a selection and a set of buffers that could disagree with each other:

  - The Address List editor kept its contents after Add or Apply, so a second
    click added a duplicate entry — and, since Apply now writes, wrote the value
    again. Editing a pointer-chain entry and unticking Pointer left the chain
    attached, so the background resolver rewrote the address back within the
    frame and the row stayed unresolved forever. Editing one that stayed a chain
    dropped its frozen state on the way through.
  - The Structure Dissector applied a field edit to whichever structure was
    selected *now* rather than the one the offset was read from, and the name box
    kept the previous structure's name after New, Remove, or an automatic
    reselection.
  - The Scripts panel let a script be enabled while the editor held unsaved
    changes, which ran the saved source rather than the one on screen — the
    checkbox is disabled until the edits are saved and says so. Switching scripts
    or applying a template over unsaved work now asks first, and New creates a
    script called "New script" rather than a row with no visible name.

- **Controls that were clipped, unreachable, or lying.** The address editor and
  the scan controls sat in fixed-height boxes that cut off their own buttons as
  soon as a warning appeared inside them; they size to their contents now. The
  "Region filters" header vanished entirely when its View toggle was off. Two
  of the four buttons on an Access Watch row were culled by the table before
  they could be clicked. The Memory Viewer's navigation row ran off the edge of
  a docked panel, and its hex editor claimed the keyboard every frame it was
  open, so typing anywhere else in the application went to it instead. Long
  module names, paths, symbols and patch bytes were truncated with no way to see
  the rest; they now show the full text on hover. Warnings, the results legend
  and the Help window wrap instead of running off the panel.

- **Actions that silently did nothing.** Starting a Lua scan with no process
  attached reported progress and found nothing rather than saying to attach
  first. Attaching to a second process while already attached swapped the handle
  underneath the patches, breakpoints and scripts belonging to the first, so
  unticking a patch wrote its original bytes into the *new* process at the same
  address; it now goes through the same confirmation and detach the Detach button
  does. Dissecting a ninth address reported success and discarded it. A
  confirmation dialog raised while one was already open replaced it under the
  reader. Two access sites whose addresses differ only above bit 31 shared widget
  state, so opening a menu on one opened it on the other. The status pills went
  on reading CONNECTED and WATCHING after the target had exited — Pointer Lab now
  notices within a second and detaches.

- **The interface did work proportional to the target on every frame.** Panels
  were rebuilding their whole contents 60 times a second regardless of how much
  of it was on screen: Memory Regions laid out every one of a real target's tens
  of thousands of rows, the scan results table copied the entire result set out
  from under the scan worker's lock — up to a million entries, each holding three
  vectors — and the Structure Dissector copied and sorted every region in the
  target once per frame, per field, to decide whether a value looked like a
  pointer. Reads of the target were on the same footing: the Address List read
  every entry's current value, the Patches panel re-read every patch to check for
  drift, and typing a name like `kernel32.LoadLibraryW` into an address box
  re-parsed that module's export directory on every keystroke and every frame
  after it.

  The large tables now draw only their visible rows and fetch only that range,
  target reads are refreshed on a timer rather than per frame, and everything
  derived from the module and region lists is cached until an attach, refresh or
  detach can actually have changed it.

- **An uncaught C++ exception left a crash dump behind and a crash log saying
  none had been written.** `MiniDumpWriteDump` was walking the stack of the very
  thread that called it, and on the terminate path that walk runs with an
  exception still in flight underneath it: the call writes most of the file and
  then returns false. The dump is now written from a thread created for the job,
  which has a clean stack and nothing in flight, and DbgHelp suspends and walks
  the crashing thread the way it would from an external debugger — the
  arrangement the API is documented for. The reason a dump failed is written
  into the crash log too, so the next report of this kind says what went wrong
  rather than only that something did.

## [2.1.0] — 2026-08-22

A feature release, and the first one that is not mostly repair work. Every item
in it closes a gap that 2.0.0 documented as a known limit rather than fixed.

Nothing in it is breaking: `.iretable` project files are still **format version
3**, the licence is unchanged, and every existing workflow behaves as it did.
Software breakpoints remain the default, and settings live in their own file
rather than in the project format.

### Added

- **Pointer scan rescan pass.** The pointer scanner can now narrow the chains it
  already found instead of only starting over. Restart the target, find the
  value's new address, put it in Target address and press Rescan: every chain is
  re-resolved and only the ones that still land on the value are kept. A first
  scan returns thousands of chains that pointed the right way once; this is what
  reduces them to the handful that actually track the value. It costs a few reads
  per chain rather than another sweep of the address space, so it takes seconds
  where the first scan took minutes.

  A rescan refuses, and leaves the chains untouched, when nothing is attached or
  there is nothing to narrow — resolution fails for every chain with no process
  attached, so running it anyway would silently discard a scan that took minutes.
  Cancelling a rescan also leaves the chains untouched, because a half-applied
  filter is not a result set.

- **Hardware breakpoints and watchpoints.** A breakpoint can now use one of the
  processor's four debug registers instead of an `int3`. Nothing in the target is
  modified and nothing is ever disarmed, which closes the window a software
  breakpoint can be missed through: another thread running the address while the
  original byte is temporarily back no longer slips past. They also break on
  **data** rather than only on code — watch an address for writes, or for reads
  and writes, in widths of 1, 2, 4 or 8 bytes.

  There are exactly four, because the processor has four debug registers. The
  fifth is refused with a message that says so rather than silently displacing
  one, and the breakpoint table names the register each one holds. Software
  breakpoints are unchanged and remain the default.

- **Global hotkeys.** F1–F12 freeze toggles are now registered with Windows
  instead of being read from Pointer Lab's own message queue, so they fire while
  the target window is in the foreground. That is the only time they were ever
  wanted: the previous in-window handling stopped working the moment you clicked
  into the target.

  Only keys actually assigned to an address list entry are registered, so
  Pointer Lab does not take F1–F12 away from every other application for as long
  as it is open. A key another application already owns cannot be registered; it
  is reported and falls back to the old in-window behaviour rather than silently
  doing nothing.

- **Settings persistence.** Scan options (result limit, float tolerance, the
  writable and executable filters), the selected value types, the pointer scan
  depth and which panels are open now survive a restart. ImGui's own `.ini`
  already remembered where a panel sat and what it was docked to, but whether it
  was open at all is the application's state, so a panel you closed came back on
  every launch.

  These live in a plain `settings.ini` next to the log, not in `.iretable`: they
  belong to the installation rather than to a project, and a result limit has no
  business travelling with a project file to somebody else's machine. **The
  project format is unchanged and still version 3.**

  A missing, malformed or newer-than-expected settings file falls back to the
  values the previous release used, and one unparseable line does not discard the
  rest of the file.

- **Lua `cancelled()` and `check_cancel()`.** A script can now see that Stop has
  been pressed, which matters in a loop that spends its time inside Pointer Lab's
  own functions where the VM hook rarely gets a turn. `check_cancel()` ends the
  script there and then; like Stop itself, it cannot be caught.

### Changed

- **`loadlibrary()` returns the module's base address** rather than the remote
  thread's exit code, which was only the low 32 bits of the module handle and so
  looked like an address on a 64-bit target without being one. The module list is
  refreshed as part of the call, so a newly loaded DLL is visible to `modules()`
  immediately. If the module cannot be found afterwards the old exit code is
  returned rather than reporting a failure that did not happen.

### Fixed

- **A cancelled Lua script can no longer catch its own cancellation.** Stop
  raised a Lua error, and `pcall` catches errors, so
  `while true do pcall(f) end` swallowed the cancel and carried on — erroring
  again every 10 000 instructions and never stopping. A runaway loop is exactly
  what Stop is for. Scripts now run on a coroutine and cancelling yields instead
  of raising; a yield passes straight through `pcall` and `xpcall`, and the
  coroutine is simply never resumed.

- **Stopping a script now stops the scan it started.** The script ended but its
  scan carried on in the background, so Stop did not stop the work, and the
  results turned up in the next script that looked.

- **A crash dump is no longer lost to a failed memory walk.** The dump asks
  for the memory the stack points at, and that walk can report failure after
  most of the file has already been written -- most often when an uncaught
  throw brings the process down, where an exception is still in flight while
  the walk runs. The crash then left behind a large `.dmp` and a log line
  saying no dump had been written, which is the one moment the log has to be
  believable. A rich dump that fails is now started again as a plain one,
  which needs no walk: much less than the full dump, much more than nothing.

### Internal

- **`UiApp.cpp` is split into nine files grouped by area.** It was a single
  2500-line file, roughly five times the size of the next largest, and the one
  component with no test coverage. The class and its header are unchanged and
  every panel is still one method; only which file each lives in has moved. The
  helpers the panels share moved from an anonymous namespace into
  `src/ui/UiInternal.h`.

### Known limits

Still true, and still stated rather than hidden — see the README for the full
list. A software breakpoint can be missed during its single-step window, which is
inherent to `int3`; use a hardware breakpoint when it matters. There are only
four hardware breakpoints, because the processor has four debug registers.
Breakpoint notifications are rate limited (hit counts are not). Time inside a
Lua C function is still not interruptible. Partial reads succeed with a short
buffer. 64-bit targets only.

## [2.0.0] — 2026-08-18

The first release intended for real use. The alpha built and ran, but a number
of its advertised features were stubs that returned nothing, and its breakpoints
crashed the target. Everything below is either a fix for that or the work needed
to make the claims in the README true.

A major version because two things changed that are not backward compatible:
Pointer Lab is now **licensed under the GPLv2** rather than being unlicensed, and
`.iretable` project files are now written in **format version 3**, which earlier
releases cannot read. (Pointer Lab still reads versions 1 and 2.)

### Added

- **Project persistence.** File → New / Open / Save / Save As, autosave on exit
  and autoload on start. `ProjectStore` existed in the alpha
  but was called from nowhere, so the address list was lost on every exit. The
  format now records pointer chains and is documented in
  [docs/iretable-format.md](docs/iretable-format.md).
- **Real disassembler.** Zydis replaces the roughly twenty-opcode hand-rolled
  decoder. Instruction lengths are correct, so the listing no longer desyncs;
  undecodable bytes are shown as `db` and advance one byte. Branch targets are
  resolved and followable.
- **Real assembler.** Keystone replaces the nine-mnemonic parser. Patches are
  NOP-padded to the next instruction boundary so a short patch cannot leave half
  an instruction behind.
- **Working breakpoints.** Software breakpoints are now repeatable: RIP rewind,
  original-byte restore, trap-flag single-step and re-arm. Register context is
  captured on hit and shown in the UI. Hit counts climb and the target keeps
  running.
- **Relative scan modes as first scans.** Changed / Unchanged / Increased /
  Decreased capture a baseline instead of matching nothing, and Next scan works
  with an empty value box — the unknown-value workflow that is the reason those
  modes exist.
- **Wildcard byte patterns.** `48 8B ?? 24` now works; `??` was previously
  stripped and ignored.
- **Float epsilon matching.** Bit-exact float comparison finds nothing in
  practice, so exact float scans now use a configurable tolerance.
- **Pointer chains as live addresses.** A found chain can be added to the address
  list, is stored as `module+offset`, is re-resolved about twice a second, and
  survives both a save/load cycle and a target restart under ASLR. Previously
  "Add base" discarded the offsets.
- **Lua console off the UI thread**, with a working cancel — an infinite loop no
  longer hangs the application permanently. Added scan-result access so scripted
  scans can be chained, typed reads and writes, module and region enumeration,
  and pointer-chain resolution. Errors come back with a traceback.
- **Confirmation dialogs** on every destructive action: remote RWX allocation,
  remote thread creation, DLL injection, assemble-and-patch, raw patch, entry
  removal, breakpoint removal, and detaching with live breakpoints. The alpha had no
  dialogs at all.
- **Error surfacing.** Failures raise a toast and are logged; previously they
  went only to a hidden log panel, and many were discarded entirely.
- **Crash minidumps.** `MiniDumpWriteDump` on unhandled exceptions, uncaught C++
  throws, invalid CRT parameters and pure-call violations, with a message naming
  the dump. Help → Write a diagnostic dump captures a dump without crashing.
- **About and Help dialogs**, including the keybindings, the Lua API and the
  legal/ethical-use notice.
- **Tests and CI.** 80 Catch2 tests, including integration tests against a real
  spawned target process, an end-to-end workflow test that scans, freezes,
  saves, restarts the target and re-resolves, and crash-path tests against a
  process that crashes on purpose. GitHub Actions builds and tests Debug and
  Release, packages the release zip, and attaches it to tagged releases.
- **LICENSE (GPLv2), THIRD_PARTY_NOTICES, CONTRIBUTING, SECURITY**, version
  resources, an application manifest, and CPack packaging.

### Fixed

- **Breakpoints crashed the target.** Execution resumed in the middle of the
  overwritten instruction. This was the most serious bug in the alpha.
- **Detaching or removing a breakpoint could kill the target.** An `int3` already
  in flight was delivered to a process that no longer had a debugger. Fixed by
  recognising stale traps and draining queued events before releasing.
- **Use-after-free in DLL injection** — the remote path buffer was freed before
  the wait completed.
- **Partial `ReadProcessMemory` was reported as complete success.**
- **`parseAddress` misread un-prefixed hex**, affecting every address field.
- **Missing font files null-dereferenced on startup.** Fonts are now embedded, so
  the executable is genuinely portable.
- **64-bit identifiers were truncated to `int`** for ImGui IDs, so rows more than
  4 GB apart could collide.
- **Chunk-boundary duplicates and a wrong progress denominator** in the scanner;
  a cap that silently aborted an unknown scan after about 1 MB.
- **The pointer scanner's linear frontier search**, which made anything past
  depth 1 effectively never finish. It now uses a sorted target set with a binary
  search, plus dedup and a cycle guard.
- **Unguarded `std::stoull` in the project loader**, which threw out of `load()`
  on a malformed file — and because an uncaught C++ exception bypassed the SEH
  filter, produced no crash log either.
- **Dropped Win32 and D3D return values**, including `VirtualProtectEx`.
  Device-removed (TDR) is now recovered from, and rendering is skipped when
  minimized.
- **Write failures in the freeze loop were discarded silently.** A failing entry
  is now disabled and reported.
- **A target-handle lifetime race** between the freeze loop and `detach()`.
- **Most of the UI was invisible on a first run.** Ten of the thirteen panels
  defaulted to hidden and had no place in the default layout, so a new user saw
  the process list, the scanner and the address list and nothing else. Every
  panel now opens by default with its own home in the layout, and Reset Layout
  reopens the ones you closed.
- **The default dock layout was overridden on the frame it was built**, which
  collapsed six panels into two tab bars even once they were visible.
- **The log file was reopened for every line** and truncated on launch,
  destroying the evidence from the run that crashed. It is now held open and
  rotated.
- Swapped ternary in the runtime services status text; non-atomic pump flags;
  ImGui and DX11 shutdown running when `run()` bailed before initialisation.

### Changed

- **Licensed under GPLv2** (was unlicensed), because Keystone is GPLv2 and is
  statically linked.
- **Statically linked C runtime.** The alpha claimed "no dependencies" while
  requiring the VC++ redistributable.
- **Scan result limit is configurable and truncation is reported** as "N of M"
  rather than silently capping.
- **Read-only attachment is surfaced** with a badge instead of a silent
  half-attach with failing writes.
- **`SeDebugPrivilege` is acquired at startup** when available.
- Log panel gained per-thread ids, a level filter, a text filter and a link to
  the log folder; the scanner, pointer scanner and project store now log what
  they do.
- README rewritten to describe what the tool actually does, including a
  **Known limits** section.

### Known limits

Carried forward deliberately rather than hidden — see the README for the full
list. In short: no pointer-scan rescan pass; a software breakpoint can be missed
by another thread during its single-step window; breakpoint notifications are
rate limited (hit counts are not); partial reads succeed with a short buffer;
64-bit targets only.

## [1.0.1] — 2026-05-13

README cleanup for the alpha release. No code changes.

Note for anyone comparing version numbers: the tree's own
`project(PointerLab VERSION ...)` still read `0.1.0` at this point, while the
published tags had moved to `1.0.x`. 2.0.0 is the first release where the
version in the source, the version resource in the executable, the packaged zip
and the git tag all agree.

## [1.0.0-alpha] — 2026-05-13

Initial alpha. Built and launched, with a working process browser, exact-value
scanner, address list, memory viewer and ImGui dockspace UI.
