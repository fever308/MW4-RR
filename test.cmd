@echo off
setlocal
cd /d "%~dp0"
if not exist "%~1\nvsdk_ngx.h" (
 echo Usage: test.cmd "NGX include directory"
 exit /b 2
)
if not exist build\rr_sparse.generated.h (
 echo Run build.cmd first to prepare shader headers.
 exit /b 2
)
call tools\vcvars.bat || exit /b 1
if not exist build\tests mkdir build\tests
set "INC=/Isrc /Iexternal\imgui /Iexternal\streamline /Iexternal\reshade\include /I"%~1" /Iexternal\minhook\include /Iexternal\dxc-1.8.2505.1\inc"
set "FLAGS=/nologo /EHsc /O2 /MD /W3 /std:c++20"
set "HOOKS=external\minhook\src\buffer.c external\minhook\src\hook.c external\minhook\src\trampoline.c external\minhook\src\hde\hde64.c"
set "LIBS=kernel32.lib user32.lib psapi.lib d3d12.lib dxgi.lib d3dcompiler.lib"
cl %FLAGS% %INC% /c tests\preset-policy.cpp /Fobuild\tests\ || exit /b 1
for %%T in (runtime-shader shader-cache sr-replacement sr-presentation sr-layouts settings-and-layout controller-lifecycle weapon-depth edge-fill release-buffer-cpu release-hooks-cpu release-private-transition-cpu) do (
 cl %FLAGS% %INC% /Fobuild\tests\ /Febuild\tests\%%T.exe tests\%%T.cpp %HOOKS% /link %LIBS% || exit /b 1
 build\tests\%%T.exe || exit /b 1
)
for %%T in (transparency-admission transparency-classifier transparency-blend transparency-blend-hooks-cpu transparency-coverage-pipeline transparency-coverage transparency-blend-controller transparency-blend-arm) do (
 cl %FLAGS% %INC% /Fobuild\tests\ /Febuild\tests\%%T.exe tests\%%T.cpp %HOOKS% /link %LIBS% || exit /b 1
 build\tests\%%T.exe || exit /b 1
)
echo CPU regression tests passed. No GPU or game was started.
exit /b 0
