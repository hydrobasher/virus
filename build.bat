@echo off
nasm -f win64 helper.asm -o helper.obj
gcc -c main.c -m64 -o main.obj
gcc main.obj helper.obj -o out.exe -mwindows -luser32 -lgdi32
.\out
del *.obj
del out.exe