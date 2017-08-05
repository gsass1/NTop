@echo off

call "C:\Program Files (x86)\Microsoft Visual Studio 14.0\VC\bin\vcvars32.bat"
cl /GA /MT ntop.c Advapi32.lib User32.lib"
