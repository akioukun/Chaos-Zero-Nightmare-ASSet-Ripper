#pragma once
#include <algorithm>
#include <fstream>
#include <string>

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

// Writing the whole file each time means a caller that only owns some of the fields must load the current options first, change what it owns, and save
// that. Default-constructing a RipperOptions and saving it would reset every field the caller did not set.
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
