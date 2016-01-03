@echo off
SET CompilerFlags = /O2 /Oi -fp:fast
SET LinkerFlags = WS2_32.LIB

cl %LinkerFlags% httpserver.cpp -link %LinkerFlags%