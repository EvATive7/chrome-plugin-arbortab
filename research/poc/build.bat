@echo off
setlocal
pushd "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "MH=..\..\native\third_party\minhook"
cl /nologo /O2 /MT /LD /EHsc /I"%MH%\include" poc.cpp "%MH%\src\buffer.c" "%MH%\src\hook.c" "%MH%\src\trampoline.c" "%MH%\src\hde\hde64.c" /Fe:version.dll /link /DEF:poc.def
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%

