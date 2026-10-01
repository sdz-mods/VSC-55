/* VSC-55 waveOut probe; accepts coreprobe's canonical stereo PCM16 WAV.
 * Linear resampling to 48 kHz is a diagnostic baseline, not final audio DSP.
 * SPDX-License-Identifier: MIT */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define BLOCK 960
#define BLOCKS 4
static unsigned read32(const unsigned char *p)
{
    return p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}
int main(int argc, char **argv)
{
    FILE *f;
    unsigned char head[44];
    short *input;
    unsigned frames, rate, bytes, outframes, pos = 0, i, j, queued = 0, completed = 0;
    HWAVEOUT out = 0;
    HANDLE event;
    WAVEFORMATEX format;
    WAVEHDR headers[BLOCKS];
    short buffers[BLOCKS][BLOCK * 2];
    MMRESULT result;
    int failed = 0;

    if (argc != 2)
    {
        puts("wavplay <coreprobe.wav>");
        return 2;
    }

    f = fopen(argv[1], "rb");

    if (!f)
        return 1;

    if (fread(head, 1, 44, f) != 44 || memcmp(head, "RIFF", 4) || memcmp(head + 8, "WAVEfmt ", 8) ||
            read32(head + 16) != 16 || head[20] != 1 || head[22] != 2 || head[34] != 16 || memcmp(head + 36, "data", 4))
    {
        fclose(f);
        puts("Unsupported WAV");
        return 1;
    }

    bytes = read32(head + 40);
    rate = read32(head + 24);
    frames = bytes / 4;

    if (!frames || bytes % 4 || bytes > 64 * 1024 * 1024 || rate < 8000 || rate > 192000)
    {
        fclose(f);
        return 1;
    }

    input = (short *)malloc(bytes);

    if (!input)
    {
        fclose(f);
        return 1;
    }

    if (fread(input, 1, bytes, f) != bytes)
    {
        fclose(f);
        free(input);
        return 1;
    }

    fclose(f);
    outframes = (unsigned)((double)frames * 48000 / rate);
    event = CreateEvent(NULL, FALSE, FALSE, NULL);

    if (!event)
    {
        free(input);
        return 1;
    }

    memset(&format, 0, sizeof(format));
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = 48000;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 4;
    format.nAvgBytesPerSec = 192000;
    result = waveOutOpen(&out, WAVE_MAPPER, &format, (DWORD_PTR)event, 0, CALLBACK_EVENT);

    if (result)
    {
        printf("waveOutOpen=%u\n", result);
        CloseHandle(event);
        free(input);
        return 1;
    }

    memset(headers, 0, sizeof(headers));

    for (i = 0; i < BLOCKS; i++)
    {
        headers[i].lpData = (char *)buffers[i];
        headers[i].dwBufferLength = sizeof(buffers[i]);
        result = waveOutPrepareHeader(out, &headers[i], sizeof(WAVEHDR));

        if (result)
        {
            failed = 1;
            break;
        }
    }

    while (!failed && (pos < outframes || queued))
    {
        for (i = 0; i < BLOCKS; i++)
        {
            unsigned count;

            if (headers[i].dwFlags & WHDR_INQUEUE)
                continue;

            if (headers[i].dwFlags & WHDR_DONE)
            {
                --queued;
                ++completed;
                headers[i].dwFlags &= ~(DWORD)WHDR_DONE;
            }

            if (pos >= outframes)
                continue;

            count = outframes - pos;

            if (count > BLOCK)
                count = BLOCK;

            for (j = 0; j < count; j++)
            {
                double at = (double)(pos + j) * rate / 48000;
                unsigned index = (unsigned)at, next = index + 1;
                double frac = at - index;
                unsigned ch;

                if (next >= frames)
                    next = frames - 1;

                for (ch = 0; ch < 2; ch++)
                    buffers[i][j * 2 + ch] = (short)(input[index * 2 + ch] + (input[next * 2 + ch] - input[index * 2 + ch]) * frac);
            }

            headers[i].dwBufferLength = count * 4;
            result = waveOutWrite(out, &headers[i], sizeof(WAVEHDR));

            if (result)
            {
                printf("waveOutWrite=%u\n", result);
                failed = 1;
                break;
            }

            pos += count;
            ++queued;
        }

        if (!failed && queued && WaitForSingleObject(event, 5000) != WAIT_OBJECT_0)
        {
            puts("Audio completion timeout");
            failed = 1;
        }
    }

    waveOutReset(out);

    for (i = 0; i < BLOCKS; i++)
        if (headers[i].dwFlags & WHDR_PREPARED)
            if (waveOutUnprepareHeader(out, &headers[i], sizeof(WAVEHDR)))
                failed = 1;

    if (waveOutClose(out))
        failed = 1;

    CloseHandle(event);
    free(input);
    printf("%s waveOut blocks completed=%u\n", failed ? "FAIL" : "PASS", completed);
    return failed;
}
