#define _WIN32_WINNT 0x0601
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static void print_hr(const char *msg, HRESULT hr)
{
    fprintf(stderr, "%s: 0x%08lx\n", msg, (unsigned long)hr);
}

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
        print_hr("CoInitializeEx", hr);
        return 2;
    }

    IMMDeviceEnumerator *enumerator = NULL;
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void **)&enumerator);
    if (FAILED(hr) || !enumerator)
    {
        print_hr("CoCreateInstance(CLSID_MMDeviceEnumerator)", hr);
        CoUninitialize();
        return 3;
    }

    IMMDevice *device = NULL;
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eMultimedia, &device);
    IMMDeviceEnumerator_Release(enumerator);
    if (FAILED(hr) || !device)
    {
        print_hr("GetDefaultAudioEndpoint", hr);
        CoUninitialize();
        return 4;
    }

    IAudioClient *client = NULL;
    hr = IMMDevice_Activate(device, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&client);
    IMMDevice_Release(device);
    if (FAILED(hr) || !client)
    {
        print_hr("IMMDevice::Activate(IAudioClient)", hr);
        CoUninitialize();
        return 5;
    }

    WAVEFORMATEX *mix = NULL;
    hr = IAudioClient_GetMixFormat(client, &mix);
    if (FAILED(hr) || !mix)
    {
        print_hr("IAudioClient::GetMixFormat", hr);
        IAudioClient_Release(client);
        CoUninitialize();
        return 6;
    }

    REFERENCE_TIME period_min = 0;
    REFERENCE_TIME period_default = 0;
    hr = IAudioClient_GetDevicePeriod(client, &period_default, &period_min);
    if (FAILED(hr))
    {
        print_hr("IAudioClient::GetDevicePeriod", hr);
        CoTaskMemFree(mix);
        IAudioClient_Release(client);
        CoUninitialize();
        return 7;
    }

    WAVEFORMATEX preferred = {
        .wFormatTag = WAVE_FORMAT_PCM,
        .nChannels = 2,
        .nSamplesPerSec = 44100,
        .wBitsPerSample = 16,
    };
    preferred.nBlockAlign = (preferred.nChannels * preferred.wBitsPerSample) / 8;
    preferred.nAvgBytesPerSec = preferred.nSamplesPerSec * preferred.nBlockAlign;

    WAVEFORMATEX *format = &preferred;
    WAVEFORMATEX *closest = NULL;
    BOOL free_format = FALSE;

    hr = IAudioClient_IsFormatSupported(client, AUDCLNT_SHAREMODE_SHARED, &preferred, &closest);
    fprintf(stderr, "MMDEVAPI_FORMAT_SUPPORT preferred rc=0x%08lx\n", (unsigned long)hr);
    if (hr == S_FALSE)
    {
        if (!closest)
        {
            fprintf(stderr, "IAudioClient::IsFormatSupported returned S_FALSE with NULL closest format\n");
            CoTaskMemFree(mix);
            IAudioClient_Release(client);
            CoUninitialize();
            return 8;
        }
        format = closest;
        free_format = TRUE;
        fprintf(stderr, "MMDEVAPI_FORMAT_SELECTED closest tag=%u ch=%u rate=%u bits=%u block=%u avg=%u\n",
                (unsigned)format->wFormatTag, (unsigned)format->nChannels,
                (unsigned)format->nSamplesPerSec, (unsigned)format->wBitsPerSample,
                (unsigned)format->nBlockAlign, (unsigned)format->nAvgBytesPerSec);
    }
    else if (hr == S_OK)
    {
        fprintf(stderr, "MMDEVAPI_FORMAT_SELECTED preferred tag=%u ch=%u rate=%u bits=%u block=%u avg=%u\n",
                (unsigned)format->wFormatTag, (unsigned)format->nChannels,
                (unsigned)format->nSamplesPerSec, (unsigned)format->wBitsPerSample,
                (unsigned)format->nBlockAlign, (unsigned)format->nAvgBytesPerSec);
    }
    else if (FAILED(hr))
    {
        print_hr("IAudioClient::IsFormatSupported", hr);
        if (closest)
            CoTaskMemFree(closest);
        CoTaskMemFree(mix);
        IAudioClient_Release(client);
        CoUninitialize();
        return 8;
    }

    hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, 0, 0, 0, format, NULL);
    if (FAILED(hr))
    {
        print_hr("IAudioClient::Initialize", hr);
        if (free_format)
            CoTaskMemFree(format);
        CoTaskMemFree(mix);
        IAudioClient_Release(client);
        CoUninitialize();
        return 9;
    }

    UINT32 buffer_frames = 0;
    IAudioClient_GetBufferSize(client, &buffer_frames);

    IAudioRenderClient *render = NULL;
    hr = IAudioClient_GetService(client, &IID_IAudioRenderClient, (void **)&render);
    if (FAILED(hr) || !render)
    {
        print_hr("IAudioClient::GetService(IAudioRenderClient)", hr);
        if (free_format)
            CoTaskMemFree(format);
        CoTaskMemFree(mix);
        IAudioClient_Release(client);
        CoUninitialize();
        return 10;
    }

    hr = IAudioClient_Start(client);
    if (FAILED(hr))
    {
        print_hr("IAudioClient::Start", hr);
        IAudioRenderClient_Release(render);
        if (free_format)
            CoTaskMemFree(format);
        CoTaskMemFree(mix);
        IAudioClient_Release(client);
        CoUninitialize();
        return 11;
    }

    const UINT32 rate = format->nSamplesPerSec;
    const UINT32 channels = format->nChannels;
    const UINT32 block_align = format->nBlockAlign;
    const UINT32 target_frames = rate / 1; /* 1 second */
    const double two_pi = 6.28318530717958647692;

    char artifact_dir[MAX_PATH] = {0};
    if (!GetEnvironmentVariableA("MACRUNNER_AUDIO_TEST_OUT", artifact_dir, sizeof(artifact_dir)) ||
        strlen(artifact_dir) == 0)
        strcpy(artifact_dir, ".");

    char raw_path[MAX_PATH] = {0};
    snprintf(raw_path, sizeof(raw_path), "%s/render_mmdevapi.raw", artifact_dir);
    FILE *raw = fopen(raw_path, "wb");
    if (!raw)
        fprintf(stderr, "Unable to create raw output: %s\n", raw_path);

    uint32_t crc = 0xFFFFFFFFu;
    UINT32 produced = 0;
    while (produced < target_frames)
    {
        UINT32 padding = 0;
        hr = IAudioClient_GetCurrentPadding(client, &padding);
        if (FAILED(hr))
            break;

        UINT32 writable = buffer_frames > padding ? (buffer_frames - padding) : 0;
        if (writable == 0)
        {
            Sleep(1);
            continue;
        }

        UINT32 remain = target_frames - produced;
        UINT32 request = writable < remain ? writable : remain;
        BYTE *dst = NULL;
        hr = IAudioRenderClient_GetBuffer(render, request, &dst);
        if (FAILED(hr))
            break;

        for (UINT32 i = 0; i < request; ++i)
        {
            double t = (double)(produced + i) / (double)rate;
            double sample = sin(2.0 * two_pi * 660.0 * t);
            if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && format->wBitsPerSample == 32)
            {
                float f = (float)(sample * 0.2f);
                for (UINT32 ch = 0; ch < channels; ++ch)
                    ((float *)dst)[(i * channels) + ch] = f;
                crc = crc32_update(crc, (const uint8_t *)&f, sizeof(f));
            }
            else
            {
                int16_t s = (int16_t)(sample * 10000.0);
                for (UINT32 ch = 0; ch < channels; ++ch)
                    ((int16_t *)dst)[(i * channels) + ch] = s;
                crc = crc32_update(crc, (const uint8_t *)&s, sizeof(s));
            }
        }

        if (raw)
            fwrite(dst, 1, request * block_align, raw);
        IAudioRenderClient_ReleaseBuffer(render, request, 0);
        produced += request;
    }

    if (raw)
        fclose(raw);

    IAudioClient_Stop(client);
    IAudioRenderClient_Release(render);
    IAudioClient_Release(client);
    CoTaskMemFree(mix);
    if (free_format)
        CoTaskMemFree(format);
    CoUninitialize();

    double latency_buffer_ms = (double)buffer_frames * 1000.0 / (double)rate;
    double period_default_ms = (double)period_default / 10000.0;
    double period_min_ms = (double)period_min / 10000.0;

    printf("MMDEVAPI_RENDER_OK\n");
    printf("MMDEVAPI_NEGOTIATED format=0x%u channels=%u rate=%u bits=%u block=%u avg=%u\n",
           (unsigned)format->wFormatTag,
           (unsigned)format->nChannels,
           (unsigned)format->nSamplesPerSec,
           (unsigned)format->wBitsPerSample,
           (unsigned)format->nBlockAlign,
           (unsigned)format->nAvgBytesPerSec);
    printf("MMDEVAPI_PERIOD default_hns=%I64d min_hns=%I64d buffer_frames=%u latency_default_ms=%.3f latency_min_ms=%.3f latency_buffer_ms=%.3f\n",
           (long long)period_default, (long long)period_min, (unsigned)buffer_frames,
           period_default_ms, period_min_ms, latency_buffer_ms);
    printf("MMDEVAPI_CHECKSUM=0x%08X frames=%u bytes=%u file=%s\n",
           crc ^ 0xFFFFFFFFu, (unsigned)produced, (unsigned)(produced * block_align), raw_path);

    return (produced == target_frames) ? 0 : 12;
}
