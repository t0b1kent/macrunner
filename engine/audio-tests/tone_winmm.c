#include <windows.h>
#include <mmsystem.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>

#ifndef SAMPLE_RATE
#define SAMPLE_RATE 44100
#endif

#ifndef DURATION_SECONDS
#define DURATION_SECONDS 2
#endif

#ifndef TONE_HZ
#define TONE_HZ 440
#endif

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

static void CALLBACK wave_callback(HWAVEOUT hwo, UINT msg, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR param2)
{
    (void)hwo;
    (void)param1;
    (void)param2;
    if (msg == WOM_DONE)
    {
        HANDLE event = (HANDLE)instance;
        if (event)
            SetEvent(event);
    }
}

int main(void)
{
    const double seconds = (double)DURATION_SECONDS;
    const int num_samples = (int)(SAMPLE_RATE * seconds);
    const int num_channels = 1;
    const int bytes_per_sample = sizeof(int16_t);
    const int payload = num_samples * num_channels * bytes_per_sample;

    WAVEFORMATEX fmt = {
        .wFormatTag = WAVE_FORMAT_PCM,
        .nChannels = num_channels,
        .nSamplesPerSec = SAMPLE_RATE,
        .nAvgBytesPerSec = SAMPLE_RATE * num_channels * bytes_per_sample,
        .nBlockAlign = num_channels * bytes_per_sample,
        .wBitsPerSample = 16,
        .cbSize = 0,
    };

    HWAVEOUT wave_out = NULL;
    HANDLE done = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!done)
    {
        fprintf(stderr, "winmm-waveout: failed to create event\n");
        return 3;
    }

    MMRESULT open_err = waveOutOpen(&wave_out, WAVE_MAPPER, &fmt, (DWORD_PTR)wave_callback,
                                   (DWORD_PTR)done, CALLBACK_FUNCTION);
    if (open_err != MMSYSERR_NOERROR)
    {
        fprintf(stderr, "winmm-waveout: waveOutOpen failed: %u\n", open_err);
        CloseHandle(done);
        return 4;
    }

    int16_t *samples = (int16_t *)malloc((size_t)payload);
    if (!samples)
    {
        fprintf(stderr, "winmm-waveout: malloc samples failed\n");
        waveOutClose(wave_out);
        CloseHandle(done);
        return 5;
    }

    for (int i = 0; i < num_samples; ++i)
    {
        double phase = (2.0 * 3.14159265358979323846 * TONE_HZ * (double)i) / (double)SAMPLE_RATE;
        double value = sin(phase) * 0.5;
        int s = (int)llround(value * 32767.0);
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        samples[i] = (int16_t)s;
    }

    uint32_t crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, samples, (size_t)payload) ^ 0xFFFFFFFFu;

    char artifact_dir[MAX_PATH] = {0};
    DWORD dir_len = GetEnvironmentVariableA("MACRUNNER_AUDIO_TEST_OUT", artifact_dir, sizeof(artifact_dir));
    if (!dir_len || dir_len >= sizeof(artifact_dir) - 1)
        strcpy(artifact_dir, ".");

    char raw_path[MAX_PATH] = {0};
    snprintf(raw_path, sizeof(raw_path), "%s/tone_winmm.raw", artifact_dir);
    FILE *raw = fopen(raw_path, "wb");
    if (raw)
    {
        fwrite(samples, 1, (size_t)payload, raw);
        fclose(raw);
    }

    WAVEHDR header = {0};
    header.lpData = (LPSTR)samples;
    header.dwBufferLength = (DWORD)payload;

    MMRESULT prep_err = waveOutPrepareHeader(wave_out, &header, sizeof(header));
    if (prep_err != MMSYSERR_NOERROR)
    {
        fprintf(stderr, "winmm-waveout: prepare header failed: %u\n", prep_err);
        free(samples);
        waveOutClose(wave_out);
        CloseHandle(done);
        return 6;
    }

    MMRESULT write_err = waveOutWrite(wave_out, &header, sizeof(header));
    if (write_err != MMSYSERR_NOERROR)
    {
        fprintf(stderr, "winmm-waveout: write failed: %u\n", write_err);
        waveOutUnprepareHeader(wave_out, &header, sizeof(header));
        free(samples);
        waveOutClose(wave_out);
        CloseHandle(done);
        return 7;
    }

    ResetEvent(done);
    DWORD wait_rc = WaitForSingleObject(done, 4000);

    waveOutReset(wave_out);
    waveOutUnprepareHeader(wave_out, &header, sizeof(header));
    waveOutClose(wave_out);
    CloseHandle(done);
    free(samples);

    if (wait_rc != WAIT_OBJECT_0)
    {
        fprintf(stderr, "winmm-waveout: playback timeout waiting for WOM_DONE\n");
        return 8;
    }

    double duration_ms = (double)num_samples * 1000.0 / (double)SAMPLE_RATE;
    printf("WINMM_TONE_OK\n");
    printf("FORMAT tag=1 channels=%u rate=%lu bits=%u\n", fmt.nChannels, (unsigned long)fmt.nSamplesPerSec, fmt.wBitsPerSample);
    printf("WINMM_TONE duration_ms=%.3f checksum=0x%08X bytes=%u file=%s\n", duration_ms, crc, payload, raw_path);
    return 0;
}
