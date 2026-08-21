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
1) Click `Open Pack` and select `data.pack` located under: `WhereYouInstalledTheGame\ChaosZeroNightmare\bin\appdata\cznlive` or for the chinese version `WhereYouInstalledTheGame\bin\appdata\prod\data.pack`
2) Click `Scan Tree` which will scan the files and build game resources file tree
### Important Note for the Chinese Version
The tool also suport the chinese version of the game (卡厄思梦境). But for having the tool properly scanning each `manifest.ssra`, you need to have AT LEAST launched the game once, so that the client have generated a `data.pack` which you will select, as explained in [How to use](#how-to-use)
   
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


## Build Instructions

```bash
git clone https://github.com/akioukun/Chaos-Zero-Nightmare-ASSet-Ripper.git
cd Chaos-Zero-Nightmare-ASSet-Ripper
mkdir build && cd build
cmake ..
cmake --build . --config Release
```
