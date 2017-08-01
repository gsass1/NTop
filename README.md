# NTop
htop-like system-monitor for Windows. Because using Task Manager is not cool enough.

![NTop](https://user-images.githubusercontent.com/4589491/28242345-6a8fe79a-69a9-11e7-96d6-b1af9db9309c.png)

NTop as in Windows NT-op or NukeTop. Whatever you prefer (the latter obviously).

## Usage

### Options
| Option		| Meaning		|
| ------------- | ------------- |
| -C			| Use monochrome color scheme.  |
| -h			| Display help info.  |
| -p PID,PID...	| Show only the given PIDs.  |
| -s COLUMN		| Sort by this column.  |
| -u USERNAME	| Only display processes belonging to this user. |
| -v			| Print version.  |

### Interactive commands
| Key(s)		                       | Purpose		|
| ------------------------------------ | -------------- |
| Up and Down Arrows, PgUp and PgDown  | Scroll the process list.  |
| g									   | Go to the top of the process list.  |
| G									   | Go to the bottom of the process list.  |
| Space								   | Tag a selected process.  |
| U									   | Untag all tagged processes.  |
| F1 								   | Sort list by ID.  |
| F2 								   | Sort list by executable name.  |
| F3 								   | Sort list by user name.  |
| F4 								   | Sort list by CPU usage.  |
| F5 								   | Sort list by memory usage.  |
| F6 								   | Sort list by uptime.  |
| F7 								   | Execute a command.  |
| F8 								   | View process tree.  |
| F9 								   | Kill all tagged processes.  |
| F10, q 						       | Quit.  |
| I 								   | Invert the sort order.  |
| F 								   | Follow process: if the sort order causes the currently selected process to move in the list, make the selection bar follow it. Moving the cursor manually automatically disables this feature.  |

## Configuration
The color scheme can be customized through the [ntop.conf](ntop.conf) file. Follow link for example.

## Building
Use CMake. Only tested with Visual Studio 2017.

`cmake .`

For enabling Unicode support:

`cmake -DENABLE_UNICODE=ON .`

## TODO
* Figure out buggy resizing.
* Don't map all the function keys for sorting only. Rather let that be a distinctive menu like in htop.
* ~~View process tree.~~
* Searching.
* Filtering.
* All of htop's command line options.
* At least the most important interactive commands (e.g. following processes).