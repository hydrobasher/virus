@echo off
g++ virus.cpp -o virus.exe -luser32 -lwinmm -lole32 -lxaudio2_8 -mwindows
start "" .\virus.exe