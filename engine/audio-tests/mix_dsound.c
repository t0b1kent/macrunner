#include <windows.h>
#include <dsound.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>

typedef struct
{
    HRESULT hr;
} Result;

static uint32_t crc32_update(uint32_t crc, const void *buf, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)buf;
    uint32_t c = crc;
    for (size_t i = 0; i < len; ++i)
    {
        c ^= bytes[i];
        for (int b = 0; b < 8; ++b)
            c = (c & 1u) ? (c >> 1u) ^ 0xEDB88320u : (c >> 1u);
    }
    return c;
}

int main(void)
{
    HRESULT hr;
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr))
    {
        fprintf(stderr, "DSOUND CoInitializeEx failed: 0x%08lX\n", (unsigned long)hr);
        return 2;
    }

    IDirectSound8 *ds = NULL;
    hr = DirectSoundCreate8(NULL, &ds, NULL);
    if (FAILED(hr) || !ds)
    {
        fprintf(stderr, "DSOUND DirectSoundCreate8 failed: 0x%08lX\n", (unsigned long)hr);
        CoUninitialize();
        return 3;
    }

    HWND hwnd = GetForegroundWindow();
    if (!hwnd)
        hwnd = GetDesktopWindow();
    hr = IDirectSound8_SetCooperativeLevel(ds, hwnd, DSSCL_PRIORITY);
    if (FAILED(hr))
    {
        fprintf(stderr, "DSOUND SetCooperativeLevel failed: 0x%08lX\n", (unsigned long)hr);
        IDirectSound8_Release(ds);
        CoUninitialize();
        return 4;
    }

    DSBUFFERDESC primaryDesc = {0};
    primaryDesc.dwSize = sizeof(primaryDesc);
    primaryDesc.dwFlags = DSBCAPS_PRIMARYBUFFER | DSBCAPS_GLOBALFOCUS;
    IDirectSoundBuffer *primary = NULL;
    hr = IDirectSound8_CreateSoundBuffer(ds, &primaryDesc, &primary, NULL);
    if (FAILED(hr))
    {
        fprintf(stderr, "DSOUND Create primary buffer failed: 0x%08lX\n", (unsigned long)hr);
        IDirectSound8_Release(ds);
        CoUninitialize();
        return 5;
    }

    WAVEFORMATEX format = {0};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = 44100;
    format.wBitsPerSample = 16;
    format.nBlockAlign = (format.nChannels * format.wBitsPerSample) / 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    hr = IDirectSoundBuffer_SetFormat(primary, &format);
    IDirectSoundBuffer_Release(primary);
    if (FAILED(hr))
    {
        fprintf(stderr, "DSOUND SetFormat failed: 0x%08lX\n", (unsigned long)hr);
        IDirectSound8_Release(ds);
        CoUninitialize();
        return 6;
    }

    const DWORD num_frames = format.nSamplesPerSec / 2; /* 0.5s */
    const DWORD payload = num_frames * format.nBlockAlign;
    DSBUFFERDESC desc = {0};
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DSBCAPS_CTRLVOLUME | DSBCAPS_GLOBALFOCUS | DSBCAPS_GETCURRENTPOSITION2;
    desc.dwBufferBytes = payload;
    desc.lpwfxFormat = &format;

    IDirectSoundBuffer *secondary = NULL;
    hr = IDirectSound8_CreateSoundBuffer(ds, &desc, &secondary, NULL);
    if (FAILED(hr))
    {
        fprintf(stderr, "DSOUND Create secondary buffer failed: 0x%08lX\n", (unsigned long)hr);
        IDirectSound8_Release(ds);
        CoUninitialize();
        return 7;
    }

    LPVOID ptr1 = NULL, ptr2 = NULL;
    DWORD len1 = 0, len2 = 0;
    hr = IDirectSoundBuffer_Lock(secondary, 0, payload, &ptr1, &len1, &ptr2, &len2, 0);
    if (FAILED(hr))
    {
        fprintf(stderr, "DSOUND Lock failed: 0x%08lX\n", (unsigned long)hr);
        IDirectSoundBuffer_Release(secondary);
        IDirectSound8_Release(ds);
        CoUninitialize();
        return 8;
    }

    uint32_t crc = 0xFFFFFFFFu;
    int16_t *pcm = (int16_t *)ptr1;
    const double two_pi = 6.28318530717958647692;
    const int16_t amp = 12000;
    const int32_t frames = num_frames;
    for (int i = 0; i < frames; ++i)
    {
        double angle = (2.0 * two_pi * 440.0 * (double)i) / (double)format.nSamplesPerSec;
        int16_t s = (int16_t)(sin(angle) * amp);
        pcm[2 * i] = s;
        pcm[2 * i + 1] = s;
    }

    crc = crc32_update(crc, (uint8_t *)ptr1, len1);
    hr = IDirectSoundBuffer_Unlock(secondary, ptr1, len1, ptr2, len2);
    if (FAILED(hr))
    {
        fprintf(stderr, "DSOUND Unlock failed: 0x%08lX\n", (unsigned long)hr);
        IDirectSoundBuffer_Release(secondary);
        IDirectSound8_Release(ds);
        CoUninitialize();
        return 9;
    }

    IDirectSoundBuffer_SetCurrentPosition(secondary, 0);
    hr = IDirectSoundBuffer_Play(secondary, 0, 0, 0);
    if (FAILED(hr))
    {
        fprintf(stderr, "DSOUND Play failed: 0x%08lX\n", (unsigned long)hr);
        IDirectSoundBuffer_Release(secondary);
        IDirectSound8_Release(ds);
        CoUninitialize();
        return 10;
    }

    DWORD status = 0;
    DWORD waited = 0;
    while ((status & DSBSTATUS_PLAYING) && waited < 3000)
    {
        Sleep(50);
        waited += 50;
        IDirectSoundBuffer_GetStatus(secondary, &status);
    }

    IDirectSoundBuffer_Stop(secondary);
    IDirectSoundBuffer_Release(secondary);
    IDirectSound8_Release(ds);
    CoUninitialize();

    char artifact_dir[MAX_PATH] = {0};
    if (!GetEnvironmentVariableA("MACRUNNER_AUDIO_TEST_OUT", artifact_dir, sizeof(artifact_dir)) ||
        strlen(artifact_dir) == 0)
    {
        strcpy(artifact_dir, ".");
    }

    char raw_path[MAX_PATH] = {0};
    snprintf(raw_path, sizeof(raw_path), "%s/mix_dsound.raw", artifact_dir);
    FILE *raw = fopen(raw_path, "wb");
    if (raw)
    {
        for (int i = 0; i < frames; ++i)
        {
            uint8_t bytes[4];
            memcpy(bytes, &pcm[2*i], 2);
            memcpy(bytes + 2, &pcm[2*i + 1], 2);
            fwrite(bytes, 1, 4, raw);
        }
        fclose(raw);
    }

    printf("DSOUND_BUFFER_MIX_OK\n");
    printf("DSOUND format=PCM channels=%u rate=%lu bits=%u frames=%u\n", format.nChannels, (unsigned long)format.nSamplesPerSec, format.wBitsPerSample, frames);
    printf("DSOUND checksum=0x%08X file=%s\n", crc ^ 0xFFFFFFFFu, raw_path);
    return 0;
}
