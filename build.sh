#!/bin/sh
# Cross-compile KeyRemapper.exe with clang/lld + mingw-w64 headers, no C runtime.
set -e
W=${WINSDK:-/opt/win}
cd "$(dirname "$0")"
mkdir -p build
CFLAGS="--target=x86_64-w64-windows-gnu -isystem $W/include -O2 -Wall -Wextra -Wno-unused-parameter \
 -ffreestanding -fno-stack-protector -mno-stack-arg-probe -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DUNICODE -D_UNICODE"
clang $CFLAGS -c src/main.c -o build/main.o
clang $CFLAGS -c src/theme.c -o build/theme.o
clang $CFLAGS -c src/winmode.c -o build/winmode.o
llvm-rc -I "$W/include" -I src -C 65001 -FO build/app.res src/app.rc
lld-link /nologo /out:KeyRemapper.exe /subsystem:windows,6.02 /entry:WinMainCRTStartup /nodefaultlib \
 /stack:0x100000,0x100000 /opt:ref /opt:icf /dynamicbase /nxcompat /highentropyva /manifest:no \
 build/main.o build/theme.o build/winmode.o build/app.res \
 $W/lib/libkernel32.a $W/lib/libuser32.a $W/lib/libgdi32.a $W/lib/libcomctl32.a $W/lib/libcomdlg32.a \
 $W/lib/libshell32.a $W/lib/libole32.a $W/lib/libgdiplus.a $W/lib/libuxtheme.a $W/lib/libdwmapi.a $W/lib/libadvapi32.a
echo built KeyRemapper.exe
