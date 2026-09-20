#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include "core/Core.h"
#include "core/FileTree.h"
#include "core/Logger.h"
#include "archive/IArchive.h"
#include "archive/ArchiveFactory.h"

// //////////////////////////////////////////////////////////////////////////////////////////////////
// //////////////////////////////////////////////////////////////////////////////////////////////////
// Constants and global state

// Exit codes. Scripts can tell "nothing worked" apart from "most of it worked".
static constexpr int CLI_EXIT_OK = 0;
static constexpr int CLI_EXIT_USAGE = 1;
static constexpr int CLI_EXIT_PACK_FAILED = 2;
static constexpr int CLI_EXIT_PARTIAL_FAILURE = 3;
static constexpr int CLI_EXIT_ALL_FAILED = 4;

// Returned by parse_args when help was printed, so main can exit successfully without doing any work.
static constexpr int CLI_PARSE_HELP = -1;

static constexpr int PROGRESS_BAR_WIDTH = 30;
static constexpr int PROGRESS_POLL_MS = 100;
// When stdout is redirected we cannot redraw in place, so only emit a line every this many percent.
static constexpr int PROGRESS_FILE_STEP_PERCENT = 5;

// Set once in main, then read by the progress functions to choose between an in-place bar and plain lines.
static bool g_stdout_is_tty = false;
// Set while parsing -q, then read by the progress functions to suppress their output entirely.
static bool g_quiet = false;

/** Parsed command line for a single CLI run. */
struct CliOptions
{
    /** UTF-8 path to data.pack, a manifest.ssra, or an unpacked directory. */
    std::string pack;
    /** UTF-8 path to the destination directory. Created if it does not exist. */
    std::string out;
    /** Normalized archive-relative folder (or file) paths to extract, in the order given, without duplicates. */
    std::vector<std::string> folders;
    /** Convert .sct / .sct2 textures to .png and rewrite .atlas texture references to match. */
    bool convert_sct_to_png = true;
    /** Convert encrypted .db files to .json. */
    bool convert_db_to_json = true;
    /** Print every extracted file instead of just a progress bar. */
    bool verbose = false;
};

