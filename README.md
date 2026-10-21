<p align="center"><img src="assets/iris.png" width="96" alt="Iris Image"></p>

# 👁️ Iris Image — Instant Image Converter
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Build](https://github.com/VitomDev/Iris-Image/actions/workflows/build.yml/badge.svg)](https://github.com/VitomDev/Iris-Image/actions/workflows/build.yml)

Convert images straight from the Windows File Explorer context menu. 🖱️  
Right-click an image, choose **Iris Image — Convert to...** and a format, and the converted copy appears next to the original. That's it.

No windows, no settings, no background processes. Iris Image only runs while it is converting, then exits.

---

## 🚀 Features

- 🖼️ Converts to **PNG, JPG, WebP, BMP, GIF, TIFF and ICO**
- 🪟 Native context menu: the modern menu on Windows 11, the classic menu on Windows 10
- 📚 Converts a whole selection in one go, using up to four threads
- 🛡️ Never modifies or overwrites anything: if `photo.webp` exists, you get `photo (1).webp`
- 🧹 Never leaves half-written files behind, even if something fails
- 🫥 Keeps transparency where the format supports it; JPG is flattened onto white
- 📱 Applies EXIF orientation, so rotated phone photos stay upright
- 🎨 Keeps embedded RGB color profiles (PNG, JPG, TIFF, WebP)
- 🔌 Works offline. No telemetry, no updater, no services
- 🪶 Tiny: a ~0.9 MB executable and a ~160 KB shell extension, no .NET or Visual C++ runtime needed

---

## 🖼️ Supported formats

| Format | Read | Write | Notes |
|--------|:----:|:-----:|-------|
| PNG    | ✅ | ✅ | Keeps 16-bit and grayscale images as they are. |
| JPG    | ✅ | ✅ | Quality 90. Transparency is flattened onto white. |
| WebP   | ✅ | ✅ | Lossy, quality 90, with alpha. Max 16383 × 16383 pixels. |
| BMP    | ✅ | ✅ | 32-bit with alpha (BITMAPV5) when the image has transparency. |
| GIF    | ✅ | ✅ | 256-color palette with dithering and transparency. |
| TIFF   | ✅ | ✅ | Lossless ZIP compression. |
| ICO    | ✅ | ✅ | Multi-size icon: 16, 24, 32, 48, 64, 128 and 256 px (never upscaled). |

🎞️ **Animations and multi-page files:** Iris Image converts the first frame of animated GIF and WebP files and the first page of multi-page TIFF files. When reading an icon, it uses the largest image in it.

🔁 **Same format:** when every selected file already has the target format, that format is hidden from the submenu. In a mixed selection, files already in the target format are skipped instead of being recompressed.

---

## 📦 Install

1. Download `IrisImageSetup.exe` from the [latest release](https://github.com/VitomDev/Iris-Image/releases/latest).
2. Run it and follow the short wizard.

Requires Windows 10 version 1809 or later, or Windows 11 (x64).  
To uninstall: **Settings → Apps → Installed apps → Iris Image**.

### 🔐 Why does it ask for administrator rights?

The Windows 11 context menu only shows entries from apps with a *package identity*. Iris Image gets one from a small package that contains no code. Windows only accepts such a package without a paid code-signing certificate when an administrator installs it.

On Windows 10, or if the package cannot be registered, Iris Image uses the classic context menu instead (on Windows 11 it then appears under **Show more options**).

---

## 🖱️ Usage

1. Right-click one or more images in File Explorer.
2. Choose **Iris Image — Convert to...** and pick a format.

The new files appear in the same folder. ✨  
If something goes wrong (a damaged file, a read-only folder, a full disk), a single Windows notification explains it. Successful conversions show no message.

It can also be used from scripts:

```
Iris.exe convert <png|jpg|webp|bmp|gif|tiff|ico> <image>...
```

---

## ⚙️ How it works

| Part | Role |
|------|------|
| `IrisShell.dll` | The Explorer menu (`IExplorerCommand`). It never opens images: it only starts `Iris.exe` with the selected files, so Explorer stays fast and safe. |
| `Iris.exe` | Converts the images, shows a notification on errors and exits. |
| `Iris.msix` | Small package that adds the menu to the Windows 11 context menu. |

Images are handled by the Windows Imaging Component, which is part of Windows, except WebP, which uses [libwebp](https://github.com/webmproject/libwebp). Each image is written to a hidden temporary file in the destination folder and renamed only when complete; the rename never replaces an existing file.

---

## 🔧 Build

Requirements: Visual Studio 2022 or newer with the **Desktop development with C++** workload, and [Inno Setup 6](https://jrsoftware.org/isinfo.php) (`winget install JRSoftware.InnoSetup`).

```powershell
.\build.ps1
```

It downloads pinned, hash-checked copies of the Windows SDK and libwebp into `.deps/` if missing, builds everything, runs the tests and writes the installer to `dist/IrisImageSetup.exe`. Use `-SkipTests` or `-SkipInstaller` to shorten it.

🧪 To run the tests on their own:

```powershell
.\build\IrisTests.exe .\build\Iris.exe
```

📁 Project layout:

```
src/core/     Conversion engine and safe file output
src/app/      Iris.exe (worker and notifications)
src/shell/    IrisShell.dll (Explorer menu)
package/      Package manifest for the Windows 11 menu
installer/    Inno Setup script
tests/        Automated tests
```

---

## 🧪 Project Status

✅ Version 1.0 released.  
This project is fully functional and ready to use.
Bug reports and suggestions are welcome on the [issues](https://github.com/VitomDev/Iris-Image/issues) page.

---

## 📲 Contact

Feel free to reach out or follow me:

- 🐙 GitHub: [@VitomDev](https://github.com/VitomDev)
- 🐦 Twitter/X: [@VitomDev](https://twitter.com/VitomDev)
- 🎮 Twitch: [@VitomDev](https://www.twitch.tv/VitomDev)
- 💼 LinkedIn: [Víctor Escós](https://www.linkedin.com/in/víctor-escós-batalla/)

---

## 📄 License

Iris Image is released under the [MIT License](LICENSE). It includes libwebp, distributed under the BSD 3-Clause License; see [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

---

## 🧑‍💻 Author

**Víctor Escós Batalla**
