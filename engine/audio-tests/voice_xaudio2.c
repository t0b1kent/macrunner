#define COBJMACROS
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <xaudio2.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>

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
    HMODULE hxaudio = LoadLibraryA("xaudio2_9.dll");
    if (!hxaudio)
        hxaudio = LoadLibraryA("xaudio2_8.dll");
    if (!hxaudio)
        hxaudio = LoadLibraryA("xaudio2_7.dll");
    if (!hxaudio)
    {
        fprintf(stderr, "XAUDIO2: failed to load xaudio2_7/8/9 dll\n");
        return 2;
    }

    typedef HRESULT (WINAPI *XAudio2CreateProc)(IXAudio2 **, UINT32, XAUDIO2_PROCESSOR);
    XAudio2CreateProc XAudio2Create = (XAudio2CreateProc)GetProcAddress(hxaudio, "XAudio2Create");
    if (!XAudio2Create)
    {
        fprintf(stderr, "XAUDIO2: XAudio2Create missing\n");
        FreeLibrary(hxaudio);
        return 3;
    }

    IXAudio2 *engine = NULL;
    HRESULT hr = XAudio2Create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !engine)
    {
        fprintf(stderr, "XAUDIO2: XAudio2Create failed: 0x%08lX\n", (unsigned long)hr);
        FreeLibrary(hxaudio);
        return 4;
    }

    IXAudio2MasteringVoice *master = NULL;
    hr = IXAudio2_CreateMasteringVoice(engine, &master, 2, 44100, 0, NULL, NULL, AudioCategory_GameEffects);
    if (FAILED(hr))
    {
        fprintf(stderr, "XAUDIO2: CreateMasteringVoice failed: 0x%08lX\n", (unsigned long)hr);
        IXAudio2_Release(engine);
        FreeLibrary(hxaudio);
        return 5;
    }

    WAVEFORMATEX format = {0};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = 44100;
    format.wBitsPerSample = 16;
    format.nBlockAlign = (format.nChannels * format.wBitsPerSample) / 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    IXAudio2SourceVoice *voice = NULL;
    hr = IXAudio2_CreateSourceVoice(engine, &voice, &format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
    if (FAILED(hr))
    {
        fprintf(stderr, "XAUDIO2: CreateSourceVoice failed: 0x%08lX\n", (unsigned long)hr);
        IXAudio2MasteringVoice_DestroyVoice(master);
        IXAudio2_Release(engine);
        FreeLibrary(hxaudio);
        return 6;
    }

    const int frames = 88200 / 2; /* 1 sec */
    int16_t *samples = (int16_t *)malloc((size_t)frames * format.nBlockAlign);
    if (!samples)
    {
        fprintf(stderr, "XAUDIO2: failed to allocate tone\n");
        IXAudio2SourceVoice_DestroyVoice(voice);
        IXAudio2MasteringVoice_DestroyVoice(master);
        IXAudio2_Release(engine);
        FreeLibrary(hxaudio);
        return 7;
    }

    const double two_pi = 6.28318530717958647692;
    const int16_t amp = 12000;
    for (int i = 0; i < frames; ++i)
    {
        double angle = (2.0 * two_pi * 440.0 * (double)i) / (double)format.nSamplesPerSec;
        int16_t s = (int16_t)(sin(angle) * amp);
        samples[2 * i] = s;
        samples[2 * i + 1] = s;
    }

    uint32_t crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, samples, (size_t)frames * format.nBlockAlign) ^ 0xFFFFFFFFu;

    XAUDIO2_BUFFER buffer = {0};
    buffer.AudioBytes = frames * format.nBlockAlign;
    buffer.pAudioData = (const BYTE *)samples;
    buffer.Flags = XAUDIO2_END_OF_STREAM;

    hr = IXAudio2SourceVoice_SubmitSourceBuffer(voice, &buffer, NULL);
    if (FAILED(hr))
    {
        fprintf(stderr, "XAUDIO2: SubmitSourceBuffer failed: 0x%08lX\n", (unsigned long)hr);
        free(samples);
        IXAudio2SourceVoice_DestroyVoice(voice);
        IXAudio2MasteringVoice_DestroyVoice(master);
        IXAudio2_Release(engine);
        FreeLibrary(hxaudio);
        return 8;
    }

    hr = IXAudio2SourceVoice_Start(voice, 0, XAUDIO2_COMMIT_NOW);
    if (FAILED(hr))
    {
        fprintf(stderr, "XAUDIO2: SourceVoice_Start failed: 0x%08lX\n", (unsigned long)hr);
        free(samples);
        IXAudio2SourceVoice_DestroyVoice(voice);
        IXAudio2MasteringVoice_DestroyVoice(master);
        IXAudio2_Release(engine);
        FreeLibrary(hxaudio);
        return 9;
    }

    Sleep(1200);

    IXAudio2SourceVoice_DestroyVoice(voice);
    IXAudio2MasteringVoice_DestroyVoice(master);
    IXAudio2_Release(engine);
    FreeLibrary(hxaudio);

    char artifact_dir[MAX_PATH] = {0};
    if (!GetEnvironmentVariableA("MACRUNNER_AUDIO_TEST_OUT", artifact_dir, sizeof(artifact_dir)) ||
        strlen(artifact_dir) == 0)
    {
        strcpy(artifact_dir, ".");
    }

    char raw_path[MAX_PATH] = {0};
    snprintf(raw_path, sizeof(raw_path), "%s/voice_xaudio2.raw", artifact_dir);
    FILE *raw = fopen(raw_path, "wb");
    if (raw)
    {
        fwrite(samples, 1, (size_t)frames * format.nBlockAlign, raw);
        fclose(raw);
    }

    free(samples);
    printf("XAUDIO2_SOURCE_VOICE_OK\n");
    printf("XAUDIO2 format=PCM channels=%u rate=%lu bits=%u frames=%d\n",
           format.nChannels, (unsigned long)format.nSamplesPerSec, format.wBitsPerSample, frames);
    printf("XAUDIO2 checksum=0x%08X file=%s\n", crc, raw_path);
    return 0;
}
