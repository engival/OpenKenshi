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
