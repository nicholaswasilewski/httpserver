
@echo off
SET CompilerFlags = /O2 /Oi -fp:fast
SET LinkerFlags = WS2_32.LIB

pushd bin
cl %LinkerFlags% ../httpserver.cpp -link %LinkerFlags%
popd