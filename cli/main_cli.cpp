#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include "core/Core.h"
#include "core/FileTree.h"
#include "core/Logger.h"
#include "archive/IArchive.h"
#include "archive/ArchiveFactory.h"

// //////////////////////////////////////////////////////////////////////////////////////////////////
// //////////////////////////////////////////////////////////////////////////////////////////////////
// Constants

// Exit codes. Scripts can tell "nothing worked" apart from "most of it worked".
static constexpr int CLI_EXIT_OK = 0;
static constexpr int CLI_EXIT_USAGE = 1;
static constexpr int CLI_EXIT_PACK_FAILED = 2;
static constexpr int CLI_EXIT_PARTIAL_FAILURE = 3;
static constexpr int CLI_EXIT_ALL_FAILED = 4;

// Returned by parse_args when help was printed, so main can exit successfully without doing any work.
static constexpr int CLI_PARSE_HELP = -1;

/** Parsed command line for a single CLI run. */
struct CliOptions
{
    /** UTF-8 path to data.pack, a manifest.ssra, or an unpacked directory. */
    std::string pack;
    /** UTF-8 path to the destination directory. Created if it does not exist. */
    std::string out;
    /** Normalized archive-relative folder (or file) paths to extract, in the order given, without duplicates. */
    std::vector<std::string> folders;
};

// //////////////////////////////////////////////////////////////////////////////////////////////////
// //////////////////////////////////////////////////////////////////////////////////////////////////
// Path helpers

/**
 * Normalizes an archive path argument into the form the file tree stores in `full_path`: forward slashes, no leading or trailing slash, no empty segments.
 * @param raw The raw --folder value as typed by the user.
 * @returns The normalized path, or an empty string when nothing usable is left.
 */
static std::string normalize_archive_path(const std::string& raw)
{
    std::string normalized;
    normalized.reserve(raw.size());

    bool last_was_slash = true; // Treat the start as a slash so a leading separator gets dropped.
    for (char c : raw)
    {
        const char ch = (c == '\\') ? '/' : c;
        if (ch == '/')
        {
            if (last_was_slash)
                continue;
            last_was_slash = true;
            normalized.push_back('/');
            continue;
        }
        last_was_slash = false;
        normalized.push_back(ch);
    }

    while (!normalized.empty() && normalized.back() == '/')
        normalized.pop_back();

    return normalized;
}

// //////////////////////////////////////////////////////////////////////////////////////////////////
// //////////////////////////////////////////////////////////////////////////////////////////////////
// Argument parsing

/**
 * Collects the process arguments as UTF-8. On Windows the ANSI argv would mangle paths outside the active code page, so the wide command line is used instead.
 * @param argc Argument count from main.
 * @param argv Argument vector from main.
 * @returns The arguments after argv[0], each encoded as UTF-8.
 */
static std::vector<std::string> collect_args_utf8(int argc, char** argv)
{
    std::vector<std::string> args;
#ifdef _WIN32
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv != nullptr)
    {
        for (int i = 1; i < wargc; ++i)
            args.push_back(Core::WStringToUtf8(wargv[i]));
        LocalFree(wargv);
        return args;
    }
#endif
    for (int i = 1; i < argc; ++i)
        args.push_back(argv[i]);
    return args;
}

/** Prints the usage text. */
static void print_help()
{
    std::cout <<
        "czn-cli - headless extractor for Chaos Zero Nightmare data.pack\n"
        "\n"
        "Usage:\n"
        "  czn-cli --pack <path> --out <dir> --folder <archive/path> [--folder ...]\n"
        "\n"
        "Required:\n"
        "  -p, --pack <path>      data.pack, manifest.ssra, or an unpacked directory\n"
        "  -o, --out <dir>        destination directory (created if missing)\n"
        "  -f, --folder <path>    archive-relative folder or file to extract; repeatable\n"
        "\n"
        "Options:\n"
        "  -h, --help             show this help and exit\n"
        "\n"
        "Notes:\n"
        "  .sct and .sct2 textures are written as .png, and .db and .scsp files as .json.\n"
        "  Settings in czn_ripper.ini are ignored.\n"
        "\n"
        "Exit codes report whether each requested folder was found and extracted. Errors\n"
        "on individual files are logged and skipped without changing the exit code, so\n"
        "check czn_ripper.log in the working directory to confirm a clean run.\n"
        "  0  every requested folder was found and extracted\n"
        "  1  usage error\n"
        "  2  pack could not be opened or scanned\n"
        "  3  some requested folders were missing or failed\n"
        "  4  none of the requested folders could be extracted\n";
}

