@echo off
call tools\vcvars.bat || exit /b 1
cl /nologo /EHsc /O2 /MD /std:c++20 tests\sss-target-capture.cpp /Fobuild\tests\ /Febuild\tests\sss-target-capture.exe || exit /b 1
build\tests\sss-target-capture.exe
exit /b %errorlevel%