/** One requested folder that was found in the archive, with its measured weight. */
struct ResolvedFolder
{
    /** The matching node in the archive's file tree. Its full_path is the path the user asked for. */
    const Core::FileNode* node;
    /** Total bytes of every file under the node, used to weight the overall percentage. */
    uint64_t bytes;
    /** Number of files under the node. */
    uint32_t files;
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

/**
 * Builds the output base to hand to `IArchive::Extract` so the archive path is preserved in full. Extract appends only the node's own name, so passing the
 * node's parent prefix makes the final path come out as <dest>/<full archive path>.
 * @param dest The user's --out directory.
 * @param node_full_path The node's archive-relative full_path.
 * @returns The directory to pass as Extract's output_path.
 */
static std::filesystem::path output_base_for(const std::filesystem::path& dest, const std::string& node_full_path)
{
    const size_t slash = node_full_path.find_last_of('/');
    if (slash == std::string::npos)
        return dest;
    return dest / std::filesystem::path(Core::Utf8ToWString(node_full_path.substr(0, slash)));
}

// //////////////////////////////////////////////////////////////////////////////////////////////////
// //////////////////////////////////////////////////////////////////////////////////////////////////
// Progress reporting

/**
 * Redraws the progress bar in place on a console, or emits sparse plain lines when stdout is redirected. Safe to use a carriage return here because the
 * extraction path logs to czn_ripper.log rather than stdout. The one exception is SCTParser's std::cerr writes on a hard astcenc failure, which will smear
 * the bar. That is acceptable since those signal a real decode problem worth seeing.
 * @param label Short prefix such as "Scanning".
 * @param fraction Progress in the range [0, 1].
 * @param suffix Trailing detail such as "total 42%", or empty for none.
 * @param last_percent The last percentage this operation printed, updated in place. Only used when stdout is redirected.
 */
static void draw_progress(const std::string& label, float fraction, const std::string& suffix, int& last_percent)
{
    if (g_quiet)
        return;

    fraction = std::min(1.0f, std::max(0.0f, fraction));
    const int percent = static_cast<int>(fraction * 100.0f + 0.5f);

    if (!g_stdout_is_tty)
    {
        if (percent == last_percent || (percent % PROGRESS_FILE_STEP_PERCENT) != 0)
            return;
        last_percent = percent;
        std::cout << label << " " << percent << "%";
        if (!suffix.empty())
            std::cout << " " << suffix;
        std::cout << "\n";
        return;
    }

    const int filled = static_cast<int>(fraction * PROGRESS_BAR_WIDTH + 0.5f);
    std::string bar(static_cast<size_t>(filled), '#');
    bar.append(static_cast<size_t>(PROGRESS_BAR_WIDTH - filled), '-');

    // The trailing spaces erase leftovers from a previously longer line.
    std::cout << '\r' << label << " [" << bar << "] " << std::setw(3) << percent << "%  " << suffix << "        " << std::flush;
}

/**
 * Runs an operation and turns any escaping exception into a reported failure, so one bad folder does not abandon the rest of the run.
 * @param work The operation to run.
 * @returns True when the operation completed without throwing.
 */
static bool run_guarded(const std::function<void()>& work)
{
    try
    {
        work();
    }
    catch (const std::exception& e)
    {
        std::cerr << "  error: " << e.what() << "\n";
        LogError(std::string("CLI operation failed: ") + e.what());
        return false;
    }
    catch (...)
    {
        std::cerr << "  error: unknown exception\n";
        LogError("CLI operation failed with an unknown exception");
        return false;
    }
    return true;
}

/**
 * Runs a blocking archive operation on a worker thread while the calling thread polls its progress atomic and redraws the bar. The archive API has no
 * progress callback, so polling is the only way to show movement. Only one worker runs at a time, so the pack's sliding memory map is never shared.
 * @param label Progress bar label.
 * @param progress The atomic the operation writes into.
 * @param suffix_fn Called on each tick to build the bar's trailing text.
 * @param work The blocking operation.
 * @returns True when the operation completed without throwing.
 */
static bool run_with_progress(const std::string& label, std::atomic<float>& progress, const std::function<std::string()>& suffix_fn, const std::function<void()>& work)
{
    progress = 0.f;
    int last_percent = -1;
    std::future<void> future = std::async(std::launch::async, work);

    while (future.wait_for(std::chrono::milliseconds(PROGRESS_POLL_MS)) != std::future_status::ready)
        draw_progress(label, progress.load(), suffix_fn(), last_percent);

    draw_progress(label, 1.0f, suffix_fn(), last_percent);
    if (!g_quiet && g_stdout_is_tty)
        std::cout << "\n";

    return run_guarded([&future] { future.get(); });
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
        "  czn-cli --pack <path> --out <dir> --folder <archive/path> [--folder ...] [options]\n"
        "\n"
        "Required:\n"
        "  -p, --pack <path>      data.pack, manifest.ssra, or an unpacked directory\n"
        "  -o, --out <dir>        destination directory (created if missing)\n"
        "  -f, --folder <path>    archive-relative folder or file to extract; repeatable\n"
        "\n"
        "Options:\n"
        "      --no-png           keep .sct / .sct2 as-is instead of converting to .png\n"
        "      --no-json          keep .db as-is instead of converting to .json\n"
        "  -v, --verbose          print each extracted file\n"
        "  -q, --quiet            suppress the progress bar; print only the summary\n"
        "  -h, --help             show this help and exit\n"
        "\n"
        "Notes:\n"
        "  Output preserves the full archive path. --out D:\\out --folder gameres/spine\n"
        "  writes D:\\out\\gameres\\spine\\...\n"
        "  .scsp files are always converted to .json.\n"
        "  Settings in czn_ripper.ini are ignored. Use the flags above.\n"
        "  Prefer a short destination path. Deep archive paths can exceed the Windows\n"
        "  MAX_PATH limit when the destination is already long.\n"
        "\n"
        "Exit codes report whether each requested folder was FOUND and extracted. Errors\n"
        "on individual files are logged and skipped without changing the exit code, so\n"
        "check czn_ripper.log in the working directory to confirm a clean run.\n"
        "  0  every requested folder was found and extracted\n"
        "  1  usage error\n"
        "  2  pack could not be opened or scanned\n"
        "  3  some requested folders were missing or failed\n"
        "  4  none of the requested folders could be extracted\n"
        "\n"
        "Example:\n"
        "  czn-cli --pack \"C:\\Games\\ChaosZeroNightmare\\bin\\appdata\\cznlive\\data.pack\"\n"
        "          --out D:\\czn_assets\n"
        "          --folder card --folder story --folder cutin --folder collapse\n";
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

        // Rejects "--no-png=false" and friends, which would otherwise be read as the flag with the value silently discarded.
        auto reject_value = [&]() -> bool
        {
            if (!has_inline)
                return true;
            std::cerr << "error: " << arg << " does not take a value\n";
            return false;
        };

        if (arg == "-h" || arg == "--help")
        {
            if (!reject_value())
                return CLI_EXIT_USAGE;
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
        else if (arg == "--no-png")
        {
            if (!reject_value())
                return CLI_EXIT_USAGE;
            out_options.convert_sct_to_png = false;
        }
        else if (arg == "--no-json")
        {
            if (!reject_value())
                return CLI_EXIT_USAGE;
            out_options.convert_db_to_json = false;
        }
        else if (arg == "-v" || arg == "--verbose")
        {
            if (!reject_value())
                return CLI_EXIT_USAGE;
            out_options.verbose = true;
        }
        else if (arg == "-q" || arg == "--quiet")
        {
            if (!reject_value())
                return CLI_EXIT_USAGE;
            g_quiet = true;
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
// Extraction

/**
 * Extracts one file at a time so each name can be printed. `IArchive::Extract` accepts any node including a leaf file, so this needs no change to the archive
 * code. Only used for --verbose since the per-file calls add a couple of log lines each.
 * @param archive The scanned archive.
 * @param node The node to walk.
 * @param dest The user's --out directory.
 * @param options Conversion flags.
 * @param files_done Running count of files written, updated in place.
 * @param files_total Total files expected, used for the printed counter.
 */
static void extract_verbose(IArchive& archive, const Core::FileNode& node, const std::filesystem::path& dest, const CliOptions& options, uint32_t& files_done, uint32_t files_total)
{
    if (std::holds_alternative<Core::FileInfo>(node.data))
    {
        const auto& info = std::get<Core::FileInfo>(node.data);
        const std::filesystem::path out_base = output_base_for(dest, node.full_path);

        std::atomic<float> local_progress = 0.f;
        archive.Extract(node, out_base.wstring(), local_progress, options.convert_sct_to_png, options.convert_db_to_json);

        ++files_done;
        std::cout << "  [" << files_done << "/" << files_total << "] " << node.full_path << "  (" << Core::FormatSize(info.size) << ")\n";
        return;
    }

    for (const auto& child : std::get<Core::FolderInfo>(node.data).children)
        extract_verbose(archive, child, dest, options, files_done, files_total);
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
    g_stdout_is_tty = _isatty(_fileno(stdout)) != 0;
#else
    g_stdout_is_tty = isatty(1) != 0;
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
    if (!run_guarded([&] { archive = CreateArchive(pack_path.wstring()); }) || !archive)
    {
        std::cerr << "error: could not open pack: " << options.pack << "\n";
        return CLI_EXIT_PACK_FAILED;
    }

    std::cout << "Pack:        " << options.pack << "\n";
    std::cout << "Destination: " << Core::PathToUtf8(std::filesystem::absolute(dest_path)) << "\n";
    std::cout << "Scanning pack (this can take a while for a full data.pack)...\n";

    std::atomic<float> progress = 0.f;
    if (!run_with_progress("Scanning", progress, [] { return std::string(); }, [&] { archive->Scan(progress); }))
    {
        std::cerr << "error: scan failed\n";
        return CLI_EXIT_PACK_FAILED;
    }

    std::cout << "Scanned " << archive->GetParsedFileCount() << " files (" << Core::FormatSize(archive->GetParsedTotalSize()) << ")\n\n";

    // Resolve every requested folder up front so missing ones are reported before any writing starts.
    const Core::FileNode& root = archive->GetFileTree();
    std::vector<ResolvedFolder> resolved;
    std::vector<std::string> missing;
    uint64_t bytes_total = 0;

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

        uint64_t bytes = 0;
        uint32_t files = 0;
        Core::NodeStats(*node, bytes, files);
        bytes_total += bytes;
        resolved.push_back({node, bytes, files});
    }

    if (resolved.empty())
    {
        std::cerr << "\nNothing to extract.\n";
        return CLI_EXIT_ALL_FAILED;
    }

    std::vector<std::string> failed;
    uint64_t bytes_done = 0;

    for (size_t i = 0; i < resolved.size(); ++i)
    {
        const ResolvedFolder& folder = resolved[i];
        const std::string& folder_path = folder.node->full_path;

        std::cout << "[" << (i + 1) << "/" << resolved.size() << "] " << folder_path << "  (" << folder.files << " files, " << Core::FormatSize(folder.bytes) << ")\n";

        // Extract returns straight away when the subtree holds no bytes, so say so here rather than flashing a bar to 100%.
        if (folder.bytes == 0)
        {
            std::cout << "  warning: no data to extract\n";
            LogInfo("CLI folder holds no data: " + folder_path);
            continue;
        }

        bool ok = true;
        if (options.verbose)
        {
            uint32_t files_done = 0;
            ok = run_guarded([&] { extract_verbose(*archive, *folder.node, dest_path, options, files_done, folder.files); });
        }
        else
        {
            // The bar tracks the current folder while the suffix shows progress across all of them.
            const std::filesystem::path out_base = output_base_for(dest_path, folder_path);
            auto suffix_fn = [&]
            {
                const float local = std::min(1.0f, std::max(0.0f, progress.load()));
                const uint64_t overall_done = bytes_done + static_cast<uint64_t>(local * static_cast<float>(folder.bytes));
                return "total " + std::to_string(static_cast<int>(100.0 * static_cast<double>(overall_done) / static_cast<double>(bytes_total))) + "%";
            };
            auto work = [&] { archive->Extract(*folder.node, out_base.wstring(), progress, options.convert_sct_to_png, options.convert_db_to_json); };
            ok = run_with_progress("  Extracting", progress, suffix_fn, work);
        }

        bytes_done += folder.bytes;
        if (!ok)
            failed.push_back(folder_path);
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
