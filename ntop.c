/* 
 * NTop - an htop clone for Windows
 * Copyright (c) 2017 Gian Sass
 * 
 * This program is free software: you can redistribute it and/or modify  
 * it under the terms of the GNU General Public License as published by  
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but 
 * WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU 
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <Windows.h>
#include <psapi.h>
#include <lmcons.h>
#include <tchar.h>
#include <TlHelp32.h>
#include <assert.h>
#include <conio.h>
#include <Pdh.h>
#include <stdio.h>

#define NTOP_VER "0.0.1"

static int Width;
static int Height;
static int OldWidth;
static int OldHeight;
static int SizeX;
static int SizeY;
static const int ProcessWindowPosY = 6;
static DWORD ProcessWindowHeight;
static DWORD VisibleProcessCount;
static WORD SavedAttributes;
static HANDLE ConsoleHandle;
static CRITICAL_SECTION SyncLock;

#ifdef _MSC_VER
	#define NORETURN __declspec(noreturn)
#elif defined(__GNUC__) && defined(__MINGW32__)
	#define NORETURN __attribute__((noreturn))
#else
	#define NORETURN
#endif

static NORETURN void Die(TCHAR *Fmt, ...)
{
	TCHAR Buffer[1024];
	va_list VaList;

	va_start(VaList, Fmt);
	_vstprintf_s(Buffer, _countof(Buffer), Fmt, VaList);
	va_end(VaList);

	system("cls");
	WriteFile(GetStdHandle(STD_ERROR_HANDLE), Buffer, sizeof(*Buffer) * (_tcslen(Buffer) + 1), NULL, NULL);

	exit(EXIT_FAILURE);
}

#define FOREGROUND_WHITE (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE)
#define FOREGROUND_CYAN (FOREGROUND_INTENSITY | FOREGROUND_GREEN | FOREGROUND_BLUE)
#define BACKGROUND_WHITE (BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE)
#define BACKGROUND_CYAN (BACKGROUND_INTENSITY | BACKGROUND_GREEN | BACKGROUND_BLUE)

typedef struct config {
	WORD FGColor;
	WORD BGColor;
	WORD FGHighlightColor;
	WORD BGHighlightColor;
	WORD MenuBarColor;
	WORD ProcessListHeaderColor;
	WORD CPUBarColor;
	WORD MemoryBarColor;
	WORD PageMemoryBarColor;
} config;

static config Config = {
	/* Default values */
	FOREGROUND_WHITE,
	FOREGROUND_INTENSITY,
	FOREGROUND_CYAN,
	BACKGROUND_CYAN,
	BACKGROUND_BLUE,
	BACKGROUND_GREEN,
	FOREGROUND_RED,
	FOREGROUND_GREEN,
	FOREGROUND_GREEN,
};

static config MonochromeConfig = {
	FOREGROUND_WHITE,
	FOREGROUND_INTENSITY,
	FOREGROUND_INTENSITY | FOREGROUND_WHITE,
	BACKGROUND_WHITE,
	0,
	BACKGROUND_WHITE,
	FOREGROUND_WHITE,
	FOREGROUND_WHITE,
	FOREGROUND_WHITE,
};

static void ParseConfigLine(char *Line)
{
	const char *Delimeter = " \t\n";
	char *Context;
	char *Key = strtok_s(Line, Delimeter, &Context);
	if(!Key)
		return;
	if(Key[0] == '#') /* Comment char*/
		return;
	char *Value = strtok_s(NULL, Delimeter, &Context);
	if(!Value)
		return;

	WORD Num = (WORD)strtol(Value, NULL, 0);

	if(_strcmpi(Key, "FGColor") == 0) {
		Config.FGColor = Num;
	} else if(_strcmpi(Key, "BGColor") == 0) {
		Config.BGColor = Num;
	} else if(_strcmpi(Key, "FGHighlightColor") == 0) {
		Config.FGHighlightColor = Num;
	} else if(_strcmpi(Key, "FGHighlightColor") == 0) {
		Config.BGHighlightColor = Num;
	} else if(_strcmpi(Key, "MenuBarColor") == 0) {
		Config.MenuBarColor = Num;
	} else if(_strcmpi(Key, "ProcessListHeaderColor") == 0) {
		Config.ProcessListHeaderColor = Num;
	} else if(_strcmpi(Key, "CPUBarColor") == 0) {
		Config.CPUBarColor = Num;
	} else if(_strcmpi(Key, "MemoryBarColor") == 0) {
		Config.MemoryBarColor = Num;
	} else if(_strcmpi(Key, "PageMemoryBarColor") == 0) {
		Config.PageMemoryBarColor = Num;
	}
}

static void ReadConfigFile(void)
{
	FILE *File;
	errno_t Error;

	Error = fopen_s(&File, "ntop.conf", "r");
	if(Error != 0)
		return;

	char Buffer[256];
	while(fgets(Buffer, _countof(Buffer), File)) {
		ParseConfigLine(Buffer);
	}

	fclose(File);
}

static inline void SetColor(WORD Color)
{
	SetConsoleTextAttribute(ConsoleHandle, Color);
}

static void SetConCursorPos(SHORT X, SHORT Y)
{
	COORD Coord = { X, Y };
	SetConsoleCursorPosition(ConsoleHandle, Coord);
}

typedef struct process {
	HANDLE Handle;
	DWORD ID;
	TCHAR UserName[UNLEN];
	DWORD BasePriority;
	double PercentProcessorTime;
	DWORD UsedMemory;
	DWORD ThreadCount;
	ULONGLONG UpTime;
	TCHAR ExeName[MAX_PATH];
} process;

