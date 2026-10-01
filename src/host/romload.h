// Shared hash-verified loader, extracted from coreprobe. GPL-2.0-or-later.
#pragma once
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include "standard_romsets.h"
#include "sha256.h"
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
