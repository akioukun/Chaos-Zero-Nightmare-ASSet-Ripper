#pragma once
#include <algorithm>
#include <fstream>
#include <string>

struct RipperOptions
{
    bool exportSctAsPng = true;
    bool exportDbAsJson = true;
    bool enableOpenFolder = false;
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
    }

    return options;
}
