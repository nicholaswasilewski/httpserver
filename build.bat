
@echo off
SET CompilerFlags = /O2 /Oi -fp:fast

IF NOT EXIST bin mkdir bin
pushd bin
cl %CompilerFlags% ..\httpserver.cpp
popd bin