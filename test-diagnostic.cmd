@echo off
call tools\vcvars.bat || exit /b 1
cl /nologo /EHsc /O2 /MD /std:c++20 /Iexternal\reshade\include /Iexternal\dxc-1.8.2505.1\inc tests\sss-recognition-diagnostic.cpp /Fobuild\tests\ /Febuild\tests\sss-recognition-diagnostic.exe /link bcrypt.lib || exit /b 1
build\tests\sss-recognition-diagnostic.exe
exit /b %errorlevel%
