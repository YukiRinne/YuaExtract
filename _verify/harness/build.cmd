@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
cd /d "E:\Harness&Agent Project\YuaToolkit²å¼þ\YuaExtract\_verify\harness"
cl /nologo /LD /EHsc /utf-8 /O2 /W3 /I"E:\Harness&Agent Project\YuaToolkit²å¼þ\YuaExtract\CxdecExtractor" FakeCore.cpp /Fe:"E:\Harness&Agent Project\YuaToolkit²å¼þ\YuaExtract\_verify\harness\Run\CxdecExtractordll\CxdecExtractor.dll"