/*
 * 1024 entries ought to be enough for anybody. - Bill Gates
 * TODO: Dynamically allocate theses lists
 */
static process ProcessList[1024];
static process NewProcessList[1024];
static DWORD TaggedProcesses[1024];
static DWORD TaggedProcessesCount;

static void ToggleTaggedProcess(DWORD ID)
{
	for(DWORD i = 0; i < TaggedProcessesCount; i++) {
		if(TaggedProcesses[i] == ID) {
			for(; i < TaggedProcessesCount-1;i++) {
				TaggedProcesses[i] = TaggedProcesses[i+1];
			}
			TaggedProcessesCount--;
			return;
		}
	}

	TaggedProcesses[TaggedProcessesCount++] = ID;
}

static BOOL IsProcessTagged(DWORD ID)
{
	for(DWORD i = 0; i < TaggedProcessesCount; i++) {
		if(TaggedProcesses[i] == ID)
			return TRUE;
	}
	return FALSE;
}

static DWORD ProcessCount = 0;
static DWORD ProcessIndex = 0;
static DWORD SelectedProcessIndex = 0;
static DWORD SelectedProcessID = 0;

typedef int (*process_sort_fn_t)(const void *, const void *);

static int SortProcessByID(const void *A, const void *B)
{
	return ((const process *)B)->ID - ((const process *)A)->ID;
}

static int SortProcessByExe(const void *A, const void *B)
{
	return _tcsncmp(((const process *)A)->ExeName, ((const process *)B)->ExeName, MAX_PATH);
}

static int SortProcessByUserName(const void *A, const void *B)
{
	return _tcsncmp(((const process *)A)->UserName, ((const process *)B)->UserName, UNLEN);
}

static int SortProcessByProcessorTime(const void *A, const void *B)
{
	return (int)(((const process *)B)->PercentProcessorTime - ((const process *)A)->PercentProcessorTime);
}

static int SortProcessByUsedMemory(const void *A, const void *B)
{
	return (int)(((const process *)B)->UsedMemory - ((const process *)A)->UsedMemory);
}

static int SortProcessByUpTime(const void *A, const void *B)
{
	return (int)(((const process *)B)->UpTime - ((const process *)A)->UpTime);
}

static int SortProcessByPriority(const void *A, const void *B)
{
	return (int)(((const process *)B)->BasePriority - ((const process *)A)->BasePriority);
}

static int SortProcessByThreadCount(const void *A, const void *B)
{
	return (int)(((const process *)B)->ThreadCount - ((const process *)A)->ThreadCount);
}

typedef enum process_sort_type {
	SORT_BY_ID,
	SORT_BY_USER_NAME,
	SORT_BY_PRIORITY,
	SORT_BY_PROCESSOR_TIME,
	SORT_BY_USED_MEMORY,
	SORT_BY_THREAD_COUNT,
	SORT_BY_UPTIME,
	SORT_BY_EXE,
} process_sort_type;

static process_sort_type ProcessSortType = SORT_BY_ID;

static TCHAR OSName[256];
static DWORD CPUCoreCount;
static double CPUUsage;

static ULONGLONG SubtractTimes(const FILETIME *A, const FILETIME *B)
{
	LARGE_INTEGER lA, lB;

	lA.LowPart = A->dwLowDateTime;
	lA.HighPart = A->dwHighDateTime;

	lB.LowPart = B->dwLowDateTime;
	lB.HighPart = B->dwHighDateTime;

	return lA.QuadPart - lB.QuadPart;
}

static void SortProcessList(void)
{
	process_sort_fn_t SortFn = NULL;

	switch(ProcessSortType) {
	case SORT_BY_ID:
		SortFn = SortProcessByID;
		break;
	case SORT_BY_EXE:
		SortFn = SortProcessByExe;
		break;
	case SORT_BY_USER_NAME:
		SortFn = SortProcessByUserName;
		break;
	case SORT_BY_PROCESSOR_TIME:
		SortFn = SortProcessByProcessorTime;
		break;
	case SORT_BY_USED_MEMORY:
		SortFn = SortProcessByUsedMemory;
		break;
	case SORT_BY_UPTIME:
		SortFn = SortProcessByUpTime;
		break;
	case SORT_BY_PRIORITY:
		SortFn = SortProcessByPriority;
		break;
	case SORT_BY_THREAD_COUNT:
		SortFn = SortProcessByThreadCount;
		break;
	}

	if(SortFn) {
		qsort(ProcessList, ProcessCount, sizeof(*ProcessList), SortFn);
	}
}

static BOOL FilterByUserName = FALSE;
static TCHAR FilterUserName[UNLEN];

static BOOL FilterByPID = FALSE;
static DWORD PidFilterList[1024];
static DWORD PidFilterCount;

