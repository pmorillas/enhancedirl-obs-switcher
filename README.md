# EnhancedIRL Switcher for OBS Studio

Hand your live stream from one OBS to another without ending it.

EnhancedIRL Switcher is an OBS Studio plugin. Install it on each OBS you stream from, link them to
your EnhancedIRL account, and move the stream between them from **My OBS** in the EnhancedIRL
dashboard: the stream keeps going on your channel instead of ending and starting a new one.

It works between your home PC and an EnhancedIRL Cloud OBS, or from one PC to another.

**Supported platforms:** Twitch, Kick and YouTube.

Full guide: <https://docs.enhancedirl.com/go/en/my-obs>

## Requirements

- OBS Studio 31 or later.
- Windows 10/11 64-bit, macOS 12 or later (Apple Silicon and Intel), or Linux x86_64 (tested on
  Ubuntu 24.04; OBS installed with Flatpak is not supported).
- A free EnhancedIRL account with a verified email.

## Install

Download the plugin from <https://enhancedirl.com/obs-plugin> and close OBS before installing.

| System | File |
|---|---|
| Windows | `enhancedirl-obs-switcher-windows-x64.zip` |
| macOS | `enhancedirl-obs-switcher-macos-universal.tar.xz` |
| Linux | `enhancedirl-obs-switcher-x86_64-linux-gnu.tar.xz` |

**Windows.** Unzip the `.zip` into `C:\ProgramData\obs-studio\plugins\`. You should end up with an
`enhancedirl-obs-switcher` folder inside `plugins`.

**macOS.** Double-click the `.tar.xz` to open it and move `enhancedirl-obs-switcher.plugin` to
`~/Library/Application Support/obs-studio/plugins/`. The plugin is not signed yet, so run this once
in Terminal:

```sh
xattr -dr com.apple.quarantine ~/Library/Application\ Support/obs-studio/plugins/enhancedirl-obs-switcher.plugin
```

**Linux.** From the folder you downloaded it to:

```sh
sudo tar -xJf enhancedirl-obs-switcher-x86_64-linux-gnu.tar.xz -C /usr
```

Step-by-step guide with screenshots and troubleshooting:
<https://docs.enhancedirl.com/go/en/obs-plugin>

## First use

1. Open OBS and go to **Tools → EnhancedIRL Switcher...**.
2. Click **Link this OBS**. OBS shows a code and opens the EnhancedIRL dashboard in your browser.
3. Check that the dashboard shows the same code and confirm. The code expires after 10 minutes.
4. Back in OBS, with the stream stopped, click **Use EnhancedIRL Switcher**. This switches your
   stream service to EnhancedIRL Switcher and keeps your platform, server and stream key.
5. Repeat on your other OBS. From then on, **My OBS** lets you move the stream between them.

To remove an OBS, use **Unlink** in the plugin window or in **My OBS**.

## Privacy and security

The plugin never sends your stream key, only a one-way fingerprint of it, and the service has no
way to change where you stream to: it can only ask the plugin to start, stop or hand over the
stream you already set up. Everything the plugin exchanges with EnhancedIRL is documented in
[docs/PROTOCOL.md](docs/PROTOCOL.md).

## Building from source

You need CMake 3.28 or later and the toolchain for your system (Xcode on macOS, Visual Studio 2022
on Windows, GCC/Ninja on Linux). On macOS and Windows the build downloads the OBS sources and
dependencies pinned in `buildspec.json`; on Linux it uses the system OBS development packages,
Qt 6, mbedTLS and OpenSSL.

```sh
# macOS
cmake --preset macos
cmake --build --preset macos

# Windows
cmake --preset windows-x64
cmake --build --preset windows-x64

# Linux
cmake --preset ubuntu-x86_64
cmake --build --preset ubuntu-x86_64
```

`-DENABLE_TEST_HOOKS=ON` builds extra hooks for automated tests. Never distribute such a build.

## Support

Write to <support@enhancedirl.com> or open a ticket from **Support** in the EnhancedIRL dashboard.

## License

EnhancedIRL Switcher is free software, licensed under the GNU General Public License, version 2 or
(at your option) any later version. See [LICENSE](LICENSE).

It includes third-party code under its own licenses:

| Component | Where | License |
|---|---|---|
| RTMP output, derived from OBS Studio's `obs-outputs` | `src/rtmp-output/` | GPL-2.0-or-later (file headers) |
| librtmp | `src/rtmp-output/librtmp/` | LGPL-2.1-or-later ([COPYING](src/rtmp-output/librtmp/COPYING)) |
| IXWebSocket v11.4.6 | `deps/ixwebsocket/` | BSD-3-Clause ([LICENSE.txt](deps/ixwebsocket/LICENSE.txt)) |
| happy-eyeballs (Twitch Interactive) | `src/shared/happy-eyeballs/` | MIT (file headers) |
| obs-websocket API header (test builds only) | `src/obs-websocket-api.h` | GPL-2.0-or-later (file header) |
