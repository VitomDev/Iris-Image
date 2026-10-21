# 👁️ Iris Image — Instant Image Converter
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Build](https://github.com/VitomDev/Iris-Image/actions/workflows/build.yml/badge.svg)](https://github.com/VitomDev/Iris-Image/actions/workflows/build.yml)

A tiny Windows utility to convert images straight from the File Explorer context menu, built with C++ and the Windows Imaging Component.  
Right-click an image, choose **Iris Image — Convert to...** and a format, and the converted copy appears next to the original.

---

## 🚀 Features

- Converts to PNG, JPG, WebP, BMP, GIF, TIFF or ICO from the right-click menu
- Native integration: modern context menu on Windows 11, classic menu on Windows 10
- Converts a whole selection in one go, using up to four threads
- Never modifies or overwrites anything: if `photo.webp` exists, you get `photo (1).webp`
- No partial files: images are written to a hidden temporary file and only renamed when complete
- Keeps transparency where the format supports it; JPG is flattened onto white
- Applies EXIF orientation, so rotated phone photos stay upright
- Keeps embedded RGB color profiles (PNG, JPG, TIFF, WebP)
- ICO output with every common icon size (16 to 256 px, never upscaled)
- Hides the format a selection already has; files already in the target format are skipped, not recompressed
- No windows, no settings, no background processes, no telemetry, works offline
- Tiny: ~0.9 MB executable, no .NET or Visual C++ runtime needed
- Animated GIF/WebP and multi-page TIFF: the first frame is converted

---

## 📦 Install

Run `IrisImageSetup.exe` from the [latest release](https://github.com/VitomDev/Iris-Image/releases/latest). Requires Windows 10 1809 or later, or Windows 11 (x64).

The installer asks for administrator rights: Windows 11 only shows unsigned apps in its modern context menu when they are installed by an administrator. If that registration is not possible, Iris Image falls back to the classic menu (**Show more options** on Windows 11).

It can also be used from scripts:

```
Iris.exe convert <png|jpg|webp|bmp|gif|tiff|ico> <image>...
```

---

## 🔧 Build

Requirements: Visual Studio 2022 or newer with the C++ workload, and Inno Setup 6 (`winget install JRSoftware.InnoSetup`).

```powershell
.\build.ps1
```

It downloads pinned, hash-checked copies of the Windows SDK and [libwebp](https://github.com/webmproject/libwebp) into `.deps/` if missing, builds the app, runs the tests and writes the installer to `dist/`.

---

## 🧪 Project Status

✅ Version 1.0 released.  
This project is fully functional and ready to use.
Bug reports and suggestions are welcome through the [issues](https://github.com/VitomDev/Iris-Image/issues) page.

---

## 📲 Contact

Feel free to reach out or follow me:

- 🐙 GitHub: [@VitomDev](https://github.com/VitomDev)
- 🐦 Twitter/X: [@VitomDev](https://twitter.com/VitomDev)
- 🎮 Twitch: [@VitomDev](https://www.twitch.tv/VitomDev)
- 💼 LinkedIn: [Víctor Escós](https://www.linkedin.com/in/víctor-escós-batalla/)

---

## 🧑‍💻 Author

**Víctor Escós Batalla**

Iris Image is released under the [MIT License](LICENSE). It includes libwebp (BSD 3-Clause License); see [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
