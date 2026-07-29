/*
 * winaudioprobe — does sound actually come out of the speakers?
 *
 * Hollow Knight cannot answer this: it wedges at +58 s, long before audio init, so every
 * "FMOD error count = 0" we have measured means "never got there", not "no error". This
 * exercises the same stack in ~10 seconds instead: winmm -> mmdevapi -> winecoreaudio.drv ->
 * CoreAudio, which is exactly the path HK's FMOD takes.
 *
 * It plays three half-second tones (A4, C#5, E5) so the operator can hear it, and prints every
 * step with its return code so a silent run is still diagnosable from the log alone. Deliberately
 * uses waveOut rather than Beep(): Beep can be emulated by the PC-speaker path and would pass
 * without the audio driver being involved at all — the exact false positive worth avoiding here.
 *
 * Build:  winegcc --target=aarch64-windows tools/winaudioprobe.c -o winaudioprobe.exe -lwinmm
 * Run:    wine winaudioprobe.exe
 */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <math.h>

#define SAMPLE_RATE 44100
#define TONE_MS     500
#define N_TONES     3

static const double TONES[N_TONES] = { 440.0, 554.37, 659.25 };  /* A4, C#5, E5 */

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

int main(void)
{
    WAVEFORMATEX fmt;
    HWAVEOUT out = NULL;
    MMRESULT rc;
    UINT ndev;
    int t;

    say("winaudioprobe: start\n");

    ndev = waveOutGetNumDevs();
    say("winaudioprobe: waveOutGetNumDevs = %u\n", ndev);
    if (!ndev)
    {
        /* This is the failure HK reports as "FMOD failed to initialize any audio devices".
         * Zero devices means mmdevapi found no endpoints, which means no backend driver. */
        say("winaudioprobe: VERDICT=NO_DEVICES — no audio endpoint; nothing can play\n");
        return 2;
    }

    {
        WAVEOUTCAPSA caps;
        if (waveOutGetDevCapsA(0, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
            say("winaudioprobe: device0 = \"%s\" channels=%u\n", caps.szPname, caps.wChannels);
    }

    memset(&fmt, 0, sizeof(fmt));
    fmt.wFormatTag      = WAVE_FORMAT_PCM;
    fmt.nChannels       = 1;
    fmt.nSamplesPerSec  = SAMPLE_RATE;
    fmt.wBitsPerSample  = 16;
    fmt.nBlockAlign     = fmt.nChannels * fmt.wBitsPerSample / 8;
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

    rc = waveOutOpen(&out, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL);
    say("winaudioprobe: waveOutOpen rc=%u %s\n", rc, rc == MMSYSERR_NOERROR ? "(ok)" : "(FAILED)");
    if (rc != MMSYSERR_NOERROR)
    {
        say("winaudioprobe: VERDICT=OPEN_FAILED rc=%u\n", rc);
        return 3;
    }

    for (t = 0; t < N_TONES; t++)
    {
        const int nsamp = SAMPLE_RATE * TONE_MS / 1000;
        short *buf = HeapAlloc(GetProcessHeap(), 0, nsamp * sizeof(short));
        WAVEHDR hdr;
        int i;

        if (!buf) { say("winaudioprobe: alloc failed\n"); break; }
        for (i = 0; i < nsamp; i++)
        {
            /* Fade the edges so the tones do not click — a click is audible even when the
             * tone is not, and would make "I heard something" ambiguous. */
            double env = 1.0;
            if (i < 400)             env = i / 400.0;
            else if (i > nsamp - 400) env = (nsamp - i) / 400.0;
            buf[i] = (short)(12000.0 * env * sin(2.0 * M_PI * TONES[t] * i / SAMPLE_RATE));
        }

        memset(&hdr, 0, sizeof(hdr));
        hdr.lpData         = (LPSTR)buf;
        hdr.dwBufferLength = nsamp * sizeof(short);

        rc = waveOutPrepareHeader(out, &hdr, sizeof(hdr));
        if (rc != MMSYSERR_NOERROR) { say("winaudioprobe: prepare rc=%u\n", rc); break; }
        rc = waveOutWrite(out, &hdr, sizeof(hdr));
        say("winaudioprobe: tone %d (%.0f Hz) waveOutWrite rc=%u\n", t + 1, TONES[t], rc);
        if (rc != MMSYSERR_NOERROR) break;

        while (!(hdr.dwFlags & WHDR_DONE)) Sleep(20);
        waveOutUnprepareHeader(out, &hdr, sizeof(hdr));
        HeapFree(GetProcessHeap(), 0, buf);
        Sleep(150);
    }

    waveOutClose(out);
    say("winaudioprobe: VERDICT=PLAYED %d tones through waveOut — if you heard nothing, the\n"
        "               failure is below winmm (driver opened but produced no sound)\n", N_TONES);
    return 0;
}
