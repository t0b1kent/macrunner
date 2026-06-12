#define _WIN32_WINNT 0x0601
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <dinput.h>
#include <stdio.h>
#include <string.h>

typedef struct
{
    int found;
    GUID first_guid;
} enum_state_t;

static int g_button_changes = 0;

static BOOL CALLBACK enum_gamepad_cb(const DIDEVICEINSTANCEA *instance, VOID *context)
{
    enum_state_t *state = (enum_state_t *)context;
    printf("DINPUT_ENUM index=%d guid=%08lx-%04x name=%s\n",
           state->found, instance->guidInstance.Data1,
           (unsigned)instance->guidInstance.Data3,
           instance->tszInstanceName[0] ? instance->tszInstanceName : "<noname>");
    if (!state->found)
    {
        memcpy(&state->first_guid, &instance->guidInstance, sizeof(GUID));
    }
    state->found++;
    return DIENUM_CONTINUE;
}

int main(void)
{
    HRESULT hr;
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr))
    {
        fprintf(stderr, "DINPUT: CoInitializeEx failed 0x%08lX\n", (unsigned long)hr);
        return 2;
    }

    HMODULE dinput_module = LoadLibraryA("dinput8.dll");
    if (!dinput_module)
    {
        fprintf(stderr, "DINPUT: dinput8.dll missing\n");
        CoUninitialize();
        return 3;
    }

    typedef HRESULT (WINAPI *DirectInput8CreateFn)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
    DirectInput8CreateFn DirectInput8CreateProc = (DirectInput8CreateFn)GetProcAddress(dinput_module, "DirectInput8Create");
    if (!DirectInput8CreateProc)
    {
        fprintf(stderr, "DINPUT: DirectInput8Create export missing\n");
        FreeLibrary(dinput_module);
        CoUninitialize();
        return 4;
    }

    IDirectInput8A *di = NULL;
    hr = DirectInput8CreateProc(GetModuleHandleA(NULL), DIRECTINPUT_VERSION, &IID_IDirectInput8A, (LPVOID *)&di, NULL);
    if (FAILED(hr) || !di)
    {
        fprintf(stderr, "DINPUT: DirectInput8Create call failed 0x%08lX\n", (unsigned long)hr);
        FreeLibrary(dinput_module);
        CoUninitialize();
        return 5;
    }

    enum_state_t state = {0};
    hr = IDirectInput8_EnumDevices(di, DI8DEVCLASS_GAMECTRL, enum_gamepad_cb, &state, DIEDFL_ATTACHEDONLY);
    if (FAILED(hr))
    {
        fprintf(stderr, "DINPUT: EnumDevices failed 0x%08lX\n", (unsigned long)hr);
        IDirectInput8_Release(di);
        FreeLibrary(dinput_module);
        CoUninitialize();
        return 6;
    }

    IDirectInputDevice8 *dev = NULL;
    DIJOYSTATE2 prev = {0};
    int polled = 0;
    int iterations = 0;

    if (state.found > 0)
    {
        HRESULT hrDev = IDirectInput8_CreateDevice(di, &state.first_guid, &dev, NULL);
        if (SUCCEEDED(hrDev) && dev)
        {
            hr = IDirectInputDevice8_SetDataFormat(dev, &c_dfDIJoystick2);
            if (SUCCEEDED(hr))
            {
                HWND hwnd = GetForegroundWindow();
                if (!hwnd)
                    hwnd = GetDesktopWindow();

                hr = IDirectInputDevice8_SetCooperativeLevel(dev, hwnd, DISCL_NONEXCLUSIVE | DISCL_FOREGROUND);
                if (SUCCEEDED(hr))
                {
                    hr = IDirectInputDevice8_Acquire(dev);
                }

                if (SUCCEEDED(hr))
                {
                    while (iterations++ < 20)
                    {
                        HRESULT poll_hr = IDirectInputDevice8_Poll(dev);
                        if (FAILED(poll_hr))
                        {
                            IDirectInputDevice8_Acquire(dev);
                            Sleep(25);
                            continue;
                        }

                        DIJOYSTATE2 cur = {0};
                        hr = IDirectInputDevice8_GetDeviceState(dev, sizeof(cur), &cur);
                        if (SUCCEEDED(hr))
                        {
                            polled++;
                            if (memcmp(&prev, &cur, sizeof(cur)) != 0)
                            {
                                g_button_changes++;
                                printf("DINPUT_EVENT X:%d Y:%d Z:%d RX:%d RY:%d RZ:%d\n",
                                       (int)cur.lX, (int)cur.lY, (int)cur.lZ,
                                       (int)cur.lRx, (int)cur.lRy, (int)cur.lRz);
                                memcpy(&prev, &cur, sizeof(cur));
                            }
                        }
                        Sleep(100);
                    }
                }
            }
            if (dev)
                IDirectInputDevice8_Release(dev);
        }
    }

    IDirectInput8_Release(di);
    FreeLibrary(dinput_module);
    CoUninitialize();

    if (state.found == 0)
        printf("DINPUT_ENUM count=0 no-gamepad\n");
    else
        printf("DINPUT_EVENTS polled=%d changes=%d devices=%d\n", polled, g_button_changes, state.found);

    printf("DINPUT_POLL_OK\n");
    return 0;
}
