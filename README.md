# NTop

[![Latest Release](https://img.shields.io/github/release/Nuke928/NTop.svg)](https://github.com/Nuke928/NTop/releases/latest)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)

htop-like system-monitor with Vi-emulation for Windows. Because using Task Manager is not cool enough.

![NTop](https://user-images.githubusercontent.com/4589491/56905702-3c3d5c80-6a90-11e9-991c-b7a398742614.PNG)

NTop as in Windows NT-op or NukeTop. Whatever you prefer (the latter obviously).

## Installation

### Chocolatey

```sh
$ choco install ntop.portable
```

### Scoop

```sh
$ scoop install ntop
```

## Usage

### Options

| Option | Meaning |
|:---|:---|
| `-C` | Use monochrome color scheme. |
| `-h` | Display help info. |
| `-p` PID, PID... | Show only the given PIDs. |
| `-s` COLUMN | Sort by this column. |
| `-u` USERNAME | Only display processes belonging to this user. |
| `-v` | Print version. |

### Interactive commands

| Key(s) | Purpose |
|:---|:---|
| Up and Down Arrows, <kbd>PgUp</kbd> and <kbd>PgDown</kbd>, <kbd>j</kbd> and <kbd>k</kbd> | Scroll the process list. |
| <kbd>CTRL</kbd> + Left and Right Arrows | Change the process sort column. |
| <kbd>g</kbd> | Go to the top of the process list. |
| <kbd>G</kbd> | Go to the bottom of the process list. |
| <kbd>Space</kbd> | Tag a selected process. |
| <kbd>U</kbd> | Untag all tagged processes. |
| <kbd>K</kbd> | Kill all tagged processes. |
| <kbd>I</kbd> | Invert the sort order. |
| <kbd>F</kbd> | Follow process: if the sort order causes the currently selected process to move in the list, make the selection bar follow it. Moving the cursor manually automatically disables this feature. |
| <kbd>n</kbd> | Next in search. |
| <kbd>N</kbd> | Previous in search. |

### Vi commands

| Command(s) | Purpose |
|:---|:---|
| `:exec` CMD | Executes the given Windows command. |
| `:kill` PID(s) | Kill all given processes. |
| `:q`, `:quit` | Quit NTop. |
| `/PATTERN`, `:search` PATTERN | Do a search. |
| `:sort` COLUMN | Sort the process list after the given column. |
| `:tree` | View process tree. |

## Configuration

The color scheme can be customized through the [ntop.conf](ntop.conf) file. Follow link for example.

## Building

Use CMake or use the build.bat file. Only tested with Visual Studio 2017.

```sh
$ cmake . # For enabling Unicode support: cmake -DENABLE_UNICODE=ON .
```

## TODO

* ~~Figure out buggy resizing.~~
* ~~View process tree.~~
* ~~Searching.~~
* Filtering.
* All of htop's command line options.
* At least the most important interactive commands (e.g. ~~following processes~~).
