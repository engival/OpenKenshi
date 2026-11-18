# OpenKenshi

OpenKenshi: play Kenshi on a free, open-source engine.

**OpenKenshi is not affiliated with or endorsed by Lo-Fi Games. "Kenshi" is a trademark of Lo-Fi Games Ltd. This
project will be renamed at Lo-Fi Games' request.**

A from-scratch engine for running Kenshi: a simulation server and a client that draws the world from your
purchased copy of Kenshi. It is an early preview: rough, incomplete, and changing quickly.

This project is an independent, fan-made engine for running Kenshi. It is not supported by Lo-Fi Games. It includes
no game assets or data: it requires a purchased copy of Kenshi (Steam or GOG), and the engine reads your installation.

This is our own implementation. No Lo-Fi Games code was copied. Community reverse-engineering references (see
[CREDITS.md](CREDITS.md)) were read as documentation only.

## Project goals

- **Network-based server/client first.** The simulation is a server and the client is a view of it, even in
  single-player.
- **A mod interface, not direct mod compatibility.** Mods made for the original are not a target; instead the engine
  aims for an extendable, Lua-based mod interface.
- **The game's data and intended behaviour, not its bugs.** The engine plays your installed game data as the original
  means it to, without reproducing the original's bugs or limits.

## License

[MIT](LICENSE). Kenshi's game data and assets are not covered: they belong to Lo-Fi Games, and you need a purchased copy of Kenshi.

## Build (Linux)

Linux is the platform it is developed on. macOS and Windows builds are untested.

Needs: CMake 3.20+, Ninja, a C++20 compiler, pkg-config, Python 3, and the development packages for X11 (x11, xi,
xcursor, xrandr), OpenGL, libvorbis + libogg, PulseAudio (libpulse) and recastnavigation.

```sh
client/tools/get_shdc.sh        # fetches the sokol-shdc shader compiler into client/bin/
cmake -S client -B client/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build client/build --target OpenKenshi
```

### macOS (CI-built, untested at runtime)

GitHub Actions builds it on `macos-latest` (Apple Silicon, `.github/workflows/macos.yml`); nobody has run the result yet. Graphics is Metal
(`KENSHI_GFX=metal`, the default on macOS), audio is CoreAudio, the window is Cocoa through sokol_app.

```sh
brew install cmake ninja pkg-config libvorbis libogg recastnavigation   # recastnavigation is fetched (v1.6.0) when brew lacks it
client/tools/get_shdc.sh
cmake -S client -B client/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build client/build --target OpenKenshi
```

Copy a Windows Kenshi install over and point the game at it, or use `~/Library/Application Support/Steam/steamapps/common/Kenshi`
(searched by default).

## Run

```sh
client/build/OpenKenshi
```

With no options it opens the main menu and runs the game server in the same process. It looks for your Kenshi
install in the usual Steam and GOG locations. If it cannot find one, it says where it looked; point it at your
install with `--data /path/to/Kenshi`, the `KENSHI_DATA` environment variable, or the config file it names. Kenshi
has no macOS version: on a Mac, copy a Windows install's `Kenshi` folder over and point the engine at it.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).
