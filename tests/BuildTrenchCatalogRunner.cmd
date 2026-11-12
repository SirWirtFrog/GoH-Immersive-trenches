@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist "%~dp0..\out\catalog-runner" mkdir "%~dp0..\out\catalog-runner"
pushd "%~dp0..\out\catalog-runner"
cl /nologo /EHsc /std:c++17 /utf-8 /W4 "%~dp0TrenchCatalogRunner.cpp" "%~dp0..\src\TrenchCatalog.cpp" /Fe:TrenchCatalogRunner.exe
set "runner_exit=%errorlevel%"
popd
exit /b %runner_exit%
