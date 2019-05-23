@echo off

call "C:\Program Files (x86)\Microsoft Visual Studio 14.0\VC\bin\vcvars32.bat"

IF NOT EXIST build mkdir build
pushd build

REM Debug build
cl -W4 /GA /MT /Z7 ..\ntop.c ..\util.c ..\vi.c Advapi32.lib User32.lib

REM Release build
REM cl -W4 /GA /MT /O2 ..\ntop.c ..\util.c ..\vi.c Advapi32.lib User32.lib

popd
