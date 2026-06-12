#include <windows.h>
#include <xinput.h>
#include <stdio.h>

int main(void)
{
    HMODULE mod = LoadLibraryA("xinput1_4.dll");
    if (!mod)
        mod = LoadLibraryA("xinput1_3.dll");
    if (!mod)
        mod = LoadLibraryA("xinput1_2.dll");
    if (!mod)
        mod = LoadLibraryA("xinput1_1.dll");
    if (!mod)
        mod = LoadLibraryA("xinput9_1_0.dll");
    if (!mod)
    {
        fprintf(stderr, "XINPUT no dll\n");
        return 2;
    }

    typedef DWORD (WINAPI *XInputGetStateFn)(DWORD, XINPUT_STATE *);
    XInputGetStateFn GetState = (XInputGetStateFn)GetProcAddress(mod, "XInputGetState");
    if (!GetState)
    {
        fprintf(stderr, "XINPUT missing XInputGetState export\n");
        FreeLibrary(mod);
        return 3;
    }

    XINPUT_STATE states[4] = {0};
    DWORD prev_packet[4] = {0};
    int connects = 0;
    int events = 0;

    for (int frame = 0; frame < 30; ++frame)
    {
        for (DWORD i = 0; i < 4; ++i)
        {
            DWORD err = GetState(i, &states[i]);
            if (err == ERROR_SUCCESS)
            {
                if (prev_packet[i] == 0)
                {
                    connects++;
                    printf("XINPUT_ENUM idx=%lu connected\n", i);
                }
                if (states[i].dwPacketNumber != prev_packet[i])
                {
                    printf("XINPUT_EVENT idx=%lu packet=%lu buttons=0x%04X lt=%u rt=%u lx=%d ly=%d rx=%d ry=%d\n",
                           i, states[i].dwPacketNumber,
                           states[i].Gamepad.wButtons,
                           states[i].Gamepad.bLeftTrigger,
                           states[i].Gamepad.bRightTrigger,
                           (short)states[i].Gamepad.sThumbLX,
                           (short)states[i].Gamepad.sThumbLY,
                           (short)states[i].Gamepad.sThumbRX,
                           (short)states[i].Gamepad.sThumbRY);
                    prev_packet[i] = states[i].dwPacketNumber;
                    events++;
                }
            }
        }
        Sleep(100);
    }

    FreeLibrary(mod);
    if (connects == 0)
        printf("XINPUT_ENUM count=0 no-device\n");
    printf("XINPUT_POLL_OK connects=%d events=%d\n", connects, events);
    return 0;
}
