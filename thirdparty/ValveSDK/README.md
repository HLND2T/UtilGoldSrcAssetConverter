# Valve SDK interface files

`interface.h`, `interface.cpp` and `FileSystem.h` are Valve SDK code (copyright Valve Corporation) as used by
HLND2T-DiligentGraphics (`public/`), with their original copyright notices. They are distributed under the Half-Life 1
SDK license in [LICENSE](LICENSE), not under this project's MIT License.

They differ from the original Half-Life SDK: `FileSystem.h` matches the `filesystem_stdio.dll` of HLND2T-DiligentGraphics
(for example `Read` / `Write` take `size_t` sizes), and `interface.h` declares `CreateInterfaceFn` as returning `void*`.
Keep them identical to the HLND2T-DiligentGraphics copies; the command line tool calls that `filesystem_stdio.dll`
through `FileSystem.h`.