static void PollProcessList(void)
{
	HANDLE Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPALL, 0);
	if(!Snapshot) {
		Die(_T("CreateToolhelp32Snapshot failed: %ld\n"), GetLastError());
	}

	PROCESSENTRY32 Entry;
	Entry.dwSize = sizeof(Entry);

	BOOL Status = Process32First(Snapshot, &Entry);
	if(!Status) {
		Die(_T("Process32First failed: %ld\n"), GetLastError());
	}

	DWORD i = 0;

	for(; Status; Status = Process32Next(Snapshot, &Entry)) {
		process Process = { 0 };
		Process.ID = Entry.th32ProcessID;
		Process.ThreadCount = Entry.cntThreads;
		Process.BasePriority = Entry.pcPriClassBase;

		_tcsncpy_s(Process.ExeName, MAX_PATH, Entry.szExeFile, MAX_PATH);
		_tcsncpy_s(Process.UserName, UNLEN, _T("SYSTEM"), UNLEN);

		Process.Handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, Entry.th32ProcessID);
		if(Process.Handle) {
			PROCESS_MEMORY_COUNTERS ProcMemCounters;
			if(GetProcessMemoryInfo(Process.Handle, &ProcMemCounters, sizeof(ProcMemCounters))) {
				Process.UsedMemory = ProcMemCounters.WorkingSetSize;
			}

			HANDLE ProcessTokenHandle;
			if(OpenProcessToken(Process.Handle, TOKEN_READ, &ProcessTokenHandle)) {
				DWORD ReturnLength;

				GetTokenInformation(ProcessTokenHandle, TokenUser, NULL, 0, &ReturnLength);
				assert(GetLastError() == ERROR_INSUFFICIENT_BUFFER);
				PTOKEN_USER TokenUserStruct = malloc(ReturnLength);

				if(GetTokenInformation(ProcessTokenHandle, TokenUser, TokenUserStruct, ReturnLength, &ReturnLength)) {
					SID_NAME_USE NameUse;
					DWORD NameLength = UNLEN;
					TCHAR DomainName[MAX_PATH];
					DWORD DomainLength = MAX_PATH;

					if(!LookupAccountSid(NULL, TokenUserStruct->User.Sid, Process.UserName, &NameLength, DomainName, &DomainLength, &NameUse)) {
						DWORD Error = GetLastError();
						DebugBreak();
					}
				}
				CloseHandle(ProcessTokenHandle);
				free(TokenUserStruct);
			}
		}

		if(FilterByUserName && lstrcmpi(Process.UserName, FilterUserName) != 0) {
			continue;
		}

		if(FilterByPID) {
			BOOL InFilter = FALSE;
			for(DWORD i = 0; i < PidFilterCount; i++) {
				if(PidFilterList[i] == Process.ID) {
					InFilter = TRUE;
				}
			}

			if(!InFilter) {
				continue;
			}
		}

		NewProcessList[i++] = Process;
	}

	CloseHandle(Snapshot);

	DWORD NewProcessCount = i;

	typedef struct system_times
	{
		FILETIME IdleTime, KernelTime, UserTime;
	} system_times;

	system_times PrevSysTimes;

	GetSystemTimes(&PrevSysTimes.IdleTime, &PrevSysTimes.KernelTime, &PrevSysTimes.UserTime);

	typedef struct process_times
	{
		FILETIME CreationTime, ExitTime, KernelTime, UserTime; 
	} process_times;

	process_times *ProcessTimes = (process_times *)malloc(NewProcessCount * sizeof(*ProcessTimes));

	for(DWORD i = 0; i < NewProcessCount; i++) {
		const process *Process = &NewProcessList[i];
		process_times *ProcessTime = &ProcessTimes[i];
		if(Process->Handle) {
			GetProcessTimes(Process->Handle, &ProcessTime->CreationTime, &ProcessTime->ExitTime, &ProcessTime->KernelTime, &ProcessTime->UserTime);
		}
	}

	Sleep(200);

	system_times SysTimes;

	GetSystemTimes(&SysTimes.IdleTime, &SysTimes.KernelTime, &SysTimes.UserTime);

	ULONGLONG SysKernelDiff = SubtractTimes(&SysTimes.KernelTime, &PrevSysTimes.KernelTime);
	ULONGLONG SysUserDiff = SubtractTimes(&SysTimes.UserTime, &PrevSysTimes.UserTime);
	ULONGLONG SysIdleDiff = SubtractTimes(&SysTimes.IdleTime, &PrevSysTimes.IdleTime);

	for(DWORD i = 0; i < NewProcessCount; i++) {
		process_times ProcessTime = { 0 };
		process_times *PrevProcessTime = &ProcessTimes[i];
		process *Process = &NewProcessList[i];
		if(Process->Handle) {
			GetProcessTimes(Process->Handle, &ProcessTime.CreationTime, &ProcessTime.ExitTime, &ProcessTime.KernelTime, &ProcessTime.UserTime);

			ULONGLONG ProcKernelDiff = SubtractTimes(&ProcessTime.KernelTime, &PrevProcessTime->KernelTime);
			ULONGLONG ProcUserDiff = SubtractTimes(&ProcessTime.UserTime, &PrevProcessTime->UserTime);

			ULONGLONG TotalSys = SysKernelDiff + SysUserDiff;
			ULONGLONG TotalProc = ProcKernelDiff + ProcUserDiff;

			if(TotalSys > 0) {
				Process->PercentProcessorTime = (double)((100.0 * (double)TotalProc) / (double)TotalSys);
			}

			FILETIME SysTime;
			GetSystemTimeAsFileTime(&SysTime);

			Process->UpTime = SubtractTimes(&SysTime, &ProcessTime.CreationTime) / 10000;

			CloseHandle(Process->Handle);
			Process->Handle = NULL;
		}
	}

	free(ProcessTimes);

	/* Since we have the values already we can compute CPU usage too */
	ULONGLONG SysTime = SysKernelDiff + SysUserDiff;
	if(SysTime > 0) {
		double Percentage = (double)(SysTime - SysIdleDiff) / (double)SysTime;
		CPUUsage = min(Percentage, 1.0);
	}

	EnterCriticalSection(&SyncLock);

	memcpy(&ProcessList, &NewProcessList, sizeof(process) * NewProcessCount);
	ProcessCount = NewProcessCount;
	SortProcessList();

	/* After process list update ProcessIndex and SelectedProcessIndex may become out of range */
	ProcessIndex = min(ProcessIndex, ProcessCount - VisibleProcessCount + 1);
	SelectedProcessIndex = min(SelectedProcessIndex, ProcessCount - 1);

	LeaveCriticalSection(&SyncLock);
}