/**
 * Parses the command line into a CliOptions, printing its own diagnostics.
 * @param args UTF-8 arguments, excluding the program name.
 * @param out_options Filled in on success.
 * @returns CLI_EXIT_OK on success, CLI_PARSE_HELP when help was printed, CLI_EXIT_USAGE on a bad command line.
 */
static int parse_args(const std::vector<std::string>& args, CliOptions& out_options)
{
    std::set<std::string> seen_folders;

    for (size_t i = 0; i < args.size(); ++i)
    {
        std::string arg = args[i];

        std::string inline_value;
        bool has_inline = false;
        if (arg.rfind("--", 0) == 0)
        {
            const size_t equals = arg.find('=');
            if (equals != std::string::npos)
            {
                inline_value = arg.substr(equals + 1);
                arg = arg.substr(0, equals);
                has_inline = true;
            }
        }

        // Pulls a flag's value from either "--flag value" or "--flag=value".
        auto take_value = [&](const char* flag_name, std::string& dest) -> bool
        {
            if (has_inline)
            {
                dest = inline_value;
            }
            else if (i + 1 < args.size())
            {
                dest = args[++i];
            }
            else
            {
                std::cerr << "error: " << flag_name << " needs a value\n";
                return false;
            }

            if (dest.empty())
            {
                std::cerr << "error: " << flag_name << " needs a non-empty value\n";
                return false;
            }
            return true;
        };

        if (arg == "-h" || arg == "--help")
        {
            print_help();
            return CLI_PARSE_HELP;
        }
        else if (arg == "-p" || arg == "--pack")
        {
            if (!take_value("--pack", out_options.pack))
                return CLI_EXIT_USAGE;
        }
        else if (arg == "-o" || arg == "--out")
        {
            if (!take_value("--out", out_options.out))
                return CLI_EXIT_USAGE;
        }
        else if (arg == "-f" || arg == "--folder")
        {
            std::string raw;
            if (!take_value("--folder", raw))
                return CLI_EXIT_USAGE;

            // A folder that normalizes away means the root was requested. Root extraction behaves inconsistently across pack types, so reject it outright.
            const std::string normalized = normalize_archive_path(raw);
            if (normalized.empty())
            {
                std::cerr << "error: whole-archive extraction is not supported; name a folder (got \"" << raw << "\")\n";
                return CLI_EXIT_USAGE;
            }
            if (seen_folders.insert(normalized).second)
                out_options.folders.push_back(normalized);
        }
        else
        {
            std::cerr << "error: unrecognized argument \"" << args[i] << "\"\n";
            return CLI_EXIT_USAGE;
        }
    }

    if (out_options.pack.empty())
    {
        std::cerr << "error: --pack is required\n";
        return CLI_EXIT_USAGE;
    }
    if (out_options.out.empty())
    {
        std::cerr << "error: --out is required\n";
        return CLI_EXIT_USAGE;
    }
    if (out_options.folders.empty())
    {
        std::cerr << "error: at least one --folder is required\n";
        return CLI_EXIT_USAGE;
    }

    return CLI_EXIT_OK;
}

// //////////////////////////////////////////////////////////////////////////////////////////////////
// //////////////////////////////////////////////////////////////////////////////////////////////////
// Entry point

/**
 * Opens the pack, scans it, and extracts every requested folder.
 * @param argc Argument count.
 * @param argv Argument vector. On Windows the wide command line is read instead, so this is only a fallback.
 * @returns One of the CLI_EXIT_* codes.
 */
