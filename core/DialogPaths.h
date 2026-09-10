#pragma once
#include "Core.h"
#include "RipperOptions.h"
#include "portable-file-dialogs.h"
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

// Keeps each kind of file dialog opening where the user last left it. The folders are stored in the same ini as the rest of the options so they survive
// a restart. Call the three wrappers at the bottom rather than pfd directly, so seeding a dialog and recording its result stay one step.
namespace DialogPaths
{
    /** Which remembered location a file dialog should start from. */
    enum class Slot
    {
        /** Picking an archive, a manifest, or a folder to browse. */
        Open,
        /** Picking a destination folder for an extraction. */
        Extract,
        /** Saving or re-opening an individual file, such as an exported file map. */
        Export,
    };

    namespace Internal
    {
        /**
         * Looks up the stored directory for a slot so callers can read it or write it back.
         *
         * @param options The options to read from or update.
         * @param slot The dialog kind whose remembered directory is wanted.
         * @returns Reference to the matching field.
         */
        inline std::string &slotDir(RipperOptions &options, Slot slot)
        {
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
         * Reads the remembered directory for a slot, treating one that no longer exists as nothing remembered. That covers a removed drive or an output
         * folder deleted between runs.
         *
         * @param slot The dialog kind about to be shown.
         * @returns The directory, or an empty string when there is nothing usable to start from.
         */
        inline std::string rememberedDir(Slot slot)
        {
            RipperOptions options = LoadRipperOptions();
            const std::string stored = slotDir(options, slot);
            if (stored.empty())
            {
                return "";
            }

            std::error_code ec;
            if (!std::filesystem::is_directory(std::filesystem::path(Core::Utf8ToWString(stored)), ec))
            {
                return "";
            }
            return stored;
        }

        /** How to seed a folder dialog. The two fields have to agree, so they are decided together in `folderSeed`. */
        struct FolderSeed
        {
            /** Path to hand portable-file-dialogs. "." means no preference, see `folderSeed` for why it is not an empty string. */
            std::string path;
            /** Whether that path overrides the folder Windows would otherwise pick. */
            pfd::opt options;
        };

        /**
         * Decides how to seed a folder dialog. This is folder-dialog-only. A file dialog reads the same string through `GetFileAttributesW`, where "."
         * is a real directory and would pin the dialog to the working directory, so open and save dialogs pass the remembered path straight through.
         *
         * With a folder remembered, `force_path` is required: without it portable-file-dialogs calls `IFileDialog::SetDefaultFolder`, which Windows
         * ignores whenever the shell has a most-recently-visited folder for this app, so opening a pack would strand every later folder dialog there.
         *
         * With nothing remembered, "." is deliberate and must not become an empty string. `SHCreateItemFromParsingName` rejects "." so no folder is set
         * at all and the shell picks, whereas "" resolves to the desktop and would pin every folder dialog there.
         *
         * @param remembered The remembered directory, or an empty string when there is none.
         * @returns The path and options to hand the dialog.
         */
        inline FolderSeed folderSeed(const std::string &remembered)
        {
            if (remembered.empty())
            {
                return {".", pfd::opt::none};
            }
            return {remembered, pfd::opt::force_path};
        }

        /**
         * Builds the default path for a save dialog by putting the suggested name inside the remembered directory. The join goes through
         * `std::filesystem` so the separator is native, because the Windows shell fails to parse a folder written with forward slashes.
         *
         * @param remembered The remembered directory, or an empty string when there is none.
         * @param defaultName The suggested file name, such as "filemap.json".
         * @returns A full path when a directory is remembered, otherwise just the name.
         */
        inline std::string startPath(const std::string &remembered, const std::string &defaultName)
        {
            if (remembered.empty())
            {
                return defaultName;
            }
            return Core::PathToUtf8(std::filesystem::path(Core::Utf8ToWString(remembered)) / Core::Utf8ToWString(defaultName));
        }

        /**
         * Stores a directory for a slot and writes it to the ini. The ini is read back first so this keeps whatever another part of the app has saved
         * since the process started.
         *
         * @param slot The dialog kind that was just used.
         * @param dir The directory to remember. An empty string is ignored, which is what a cancelled dialog gives.
         */
        inline void rememberDir(Slot slot, const std::string &dir)
        {
            if (dir.empty())
            {
                return;
            }

            RipperOptions options = LoadRipperOptions();
            std::string &stored = slotDir(options, slot);
            if (stored == dir)
            {
                return;
            }

            stored = dir;
            SaveRipperOptions(options);
        }

        /**
         * Stores the folder that a chosen file sits in.
         *
         * @param slot The dialog kind that was just used.
         * @param path The file the user picked or saved. An empty string is ignored.
         */
        inline void rememberParentOf(Slot slot, const std::string &path)
        {
            if (path.empty())
            {
                return;
            }

            const std::filesystem::path parent = std::filesystem::path(Core::Utf8ToWString(path)).parent_path();
            if (!parent.empty())
            {
                rememberDir(slot, Core::PathToUtf8(parent));
            }
        }
    }

    /**
     * Shows a save dialog starting in the folder the last single file went into, and remembers where this one goes.
     *
     * @param title Dialog title.
     * @param defaultName Suggested file name.
     * @param filters Filter pairs in the portable-file-dialogs form.
     * @returns The chosen path, or an empty string when the dialog was cancelled.
     */
    inline std::string SaveFile(const std::string &title, const std::string &defaultName, const std::vector<std::string> &filters)
    {
        const std::string remembered = Internal::rememberedDir(Slot::Export);
        pfd::save_file dialog(title, Internal::startPath(remembered, defaultName), filters);
        const std::string picked = dialog.result();
        Internal::rememberParentOf(Slot::Export, picked);
        return picked;
    }

    /**
     * Shows an open dialog starting in the remembered folder for a slot, and remembers the folder the chosen file came from.
     *
     * @param slot Which remembered location to start from.
     * @param title Dialog title.
     * @param filters Filter pairs in the portable-file-dialogs form.
     * @returns The chosen path, or an empty string when the dialog was cancelled.
     */
    inline std::string OpenFile(Slot slot, const std::string &title, const std::vector<std::string> &filters)
    {
        // Passed straight through: portable-file-dialogs skips the initial directory when this is empty, which lets Windows pick.
        const std::string remembered = Internal::rememberedDir(slot);
        pfd::open_file dialog(title, remembered, filters);
        const std::vector<std::string> picked = dialog.result();
        if (picked.empty())
        {
            return "";
        }

        Internal::rememberParentOf(slot, picked[0]);
        return picked[0];
    }

    /**
     * Shows a folder picker starting in the remembered folder for a slot, and remembers the folder that was chosen.
     *
     * @param slot Which remembered location to start from.
     * @param title Dialog title.
     * @returns The chosen folder, or an empty string when the dialog was cancelled.
     */
    inline std::string SelectFolder(Slot slot, const std::string &title)
    {
        const Internal::FolderSeed seed = Internal::folderSeed(Internal::rememberedDir(slot));
        pfd::select_folder dialog(title, seed.path, seed.options);
        const std::string picked = dialog.result();
        Internal::rememberDir(slot, picked);
        return picked;
    }
}
