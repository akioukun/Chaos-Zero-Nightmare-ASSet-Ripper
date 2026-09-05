#pragma once
#include "Core.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

struct RipperOptions
{
    /** Convert SCT files to PNG while extracting instead of writing the raw SCT. */
    bool exportSctAsPng = true;
    /** Convert database files to JSON while extracting instead of writing the raw database. */
    bool exportDbAsJson = true;
    /** Show the "Open Folder" button so a loose folder can be browsed like an archive. */
    bool enableOpenFolder = false;
    /** Folder the last archive, manifest or view-only folder was picked from. Empty until something has been opened. */
    std::string lastOpenDir;
    /** Folder last chosen as an extraction destination. Empty until something has been extracted. */
    std::string lastExtractDir;
    /** Folder a single exported file was last saved into. Empty until something has been exported. */
    std::string lastExportDir;
};

namespace RipperOptionsInternal
{
    inline std::string trimCopy(const std::string &value)
    {
        const std::string whitespace = " \t\r\n";
        const size_t start = value.find_first_not_of(whitespace);
        if (start == std::string::npos)
        {
            return "";
        }
        const size_t end = value.find_last_not_of(whitespace);
        return value.substr(start, end - start + 1);
    }

    inline bool parseBool(const std::string &value, bool defaultValue)
    {
        std::string normalized = trimCopy(value);
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::tolower);

        if (normalized == "1" || normalized == "true")
        {
            return true;
        }
        if (normalized == "0" || normalized == "false")
        {
            return false;
        }

        return defaultValue;
    }
}

inline void SaveRipperOptions(const RipperOptions &options, const std::string &iniPath = "czn_ripper.ini")
{
    std::ofstream out(iniPath, std::ios::trunc);
    if (!out.is_open())
    {
        return;
    }

    out << "[options]\n";
    out << "export_sct_as_png=" << (options.exportSctAsPng ? true : false) << "\n";
    out << "export_db_as_json=" << (options.exportDbAsJson ? true : false) << "\n";
    out << "enable_open_folder=" << (options.enableOpenFolder ? true : false) << "\n";
    out << "last_open_dir=" << options.lastOpenDir << "\n";
    out << "last_extract_dir=" << options.lastExtractDir << "\n";
    out << "last_export_dir=" << options.lastExportDir << "\n";
    out.flush();
}

inline RipperOptions LoadRipperOptions(const std::string &iniPath = "czn_ripper.ini")
{
    RipperOptions options;

    std::ifstream in(iniPath);
    if (!in.is_open())
    {
        SaveRipperOptions(options, iniPath);
        return options;
    }

    std::string line;
    while (std::getline(in, line))
    {
        line = RipperOptionsInternal::trimCopy(line);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[')
        {
            continue;
        }

        const size_t separator = line.find('=');
        if (separator == std::string::npos)
        {
            continue;
        }

        const std::string key = RipperOptionsInternal::trimCopy(line.substr(0, separator));
        const std::string value = RipperOptionsInternal::trimCopy(line.substr(separator + 1));

        if (key == "export_sct_as_png")
        {
            options.exportSctAsPng = RipperOptionsInternal::parseBool(value, options.exportSctAsPng);
        }
        else if (key == "export_db_as_json")
        {
            options.exportDbAsJson = RipperOptionsInternal::parseBool(value, options.exportDbAsJson);
        }
        else if (key == "enable_open_folder")
        {
            options.enableOpenFolder = RipperOptionsInternal::parseBool(value, options.enableOpenFolder);
        }
        else if (key == "last_open_dir")
        {
            options.lastOpenDir = value;
        }
        else if (key == "last_extract_dir")
        {
            options.lastExtractDir = value;
        }
        else if (key == "last_export_dir")
        {
            options.lastExportDir = value;
        }
    }

    return options;
}

/**
 * Returns the process-wide options, loaded from the ini the first time it is called.
 * Everything that persists an option goes through this, so saving one setting never drops the fields another part of the app owns.
 *
 * @returns Mutable reference to the shared options.
 */
inline RipperOptions &CurrentRipperOptions()
{
    static RipperOptions options = LoadRipperOptions();
    return options;
}

// //////////////////////////////////////////////////////////////////////////////////////////////////
// //////////////////////////////////////////////////////////////////////////////////////////////////
// Remembered dialog locations

namespace DialogPaths
{
    /** Which remembered location a file dialog should start from. */
    enum class Slot
    {
        /** Picking an archive, a manifest, or a folder to browse. */
        Open,
        /** Picking a destination folder for an extraction. */
        Extract,
        /** Saving a single file, and picking an older file map to diff against. */
        Export,
    };

    /**
     * Looks up the stored directory for a slot so callers can read it or write it back.
     *
     * @param slot The dialog kind whose remembered directory is wanted.
     * @returns Reference to the matching field inside the shared options.
     */
    inline std::string &SlotDir(Slot slot)
    {
        RipperOptions &options = CurrentRipperOptions();
        switch (slot)
        {
        case Slot::Extract:
            return options.lastExtractDir;
        case Slot::Export:
            return options.lastExportDir;
        case Slot::Open:
            break;
        }
        return options.lastOpenDir;
    }

    /**
     * Gives the directory a dialog should open in. Falls back to the working directory when nothing is remembered yet or when the remembered
     * folder is gone, which happens with a removed drive or an output folder that was deleted between runs.
     *
     * @param slot The dialog kind about to be shown.
     * @returns A usable directory path, never empty.
     */
    inline std::string StartDir(Slot slot)
    {
        const std::string &stored = SlotDir(slot);
        if (stored.empty())
        {
            return ".";
        }

        std::error_code ec;
        if (!std::filesystem::is_directory(std::filesystem::path(Core::Utf8ToWString(stored)), ec))
        {
            return ".";
        }
        return stored;
    }

    /**
     * Builds the default path for a save dialog by placing the suggested name inside the remembered directory.
     *
     * @param slot The dialog kind about to be shown.
     * @param defaultName The suggested file name, such as "filemap.json".
     * @returns A full path when a directory is remembered, otherwise just the name.
     */
    inline std::string StartPath(Slot slot, const std::string &defaultName)
    {
        const std::string dir = StartDir(slot);
        if (dir == ".")
        {
            return defaultName;
        }
        return dir + "/" + defaultName;
    }

    /**
     * Stores the directory a dialog just landed on and writes it to the ini so the next run starts there.
     *
     * @param slot The dialog kind that was just used.
     * @param picked The dialog result. A folder picker hands back the directory itself, open and save dialogs hand back a file path.
     * @param pickedIsFolder True when `picked` is already the directory to remember.
     */
    inline void Remember(Slot slot, const std::string &picked, bool pickedIsFolder)
    {
        if (picked.empty())
        {
            return;
        }

        std::string dir = picked;
        if (!pickedIsFolder)
        {
            const std::filesystem::path parent = std::filesystem::path(Core::Utf8ToWString(picked)).parent_path();
            if (parent.empty())
            {
                return;
            }
            dir = Core::PathToUtf8(parent);
        }

        std::string &stored = SlotDir(slot);
        if (stored == dir)
        {
            return;
        }

        stored = dir;
        SaveRipperOptions(CurrentRipperOptions());
    }
}