int main(int argc, char** argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    CliOptions options;
    const int parse_result = parse_args(collect_args_utf8(argc, argv), options);
    if (parse_result == CLI_PARSE_HELP)
        return CLI_EXIT_OK;
    if (parse_result != CLI_EXIT_OK)
    {
        std::cerr << "\nRun czn-cli --help for usage.\n";
        return parse_result;
    }

    const std::filesystem::path pack_path(Core::Utf8ToWString(options.pack));
    const std::filesystem::path dest_path(Core::Utf8ToWString(options.out));

    if (!std::filesystem::exists(pack_path))
    {
        std::cerr << "error: pack not found: " << options.pack << "\n";
        return CLI_EXIT_PACK_FAILED;
    }

    try
    {
        std::filesystem::create_directories(dest_path);
    }
    catch (const std::exception& e)
    {
        std::cerr << "error: could not create output directory " << options.out << ": " << e.what() << "\n";
        return CLI_EXIT_PACK_FAILED;
    }

    LogInfo("CLI run started for pack: " + options.pack);

    std::unique_ptr<IArchive> archive;
    try
    {
        archive = CreateArchive(pack_path.wstring());
    }
    catch (const std::exception& e)
    {
        std::cerr << "error: could not open pack: " << e.what() << "\n";
        LogError(std::string("CLI could not open pack: ") + e.what());
        return CLI_EXIT_PACK_FAILED;
    }
    if (!archive)
    {
        std::cerr << "error: could not open pack: " << options.pack << "\n";
        return CLI_EXIT_PACK_FAILED;
    }

    std::cout << "Pack:        " << options.pack << "\n";
    std::cout << "Destination: " << Core::PathToUtf8(std::filesystem::absolute(dest_path)) << "\n";
    std::cout << "Scanning pack (this can take a while for a full data.pack)...\n";

    std::atomic<float> progress = 0.f;
    try
    {
        archive->Scan(progress);
    }
    catch (const std::exception& e)
    {
        std::cerr << "error: scan failed: " << e.what() << "\n";
        LogError(std::string("CLI scan failed: ") + e.what());
        return CLI_EXIT_PACK_FAILED;
    }

    std::cout << "Scanned " << archive->GetParsedFileCount() << " files (" << Core::FormatSize(archive->GetParsedTotalSize()) << ")\n\n";

    // Resolve every requested folder up front so missing ones are reported before any writing starts.
    const Core::FileNode& root = archive->GetFileTree();
    std::vector<const Core::FileNode*> resolved;
    std::vector<std::string> missing;

    for (const std::string& folder : options.folders)
    {
        const Core::FileNode* node = Core::FindNodeByPath(root, folder);
        if (node == nullptr)
        {
            std::cerr << "error: not found in archive: " << folder << "\n";
            LogError("CLI folder not found in archive: " + folder);
            missing.push_back(folder);
            continue;
        }
        resolved.push_back(node);
    }

    if (resolved.empty())
    {
        std::cerr << "\nNothing to extract.\n";
        return CLI_EXIT_ALL_FAILED;
    }

    std::vector<std::string> failed;

    for (size_t i = 0; i < resolved.size(); ++i)
    {
        const Core::FileNode* node = resolved[i];
        std::cout << "[" << (i + 1) << "/" << resolved.size() << "] " << node->full_path << "\n";

        try
        {
            archive->Extract(*node, dest_path.wstring(), progress, true, true);
        }
        catch (const std::exception& e)
        {
            std::cerr << "  error: " << e.what() << "\n";
            LogError("CLI extraction failed for " + node->full_path + ": " + e.what());
            failed.push_back(node->full_path);
        }
    }

    const size_t failure_count = missing.size() + failed.size();
    const size_t success_count = options.folders.size() - failure_count;

    std::cout << "\nDone. " << success_count << " of " << options.folders.size() << " folders extracted.\n";
    if (!missing.empty())
    {
        std::cout << "Not found in archive:\n";
        for (const std::string& folder : missing)
            std::cout << "  " << folder << "\n";
    }
    if (!failed.empty())
    {
        std::cout << "Failed during extraction:\n";
        for (const std::string& folder : failed)
            std::cout << "  " << folder << "\n";
    }

    LogInfo("CLI run finished: " + std::to_string(success_count) + "/" + std::to_string(options.folders.size()) + " folders extracted");
    std::cout << "Per-file log: " << Core::PathToUtf8(std::filesystem::absolute("czn_ripper.log")) << "\n";

    if (failure_count == 0)
        return CLI_EXIT_OK;
    if (success_count == 0)
        return CLI_EXIT_ALL_FAILED;
    return CLI_EXIT_PARTIAL_FAILURE;
}
