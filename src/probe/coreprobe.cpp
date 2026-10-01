// VSC-55 deterministic backend probe. GPL-2.0-or-later.
// Links the pinned, unmodified jcmoyer backend. No SDL or audio device needed.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <stdexcept>
#include "emu.h"
#include "standard_romsets.h"
#include "sha256.h"

static void require(bool ok, const char *message)
{
    if (!ok)
        throw std::runtime_error(message);
}
static void le16(FILE *f, unsigned v)
{
    fputc(v & 255, f);
    fputc((v >> 8) & 255, f);
}
static void le32(FILE *f, unsigned v)
{
    le16(f, v);
    le16(f, v >> 16);
}
static void wav_header(FILE *f, unsigned rate, unsigned frames)
{
    fwrite("RIFF", 1, 4, f);
    le32(f, 36 + frames * 4);
    fwrite("WAVEfmt ", 1, 8, f);
    le32(f, 16);
    le16(f, 1);
    le16(f, 2);
    le32(f, rate);
    le32(f, rate * 4);
    le16(f, 4);
    le16(f, 16);
    fwrite("data", 1, 4, f);
    le32(f, frames * 4);
}
struct Capture
{
    unsigned frames = 0, target = 0;
    std::vector<AudioFrame<int16_t>> audio;
    SHA256Context raw_hash{};
    static void sample(void *ptr, const AudioFrame<int32_t> &in)
    {
        auto &self = *static_cast<Capture *>(ptr);

        if (self.frames >= self.target)
            return;

        // Exact core frame stream, before normalization/resampling. Both builds little endian.
        SHA256Input(&self.raw_hash, reinterpret_cast<const uint8_t *>(&in), sizeof(in));
        AudioFrame<int16_t> out;
        Normalize(in, out);
        self.audio.push_back(out);
        ++self.frames;
    }
};
static bool load_directory(const char *directory, const RomsetDefinition &def, RomsetInfo &info)
{
    char pattern[MAX_PATH];

    if (strlen(directory) + 3 >= sizeof(pattern))
        return false;

    snprintf(pattern, sizeof(pattern), "%s\\*", directory);
    WIN32_FIND_DATAA entry;
    HANDLE find = FindFirstFileA(pattern, &entry);

    if (find == INVALID_HANDLE_VALUE)
        return false;

    do
    {
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;

        if (entry.nFileSizeHigh || entry.nFileSizeLow > 16 * 1024 * 1024)
            continue;

        char path[MAX_PATH];

        if (strlen(directory) + strlen(entry.cFileName) + 2 >= sizeof(path))
            continue;

        snprintf(path, sizeof(path), "%s\\%s", directory, entry.cFileName);
        FILE *f = fopen(path, "rb");

        if (!f)
            continue;

        std::vector<uint8_t> bytes(entry.nFileSizeLow);
        bool ok = fread(bytes.data(), 1, bytes.size(), f) == bytes.size();
        fclose(f);
        SHA256_Digest hash;

        if (!ok || !SHA256_HashBytes(bytes, hash))
            continue;

        for (auto location : def.GetValidLocations())
            if (hash == def.GetHash(location))
            {
                info.rom_data[static_cast<size_t>(location)] = bytes;
                printf("ROM %s <- %s (%lu bytes)\n", ToCString(location), entry.cFileName, (unsigned long)bytes.size());
            }
    }
    while (FindNextFileA(find, &entry));

    FindClose(find);

    for (auto location : def.GetValidLocations())
        if (info.rom_data[static_cast<size_t>(location)].empty())
        {
            printf("MISSING %s\n", ToCString(location));
            return false;
        }

    // Upstream performs wave-ROM unscrambling. Invoke exactly once.
    return LoadRomset(info, nullptr);
}
int main(int argc, char **argv)
{
    printf("BUILD_PROFILE=%s\n", VSC55_BUILD_PROFILE);

    if (argc == 2 && !strcmp(argv[1], "--list"))
    {
        for (const auto &def : GetStandardRomsetDefinitions())
            puts(def.name);

        return 0;
    }

    if (argc != 4)
    {
        puts("coreprobe <romset-name> <ROM-directory> <output.wav>\nUse --list for all supported sets.");
        return 2;
    }

    try
    {
        const RomsetDefinition *chosen = nullptr;

        for (const auto &def : GetStandardRomsetDefinitions())
            if (!strcmp(argv[1], def.name))
                chosen = &def;

        require(chosen != nullptr, "Unknown ROM set");
        RomsetInfo info;
        require(load_directory(argv[2], *chosen, info), "Missing, unreadable or mismatched ROMs");
        Emulator emu;
        require(emu.Init({}), "Emulator initialization failed");
        require(emu.LoadRoms(chosen->romset, info), "Core ROM load failed");
        emu.Reset();
        const unsigned rate = PCM_GetOutputFrequency(emu.GetPCM());
        Capture capture;
        capture.target = rate * 8;
        capture.audio.reserve(capture.target);
        SHA256Reset(&capture.raw_hash);
        emu.SetSampleCallback(Capture::sample, &capture);
        const DWORD started = GetTickCount();
        unsigned watchdog_steps = 0;
        auto until = [&](unsigned frame)
        {
            while (capture.frames < frame)
            {
                emu.Step();

                if ((++watchdog_steps & 0xffff) == 0 && DWORD(GetTickCount() - started) > 120000)
                    throw std::runtime_error("Render watchdog expired");
            }
        };
        until(rate * 3); // firmware boot, no wall-clock sleeps
        const uint8_t notes[] = {0xC0, 0x00, 0x90, 60, 100, 0x90, 64, 100, 0x90, 67, 100};
        emu.PostMIDI(notes);
        until(rate * 5);
        const uint8_t off[] = {0x80, 60, 0, 0x80, 64, 0, 0x80, 67, 0};
        emu.PostMIDI(off);
        until(capture.target);
        DWORD elapsed = GetTickCount() - started;
        unsigned nonzero = 0;
        int peak = 0;

        for (const auto &sample : capture.audio)
        {
            if (sample.left || sample.right)
                ++nonzero;

            peak = std::max(peak, std::max(std::abs(int(sample.left)), std::abs(int(sample.right))));
        }

        require(nonzero > 0, "Rendered audio is entirely silent");
        SHA256_Digest hash;
        SHA256Result(&capture.raw_hash, hash.data());
        printf("CORE_SHA256 ");

        for (auto x : hash)
            printf("%02x", x);

        puts("");
        printf("MODEL=%s RATE=%u FRAMES=%u RENDER_MS=%lu REALTIME_FACTOR=%.3f\n",
               chosen->name, rate, capture.frames, (unsigned long)elapsed, 8000.0 / (elapsed ? elapsed : 1));
        printf("NONZERO_FRAMES=%u PEAK_PCM16=%d\n", nonzero, peak);
        FILE *f = fopen(argv[3], "wb");
        require(f != nullptr, "Cannot create output WAV");
        wav_header(f, rate, capture.frames);
        bool written = fwrite(capture.audio.data(), sizeof(AudioFrame<int16_t>), capture.frames, f) == capture.frames;
        int closed = fclose(f);
        require(written && closed == 0, "Output WAV write failed");
        puts("PASS: ROM hashes, boot/render and WAV write. Hardware playback not tested here.");
        return 0;
    }
    catch (const std::exception &e)
    {
        fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
