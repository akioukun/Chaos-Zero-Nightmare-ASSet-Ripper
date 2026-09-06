#pragma once
#include "IArchive.h"
#include <memory>
#include <string>

/**
 * Picks the right IArchive implementation for a pack path. Returns an SSRArchive for a manifest.ssra, a CompositeArchive (a DataPack plus every manifest.ssra
 * found under a sibling gameres directory) when gameres exists, or a plain DataPack otherwise.
 * @param wpath Path to data.pack, a manifest.ssra, or an unpacked directory.
 * @returns An unscanned archive. The caller must still call Scan() before the file tree exists.
 */
std::unique_ptr<IArchive> CreateArchive(const std::wstring& wpath);
