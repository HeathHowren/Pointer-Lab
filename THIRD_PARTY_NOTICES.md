# Third-Party Notices

Pointer Lab is distributed under the **GNU General Public License, version 2**
(see [LICENSE](LICENSE)). It statically links the components below. Their
licenses are reproduced or referenced here as required.

## Why Pointer Lab is GPLv2

Pointer Lab statically links **Keystone**, which is licensed under the GPLv2.
Linking GPLv2 code into a distributed binary makes the combined work GPLv2, so
the Pointer Lab binary — and this source repository — are released under the
GPLv2. The other dependencies are permissively licensed (MIT/BSD) and are
compatible with that outcome.

## Written offer for source code (GPLv2 §3)

The complete corresponding source code for Pointer Lab and for every GPL'd
component it links is available at:

    https://github.com/HeathHowren/Pointer-Lab

Each released binary is built from a tagged commit in that repository. The
exact upstream revision of every dependency is pinned in `CMakeLists.txt`,
so any release can be reproduced from source.

---

## Keystone Engine — GPLv2

- Upstream: https://github.com/keystone-engine/keystone
- Version: 0.9.2
- License: GNU General Public License v2.0 (see [LICENSE](LICENSE))

Used as the x86-64 assembler backend. Keystone embeds portions of the LLVM
Project (Apache 2.0 with LLVM exceptions / NCSA); see the upstream
`LICENSE.TXT` and `docs/` in the Keystone repository for the full details.

## Zydis — MIT

- Upstream: https://github.com/zyantific/zydis
- Version: 4.1.1
- License: MIT

Used as the x86-64 instruction decoder and formatter.

```
Copyright (c) 2014-2021 Florian Bernd
Copyright (c) 2014-2021 Joel Hoener

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Dear ImGui — MIT

- Upstream: https://github.com/ocornut/imgui (docking branch)
- Pinned commit: `b61e56346a92cfcaf1f43a545ca37b0b32239654`
- License: MIT

Used for the user interface, together with its Win32 and Direct3D 11 backends.

```
Copyright (c) 2014-2025 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

The bundled fonts (`Roboto-Medium.ttf`, `Cousine-Regular.ttf`), which ship with
Dear ImGui and are embedded into the Pointer Lab executable, are licensed under
the **Apache License 2.0** by Google. See
https://github.com/ocornut/imgui/blob/master/misc/fonts/ for details.

## Lua — MIT

- Upstream: https://www.lua.org/
- Version: 5.4.7
- License: MIT

Used as the embedded scripting engine.

```
Copyright (C) 1994-2024 Lua.org, PUC-Rio.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Catch2 — BSL-1.0 (test builds only)

- Upstream: https://github.com/catchorg/Catch2
- Version: 3.5.2
- License: Boost Software License 1.0

Used only by the unit-test executable. It is **not** linked into the shipped
`PointerLab.exe`.
