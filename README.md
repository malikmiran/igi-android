# IGI Port (Android)

Experimental native Android engine for Project I.G.I. (2000). You must own the game; point the app at your install folder.

## Build (GitHub Actions)
1. Create a new GitHub repo and push this folder to it.
2. Open the **Actions** tab, run **Build APK** (it also runs on every push).
3. Download the `igi-port-debug-apk` artifact, unzip, and install the APK (allow "install unknown apps").

The workflow first runs the parser unit tests on the runner, then builds the APK.

## Run
1. Copy your IGI install folder to the phone (or use the existing one).
2. Open the app, tap **Select IGI folder**, choose the folder that contains the `.res` files.
3. The report panel lists each archive. If an archive says `PARSE FAILED`, **send me the report text**; that is exactly
   the data needed to correct the format layouts (Stage 1 in ROADMAP.md).
4. Drag to orbit, pinch to zoom. Until a mesh loads you will see a test triangle.

## Layout
- `app/src/main/cpp/igi_formats.h` - ILFF/RES/TEX/MEF parsers (header-only, bounds-checked)
- `app/src/main/cpp/native_lib.cpp` - JNI + GLES3 viewer
- `app/src/main/java/com/igi/port/` - folder import + GL view
- `tests/test_formats.cpp` - host tests (`g++ -std=c++17 tests/test_formats.cpp && ./a.out`)
