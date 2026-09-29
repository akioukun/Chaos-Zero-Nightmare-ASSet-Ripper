# Chaos Zero Nightmare ASSet Ripper
ASSet Ripper is a tool for extracting encrypted pack assets from yuna engine, or certain anime games.

![Preview](./assets/img.png)


## [→ Download ←](https://nightly.link/akioukun/Chaos-Zero-Nightmare-ASSet-Ripper/workflows/build/main/ChaosZeroNightmareRipper.zip) 
> [!IMPORTANT]
> Releases tab has outdated builds and won't be updated anymore. Download the tool using the link above or via Github [Actions](https://github.com/akioukun/Chaos-Zero-Nightmare-ASSet-Ripper/actions)

## Features
Preview supported formats such as:
- SCT images (exported as PNG files)
- Encrypted databases (exported as JSON files)
- SCSP [spine](https://esotericsoftware.com/spine-in-depth) format (exported as JSON), compatible with tools such as [SpineViewer](https://github.com/ww-rm/SpineViewer). The tool also includes an integrated Spine Viewer

And export either all files or only selected files and folders, depending on your preference.

## How to use
### DataPack archives
1) Click `Open Pack` and select `data.pack` located under: `WhereYouInstalledTheGame\ChaosZeroNightmare\bin\appdata\cznlive` or, for the Chinese version, `WhereYouInstalledTheGame\bin\appdata\prod\data.pack`.
2) Click `Scan Tree` to scan the pack and build the game resource tree.

### SSRA archives
1) Click `Open Pack` and select a `manifest.ssra` directly, usually inside the game's `gameres` directory.
2) Keep the corresponding `.ssrc` files in the sibling `chunks` directory, for example `gameres\chunks`.
3) Click `Scan Tree` to scan the manifest and its available chunks.
   
## Navigating the File Tree and Exporting
You can navigate the file tree using either mouse or keyboard input.

#### Mouse Controls
- Scroll to move through the file tree
- Click to select items

#### Keyboard Controls
- **Up / Down Arrow** — move selection
- **Left / Right Arrow** — collapse or expand folders

#### Multi-Selection

Multiple files and folders can be selected for batch export:

- **Ctrl + Right Click**
- **Ctrl + Up / Down Arrow**


## CLI

`ChaosZeroNightmareRipper-CLI.exe` ships alongside the GUI and extracts named folders without opening a window, which makes it usable from scripts. It is a standalone binary and needs none of the DLLs the GUI uses.

```bash
ChaosZeroNightmareRipper-CLI --pack "WhereYouInstalledTheGame\ChaosZeroNightmare\bin\appdata\cznlive\data.pack" --out D:\czn_assets --folder rarity --folder tp_skill --folder select_scene --folder collapse/collapse_illustration
```

| Flag | Meaning |
| --- | --- |
| `-p`, `--pack <path>` | `data.pack`, a `manifest.ssra`, or an unpacked directory |
| `-o`, `--out <dir>` | destination directory, created if missing |
| `-f`, `--folder <path>` | archive-relative folder or file to extract; repeat for more than one |
| `--no-png` | keep `.sct` / `.sct2` as-is instead of converting to `.png` |
| `--no-json` | keep `.db` as-is instead of converting to `.json` |
| `-v`, `--verbose` | print each extracted file |
| `-q`, `--quiet` | suppress the progress bar |
| `-h`, `--help` | show help |

The output preserves the full archive path, so `--out D:\out --folder collapse/collapse_illustration` writes to `D:\out\collapse\collapse_illustration\`. Texture and database conversion are on by default, matching the GUI. A folder that is not in the archive is reported but does not stop the others, and the exit code says what happened: `0` every folder found and extracted, `1` usage error, `2` the pack could not be opened or scanned, `3` some folders were missing or failed, `4` none could be extracted. Errors on individual files are logged and skipped without changing the exit code, so check `czn_ripper.log` to confirm a clean run.

## Build Instructions

```bash
git clone https://github.com/akioukun/Chaos-Zero-Nightmare-ASSet-Ripper.git
cd Chaos-Zero-Nightmare-ASSet-Ripper
mkdir build && cd build
cmake ..
cmake --build . --config Release
```