static void DisableCursor(void)
{
	CONSOLE_CURSOR_INFO CursorInfo;
	GetConsoleCursorInfo(ConsoleHandle, &CursorInfo);
	CursorInfo.bVisible = FALSE; /* Note: this has to be set every time console buffer resizes! */
	SetConsoleCursorInfo(ConsoleHandle, &CursorInfo);
}

static BOOL PollConsoleInfo(void)
{
	CONSOLE_SCREEN_BUFFER_INFO Csbi;
	GetConsoleScreenBufferInfo(ConsoleHandle, &Csbi);
	Width = Csbi.srWindow.Right - Csbi.srWindow.Left + 1;
	Height = Csbi.srWindow.Bottom - Csbi.srWindow.Top + 1;

	if(Width != OldWidth || Height != OldHeight) {
		DisableCursor();
		COORD Size = { Width, Height };
		SetConsoleScreenBufferSize(ConsoleHandle, Size);

		OldWidth = Width;
		OldHeight = Height;

		return TRUE;
	}

	SizeX = (int)Csbi.dwSize.X;
	SizeY = (int)Csbi.dwSize.Y;

	if(SavedAttributes == 0)
		SavedAttributes = Csbi.wAttributes;

	return FALSE;
}

static TCHAR ComputerName[MAX_COMPUTERNAME_LENGTH + 1] = _T("localhost");
static double CPUFrequency;
static TCHAR CPUName[256];

static ULONGLONG TotalMemory;
static ULONGLONG UsedMemory;
static double UsedMemoryPerc;

static ULONGLONG TotalPageMemory;
static ULONGLONG UsedPageMemory;
static double UsedPageMemoryPerc;

#define TO_MB(X) ((X)/1048576)

static ULONGLONG UpTime;

static void PollInitialSystemInfo(void)
{
	LARGE_INTEGER PerformanceFrequency;
	QueryPerformanceFrequency(&PerformanceFrequency);
	CPUFrequency = (double)PerformanceFrequency.QuadPart / 1000000.0;

	SYSTEM_INFO SystemInfo;
	GetSystemInfo(&SystemInfo);
	CPUCoreCount = SystemInfo.dwNumberOfProcessors;

	HKEY Key;
	if(SUCCEEDED(RegOpenKey(HKEY_LOCAL_MACHINE, _T("HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0\\"), &Key))) {
		DWORD Count = 256;
		if(SUCCEEDED(RegQueryValueEx(Key, _T("ProcessorNameString"), NULL, NULL, (LPBYTE)&CPUName[0], &Count))) {
			RegCloseKey(Key);
		}
	}

	DWORD ComputerNameSize = MAX_COMPUTERNAME_LENGTH + 1;
	GetComputerName(ComputerName, &ComputerNameSize);
}

static void PollSystemInfo(void)
{
	MEMORYSTATUSEX MemoryInfo;
	MemoryInfo.dwLength = sizeof(MEMORYSTATUSEX);
	GlobalMemoryStatusEx(&MemoryInfo);
	TotalMemory = TO_MB(MemoryInfo.ullTotalPhys);
	UsedMemory = TO_MB(MemoryInfo.ullTotalPhys - MemoryInfo.ullAvailPhys);
	UsedMemoryPerc = (double)UsedMemory / (double)TotalMemory;

	TotalPageMemory = TO_MB(MemoryInfo.ullTotalPageFile);
	UsedPageMemory = TO_MB(MemoryInfo.ullTotalPageFile - MemoryInfo.ullAvailPageFile);
	UsedPageMemoryPerc = (double)UsedPageMemory / (double)TotalPageMemory;

	UpTime = GetTickCount64();
}

static HANDLE ProcessListThread;

DWORD WINAPI PollProcessListThreadProc(LPVOID lpParam)
{
	UNREFERENCED_PARAMETER(lpParam);

	while(1) {
		PollProcessList();
		Sleep(800);
	}
	return 0;
}

typedef enum input_mode {
	EXEC,
} input_mode;

static input_mode InputMode = EXEC;

#define TIME_STR_SIZE 12

/*
 * Create a dd:hh:mm:ss time-string similar to how the modern taskmgr does it.
 *
 * The restriction of the day count to two digits is justified by the obvious
 * fact that a Windows system cannot possibly reach an uptime of over 100 days
 * without crashing.
 */
static void FormatTimeString(TCHAR *Buffer, ULONGLONG MS)
{
	int Seconds = (int)(MS / 1000 % 60);
	int Minutes = (int)(MS / 60000 % 60);
	int Hours = (int)(MS / 3600000 % 24);
	int Days = (int)(MS / 86400000);
	wsprintf(Buffer, _T("%02d:%02d:%02d:%02d"), Days, Hours, Minutes, Seconds);
}

static void WriteBlankLine(void)
{
	for(int i = 0; i < Width; i++)
		putchar(' ');
}

