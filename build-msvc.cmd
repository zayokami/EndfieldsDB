@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cd /d "g:\endfields-db"
cmake -B build-msvc -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Debug
cd build-msvc
nmake /f Makefile 2>&1
