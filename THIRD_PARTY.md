# Third-party components

| Component | Included scope and license |
| --- | --- |
| [ReShade](https://github.com/crosire/reshade) API 20 | Public SDK headers; BSD-3-Clause OR MIT identifiers retained. See `external/reshade/LICENSE.md`. |
| [Dear ImGui 1.92.5](https://github.com/ocornut/imgui/tree/v1.92.5) | Headers matching the ReShade ABI; MIT, `external/imgui/LICENSE.txt`. |
| [MinHook](https://github.com/TsudaKageyu/minhook) | Source and headers; BSD-2-Clause and HDE notices, `external/minhook/LICENSE.txt`. |
| [Streamline](https://github.com/NVIDIA-RTX/Streamline/tree/625db07d4bca5abc67d781c42af7e7fb0761f1b6) | Public headers at the linked revision; NVIDIA MIT notices retained in each header. No Streamline runtime redistributed. |
| [DirectX Shader Compiler 1.8.2505.1](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.8.2505.1) | `dxcapi.h` and notices under `external/dxc-1.8.2505.1/`. Binary package includes the unmodified x64 `dxcompiler.dll` renamed `mw4-rr-dxcompiler.dll`; LLVM license with bundled notices, `LICENSE-LLVM.txt`. |
| [NVIDIA DLSS/NGX SDK](https://github.com/NVIDIA/DLSS) | Supplied separately by the builder under NVIDIA's terms. No SDK import library is linked. Model/runtime DLLs are supplied separately and are not licensed by this project. |

The DXC release's license mapping assigns `LICENSE-MIT.txt` to `d3d12shader.h` and `LICENSE-LLVM.txt` to its other files. The included licenses preserve the vendor's full notices. `dxil.dll` and `dxc.exe` are build tools and are not required beside the addon. Runtime reflection uses only the renamed `dxcompiler.dll`, SHA256 `5888d3e590f5bfd8743484e7f31313996734adec5741506271a5c6368cbe8ba9`.

The addon does not distribute native game shader bytecode. It recognizes a supported shader supplied by the running game and applies its guide transformation in memory. Successfully validated variants may be stored in the per-user local shader cache described in README.md. Cache files are not included in distributed packages. The project license does not grant rights over game assets or the game-supplied shader.

Inherited helper portions retain DLSS5-Feeder's MIT notice (Jean-Laurent ROUZIES) and its attribution to dlss5-dx11-bridge (NIGos). The project license does not override any third-party terms.

The project uses GPL-3.0-only. Inherited portions and separately supplied components retain their notices and applicable terms; GPL does not grant additional rights over separately supplied proprietary components.

## Inherited helper license notice

```text
MIT License

Copyright (c) 2026 Jean-Laurent ROUZIES

Portions derived from dlss5-dx11-bridge, Copyright (c) 2026 NIGos (MIT).

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