static BOOL WINAPI CtrlHandler(DWORD signal)
{
	exit(EXIT_SUCCESS);
	return TRUE;
}

typedef struct process_list_column {
	TCHAR *Name;
	int Width;
} process_list_column;

static void DrawProcessListHeader(const process_list_column *Columns, int Count)
{
	int CharsWritten = 0;
	for(int i = 0; i < Count; i++) {
		if(i == (int)ProcessSortType) {
			SetColor(Config.BGHighlightColor);
		} else {
			SetColor(Config.ProcessListHeaderColor);
		}
		CharsWritten += _tprintf(_T("%*s  "), Columns[i].Width, Columns[i].Name);
	}

	for(; CharsWritten < Width; CharsWritten++) {
		putchar(' ');
	}
}

typedef struct options_column {
	TCHAR *Key;
	TCHAR *Name;
} options_column;

static void DrawOptions(const options_column *Columns, int Count)
{
	int CharsWritten = 0;
	for(int i = 0; i < Count; i++) {
		SetColor(Config.FGColor);
		CharsWritten += _tprintf(_T("%s"), Columns[i].Key);
		SetColor(Config.BGHighlightColor);
		CharsWritten += _tprintf(_T("%-4s"), Columns[i].Name);
	}

	for(; CharsWritten < Width-1; CharsWritten++) {
		putchar(' ');
	}
}

#define BAR_WIDTH 25

static int DrawPercentageBar(TCHAR *Name, double Percentage, WORD Color)
{
	int CharsWritten = 0;

	SetColor(Config.FGHighlightColor);
	CharsWritten += _tprintf(_T("  %s"), Name);
	SetColor(Config.FGColor);
	putchar('[');
	CharsWritten++;

	int Bars = (int)((double)BAR_WIDTH * Percentage);
	SetColor(Color);
	for(int i = 0; i < Bars; i++) {
		putchar('|');
	}
	CharsWritten+= Bars;
	SetColor(Config.FGColor);
	for(int i = 0; i < BAR_WIDTH - Bars; i++) {
		putchar(' ');
	}
	CharsWritten += BAR_WIDTH - Bars;
	SetColor(Config.BGColor);
	CharsWritten += _tprintf(_T("%04.1f%%"), 100.0 * Percentage);
	SetColor(Config.FGColor);
	putchar(']');
	CharsWritten++;
	return CharsWritten;
}

static void RestoreConsole(void)
{
	SetColor(SavedAttributes);
	CONSOLE_CURSOR_INFO CursorInfo;
	GetConsoleCursorInfo(ConsoleHandle, &CursorInfo);
	CursorInfo.bVisible = TRUE;
	SetConsoleCursorInfo(ConsoleHandle, &CursorInfo);
}

static void SetupProcessColor(const process *Process, BOOL Highlighted)
{
	WORD Color = Config.FGColor;
	BOOL Selected = IsProcessTagged(Process->ID);
	if(Highlighted) {
		if(Selected) {
			Color = Config.BGColor | Config.BGHighlightColor;
		} else {
			Color = Config.BGHighlightColor;
		}
	} else if(Selected) {
		Color = Config.BGColor;
	}
	SetColor(Color);
}

static void WriteProcessInfo(const process *Process)
{
	TCHAR UpTimeStr[TIME_STR_SIZE];
	FormatTimeString(UpTimeStr, Process->UpTime);

	int CharsWritten = _tprintf(_T("\n%6u  %9s  %3u  %04.1f%%  % 6.1f MB  %4u  %s  %s"),
			Process->ID,
			Process->UserName,
			Process->BasePriority,
			Process->PercentProcessorTime,
			(double)Process->UsedMemory / 1000000.0,
			Process->ThreadCount,
			UpTimeStr,
			Process->ExeName
			);


	_tprintf(_T("%*c"), Width-CharsWritten+1, ' ');
}

static void PrintVersion(void)
{
	_tprintf(_T("NTop " NTOP_VER " - (C) 2017 Gian Sass\n"));
	_tprintf(_T("Compiled on " __DATE__ " " __TIME__ "\n"));

#ifdef _UNICODE
	TCHAR UnicodeEnabled[] = _T("Yes");
#else
	TCHAR UnicodeEnabled[] = _T("No");
#endif
	_tprintf(_T("Unicode version: %s\n\n"), UnicodeEnabled);
}

