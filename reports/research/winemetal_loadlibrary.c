#include <windows.h>
#include <stdio.h>

int main(void) {
    SetLastError(0);
    HMODULE mod = LoadLibraryW(L"winemetal.dll");
    if (!mod) {
        DWORD err = GetLastError();
        printf("LoadLibraryW(winemetal.dll) FAIL gle=%lu\n", (unsigned long)err);
        return 1;
    }

    printf("LoadLibraryW(winemetal.dll) PASS module=%p\n", (void*)mod);
    FreeLibrary(mod);
    return 0;
}