int _tmain(int argc, TCHAR *argv[])
{
	BOOL Monochrome = FALSE;

	for(int i = 1; i < argc; i++) {
		if(argv[i][0] == _T('-') && _tcslen(argv[i]) == 2) {
			switch(argv[i][1]) {
			case 'C':
				Monochrome = TRUE;
				break;
			case 'h':
				PrintVersion();
				_tprintf(_T("Usage: %s [OPTIONS]\n\n"), argv[0]);
				_tprintf(_T("Options:\n"));
				_tprintf(_T("\t-C\tUse a monochrome color scheme\n"));
				_tprintf(_T("\t-h\tDisplay this\n"));
				_tprintf(_T("\t-p PID,PID...\n\t\tShow only the given PIDs\n"));
				_tprintf(_T("\t-s COLUMN\n\t\tSort by this column\n"));
				_tprintf(_T("\t-u USERNAME\n\t\tDisplay only the processes of this user\n"));
				_tprintf(_T("\t-v\tPrint version\n"));
				_tprintf(_T("\nInteractive commands:\n"));
				_tprintf(_T("\tUp and Down Arrows, PgUp and PgDown\n\t\tScroll through the process list.\n"));
				_tprintf(_T("\tg\tGo to the top of the list.\n"));
				_tprintf(_T("\tG\tGo to the bottom of the list.\n"));
				_tprintf(_T("\tSpace\tTag or untag selected process.\n"));
				_tprintf(_T("\tU\tUntag all tagged processes.\n"));
				_tprintf(_T("\tF1\tSort list by ID.\n"));
				_tprintf(_T("\tF2\tSort list by executable name.\n"));
				_tprintf(_T("\tF3\tSort list by user name.\n"));
				_tprintf(_T("\tF4\tSort list by CPU usage.\n"));
				_tprintf(_T("\tF5\tSort list by memory usage.\n"));
				_tprintf(_T("\tF6\tSort list by uptime.\n"));
				_tprintf(_T("\tF7\tExecute a command.\n"));
				_tprintf(_T("\tF9\tKill all tagged processes.\n"));
				_tprintf(_T("\tF10, q\tQuit.\n"));
				return EXIT_SUCCESS;
			case 's':
				if(++i < argc) {
					if(!lstrcmpi(argv[i], _T("ID"))) {
						ProcessSortType = SORT_BY_ID;
					} else if(!lstrcmpi(argv[i], _T("USER"))) {
						ProcessSortType = SORT_BY_USER_NAME;
					} else if(!lstrcmpi(argv[i], _T("PRI"))) {
						ProcessSortType = SORT_BY_PRIORITY;
					} else if(!lstrcmpi(argv[i], _T("CPU%"))) {
						ProcessSortType = SORT_BY_PROCESSOR_TIME;
					} else if(!lstrcmpi(argv[i], _T("MEM"))) {
						ProcessSortType = SORT_BY_USED_MEMORY;
					} else if(!lstrcmpi(argv[i], _T("THRD"))) {
						ProcessSortType = SORT_BY_THREAD_COUNT;
					} else if(!lstrcmpi(argv[i], _T("TIME"))) {
						ProcessSortType = SORT_BY_UPTIME;
					} else if(!lstrcmpi(argv[i], _T("EXE"))) {
						ProcessSortType = SORT_BY_EXE;
					} else {
						_tprintf(_T("Unknown column: '%s'\n"), argv[i]);
						return EXIT_FAILURE;
					}
				}
				break;
			case 'u':
				if(++i < argc) {
					FilterByUserName = TRUE;
					_tcscpy_s(FilterUserName, UNLEN, argv[i]);
				}
				break;
			case 'v':
				PrintVersion();
				return EXIT_SUCCESS;
			case 'p':
				if(++i < argc) {
					const TCHAR *Delim = _T(",");
					TCHAR *Context;
					TCHAR *Token = _tcstok_s(argv[i], Delim, &Context);
					while(Token) {
						PidFilterList[PidFilterCount++] = (DWORD)_tstoi(Token);
						Token = _tcstok_s(NULL, Delim, &Context);
					}

					if(PidFilterCount != 0) {
						FilterByPID = TRUE;
					}
				}
				break;
			default:
				_tprintf(_T("Unknown option: '%c'"), argv[i][1]);
				return EXIT_FAILURE;
			}
		}
	}

	InitializeCriticalSection(&SyncLock);
	SetConsoleCtrlHandler(CtrlHandler, TRUE);
	ConsoleHandle = GetStdHandle(STD_OUTPUT_HANDLE);

	atexit(RestoreConsole);

	if(Monochrome) {
		Config = MonochromeConfig;
	} else {
		ReadConfigFile();
	}

	PollConsoleInfo();
	PollInitialSystemInfo();
	PollSystemInfo();
	PollProcessList();

	TCHAR MenuBar[256] = { 0 };
	wsprintf(MenuBar, _T("NTop on %s"), ComputerName);

	ULONGLONG StartTicks = GetTickCount64();
	ULONGLONG LastKeyPress = 0;
	ULONGLONG KeyPressStart = 0;
	BOOL KeyPress = FALSE;

	BOOL InInputMode = FALSE;
	TCHAR Input[256] = { 0 };
	TCHAR InputModeStr[64] = { 0 };
	int InputIndex = 0;

	ProcessListThread = CreateThread(NULL, 0, PollProcessListThreadProc, NULL, 0, NULL);

	while(1) {
#if _DEBUG
		ULONGLONG T1 = GetTickCount64();
#endif

		SetConCursorPos(0, 0);
		SetColor(Config.FGColor | Config.MenuBarColor);

		int MenuBarOffsetX = Width / 2 - (int)_tcslen(MenuBar) / 2;
		for(int i = 0; i < MenuBarOffsetX; i++) {
			putchar(' ');
		}

		_tprintf(_T("%s"), MenuBar);

		for(int i = 0; i < Width - MenuBarOffsetX - (int)_tcslen(MenuBar); i++) {
			putchar(' ');
		}

		SetColor(Config.FGColor);
		WriteBlankLine();

		/* CPU */
		int CharsWritten = 0;

		CharsWritten += DrawPercentageBar(_T("CPU"), CPUUsage, Config.CPUBarColor);

		SetColor(Config.FGHighlightColor);
		CharsWritten += _tprintf(_T("  Name: "));
		SetColor(Config.FGColor);
		CharsWritten +=	_tprintf(_T("%s (%u Cores)"), CPUName, CPUCoreCount);

		SetColor(Config.FGHighlightColor);
		CharsWritten +=	_tprintf(_T("  Tasks: "));
		SetColor(Config.FGColor);
		CharsWritten +=	_tprintf(_T("%u"), ProcessCount);

		for(; CharsWritten < Width; CharsWritten++) {
			putchar(' ');
		}

		/* Memory */
		CharsWritten = DrawPercentageBar(_T("Mem"), UsedMemoryPerc, Config.MemoryBarColor);

		SetColor(Config.FGHighlightColor);
		CharsWritten += _tprintf(_T("  Size: "));
		SetColor(Config.FGColor);
		CharsWritten += _tprintf(_T("%d GB"), (int)TotalMemory/1000);

		for(; CharsWritten < Width; CharsWritten++) {
			putchar(' ');
		}

		CharsWritten = DrawPercentageBar(_T("Pge"), UsedPageMemoryPerc, Config.PageMemoryBarColor);

		SetColor(Config.FGHighlightColor);
		CharsWritten += _tprintf(_T("  Uptime: "));

		TCHAR Buffer[TIME_STR_SIZE];
		FormatTimeString(Buffer, UpTime);
		SetColor(Config.FGColor);
		CharsWritten +=	_tprintf(_T("%s"), Buffer);
		SetColor(Config.FGColor);

		for(; CharsWritten < Width; CharsWritten++) {
			putchar(' ');
		}

		WriteBlankLine();

		ProcessWindowHeight = Height - ProcessWindowPosY;
		VisibleProcessCount = ProcessWindowHeight - 2;

		static process_list_column ProcessListColumns[] = {
			{ _T("ID"),	6 },
			{ _T("USER"),	9 },
			{ _T("PRI"),	3 },
			{ _T("CPU%"),	5 },
			{ _T("MEM"),	9 },
			{ _T("THRD"),	4 },
			{ _T("TIME"),	TIME_STR_SIZE - 1 },
			{ _T("EXE"),	-1 },
		};

		DrawProcessListHeader(ProcessListColumns, _countof(ProcessListColumns));

		CharsWritten = 0;

		EnterCriticalSection(&SyncLock);
		DWORD Count = 0;
		for(DWORD i = 0; i < VisibleProcessCount; i++) {
			DWORD PID = i+ProcessIndex;
			if(PID < ProcessCount) {
				const process *Process = &ProcessList[PID];
				SetupProcessColor(Process, PID == SelectedProcessIndex);
				SetConCursorPos(0, (SHORT)(i + ProcessWindowPosY));
				WriteProcessInfo(Process);
				Count++;
			}
		}
		LeaveCriticalSection(&SyncLock);

		SetColor(0);
		for(DWORD i = Count; i < VisibleProcessCount - 1; i++) {
			WriteBlankLine();
		}

		SetConCursorPos(0, (SHORT)Height-2);
		if(!InInputMode) {
			static options_column OptionColumns[] = {
				{_T("F1"), _T("ID")},
				{_T("F2"), _T("EXE")},
				{_T("F3"), _T("USER")},
				{_T("F4"), _T("CPU%")},
				{_T("F5"), _T("MEM")},
				{_T("F6"), _T("TIME")},
				{_T("F7"), _T("EXEC")},
				{_T("F9"), _T("KILL")},
				{_T("F10"), _T("QUIT")},
			};

			putchar('\n');
			DrawOptions(OptionColumns, _countof(OptionColumns));
		} else {
			SetColor(Config.BGHighlightColor);
			CharsWritten = _tprintf(_T("\n%s: %s_"), InputModeStr, Input);
			for(; CharsWritten < Width; CharsWritten++) {
				putchar(' ');
			}
		}

		SetColor(FOREGROUND_WHITE);

#ifdef _DEBUG
		ULONGLONG T2 = GetTickCount64();

		ULONG Diff = (ULONG)(T2 - T1);

		TCHAR DebugBuffer[256];
		wsprintf(DebugBuffer, _T("Drawing took: %lu ms\n"), Diff);
		OutputDebugString(DebugBuffer);
#endif

		DWORD OldSelectedProcessIndex = 0;
		process_sort_type NewProcessSortType = ProcessSortType;

		while(TRUE) {
			BOOL RedrawAtCursor = FALSE;
			if(!InInputMode) {
				if(GetTickCount64() - LastKeyPress >= 20LL) {
					ULONGLONG Now = GetTickCount64();
					if(GetAsyncKeyState(VK_UP) || GetAsyncKeyState(VK_PRIOR)) {
						if(!KeyPress) {
							KeyPress = TRUE;
							KeyPressStart = GetTickCount64();
						}
						if(Now == KeyPressStart || Now - KeyPressStart > 500) {
							if(SelectedProcessIndex != 0) {
								RedrawAtCursor = TRUE;
								OldSelectedProcessIndex = SelectedProcessIndex;
								SelectedProcessIndex--;
								LastKeyPress = GetTickCount64();

								if(SelectedProcessIndex <= ProcessIndex - 1) {
									if(ProcessIndex != 0) {
										ProcessIndex--;
										break;
									}
								}
							}
						}
					} else if(GetAsyncKeyState(VK_DOWN) || GetAsyncKeyState(VK_NEXT)) {
						if(!KeyPress) {
							KeyPress = TRUE;
							KeyPressStart = Now; 
						}

						if(Now == KeyPressStart || Now - KeyPressStart > 500) {
							if(SelectedProcessIndex != ProcessCount-1) {
								RedrawAtCursor = TRUE;
								OldSelectedProcessIndex = SelectedProcessIndex;
								SelectedProcessIndex++;
								LastKeyPress = GetTickCount64();
							}
							if(SelectedProcessIndex-ProcessIndex >= ProcessWindowHeight - 2) {
								if(ProcessIndex <= ProcessCount-1-ProcessWindowHeight) {
									ProcessIndex++;
									break;
								}
							}
						}
					} else {
						KeyPress = FALSE;
					}
				}

				if(GetAsyncKeyState(0x47)) { /* g */
					if(GetAsyncKeyState(VK_SHIFT)) {
						SelectedProcessIndex = ProcessCount - 1;
						ProcessIndex = SelectedProcessIndex - VisibleProcessCount + 1; 
					} else {
						ProcessIndex = 0;
						SelectedProcessIndex = 0;
					}
					break;
				} else if(GetAsyncKeyState(VK_SHIFT) && GetAsyncKeyState(0x55)) /* u */ {
					if(TaggedProcessesCount != 0) {
						TaggedProcessesCount = 0;
						break;
					}
				} else if(GetAsyncKeyState(VK_SPACE) == -32767) {
					ToggleTaggedProcess(ProcessList[SelectedProcessIndex].ID);
					RedrawAtCursor = TRUE;
					OldSelectedProcessIndex = SelectedProcessIndex;
				} else if(GetAsyncKeyState(VK_F1)) {
					NewProcessSortType = SORT_BY_ID;
					break;
				} else if(GetAsyncKeyState(VK_F2)) {
					NewProcessSortType = SORT_BY_EXE;
					break;
				} else if(GetAsyncKeyState(VK_F3)) {
					NewProcessSortType = SORT_BY_USER_NAME;
					break;
				} else if(GetAsyncKeyState(VK_F4)) {
					NewProcessSortType = SORT_BY_PROCESSOR_TIME;
					break;
				} else if(GetAsyncKeyState(VK_F5)) {
					NewProcessSortType = SORT_BY_USED_MEMORY;
					break;
				} else if(GetAsyncKeyState(VK_F6)) {
					NewProcessSortType = SORT_BY_UPTIME;
					break;
				} else if(GetAsyncKeyState(VK_F7)) {
					InputMode = EXEC;
					InInputMode = TRUE;
					_tcsncpy_s(InputModeStr, _countof(InputModeStr), _T("Command"), 8);
					break;
				} else if(GetAsyncKeyState(VK_F9)) {
					if(TaggedProcessesCount != 0) {
						EnterCriticalSection(&SyncLock);
						for(DWORD i = 0; i < TaggedProcessesCount; i++) {
							DWORD PID = TaggedProcesses[i];
							HANDLE Handle = OpenProcess(PROCESS_TERMINATE, FALSE, PID);
							if(Handle) {
								TerminateProcess(Handle, 9);
								CloseHandle(Handle);
							}
						}
						TaggedProcessesCount = 0;
						LeaveCriticalSection(&SyncLock);
					}
				} else if(GetAsyncKeyState(VK_F10) || GetAsyncKeyState(0x51)) /* q */ {
					exit(EXIT_SUCCESS);
				}

				if(RedrawAtCursor) {
					SetConCursorPos(0, (SHORT)(ProcessWindowPosY + SelectedProcessIndex - ProcessIndex));
					SetupProcessColor(&ProcessList[SelectedProcessIndex], TRUE);
					WriteProcessInfo(&ProcessList[SelectedProcessIndex]);

					if(OldSelectedProcessIndex != SelectedProcessIndex) {
						SetConCursorPos(0, (SHORT)(ProcessWindowPosY + OldSelectedProcessIndex - ProcessIndex));
						SetupProcessColor(&ProcessList[OldSelectedProcessIndex], FALSE);
						WriteProcessInfo(&ProcessList[OldSelectedProcessIndex]);
					}
				}

			} else {
				if(_kbhit()) {
					char c = _getch();
					if(c == '\n' || c == '\r') {
						switch(InputMode) {
							case EXEC: {
								STARTUPINFO StartupInfo;
								PROCESS_INFORMATION ProcInfo;
								ZeroMemory(&StartupInfo, sizeof(StartupInfo));
								ZeroMemory(&ProcInfo, sizeof(ProcInfo));
								StartupInfo.cb = sizeof(StartupInfo);

								CreateProcess(NULL, Input, NULL, NULL, FALSE, 0, NULL, NULL, &StartupInfo, &ProcInfo);

								break;
							}

							InputIndex = 0;
							Input[0] = '\0';
						}

						InInputMode = FALSE;
						break;
					} else {
						if(c == 0 || c == -32) {
							_getch();
						} else if(c == '\x1b') {
							InInputMode = FALSE;
							break;
						} else if(c > 0 && isprint(c) && c != '?') {
							if(InputIndex < _countof(Input)-1) {
								Input[InputIndex++] = c;
							}
						} else {
							if(c == '\b') {
								if(InputIndex != 0)
									Input[--InputIndex] = '\0';
							}
						}
						break;
					}
				}
			}
			if(GetTickCount64() - StartTicks >= 1000LL) {
				PollSystemInfo();
				StartTicks = GetTickCount64();
				break;
			}
			Sleep(30);
			if(PollConsoleInfo()) break;
		}

		if(NewProcessSortType != ProcessSortType) {
			EnterCriticalSection(&SyncLock);
			ProcessSortType = NewProcessSortType;
			SortProcessList();
			LeaveCriticalSection(&SyncLock);
		}
	}

	WaitForSingleObject(ProcessListThread, INFINITE);
	return EXIT_SUCCESS;
}
