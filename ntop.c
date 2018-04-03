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

#include <windows.h>
#include <psapi.h>
#include <lmcons.h>
#include <tchar.h>
#include <tlhelp32.h>
#include <assert.h>
#include <conio.h>
#include <pdh.h>
#include <stdio.h>
#include "ntop.h"
#include "util.h"
#include "vi.h"

#define NTOP_VER "0.2.0"
#define SCROLL_INTERVAL 20ULL
#define REDRAW_INTERVAL 1000ULL
#define INPUT_LOOP_DELAY 30
#define CARET_INTERVAL 500

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
HANDLE ConsoleHandle;
static HANDLE OldConsoleHandle;
static CRITICAL_SECTION SyncLock;

static int ConPrintf(TCHAR *Fmt, ...)
{
	TCHAR Buffer[1024];
	va_list VaList;

	va_start(VaList, Fmt);
	int CharsWritten = _vstprintf_s(Buffer, _countof(Buffer), Fmt, VaList);
	va_end(VaList);

	DWORD Dummy;
	WriteConsole(ConsoleHandle, Buffer, CharsWritten, &Dummy, NULL);

	return CharsWritten;
}

static void ConPutc(char c)
{
	DWORD Dummy;
	WriteConsole(ConsoleHandle, &c, 1, &Dummy, NULL);
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
	WORD ErrorColor;
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
	BACKGROUND_RED | FOREGROUND_WHITE,
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
	BACKGROUND_WHITE,
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
	} else if(_strcmpi(Key, "ErrorColor") == 0) {
		Config.ErrorColor = Num;
	}
}

#define BUF_INCREASE 256

static void ReadConfigFile(void)
{
	FILE *File;
	errno_t Error;

	Error = fopen_s(&File, "ntop.conf", "r");
	if(Error != 0)
		return;

	size_t BufferSize = BUF_INCREASE;
	size_t Offset = 0;
	char *Buffer = xmalloc(BUF_INCREASE);

	while(1) {
		if(!fgets(Offset + Buffer, BufferSize - Offset, File)) {
			break;
		}

		if(!feof(File) && !strchr(Buffer, '\n')) {
			Offset = BufferSize - 1;
			BufferSize += BUF_INCREASE;
			Buffer = xrealloc(Buffer, BufferSize);
		} else {
			Offset = 0;
			ParseConfigLine(Buffer);
		}
	}

	free(Buffer);
	fclose(File);
}

static WORD CurrentColor;

static void SetColor(WORD Color)
{
	CurrentColor = Color;
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
	DWORD ParentPID;
	DWORD TreeDepth;
} process;

#define PROCLIST_BUF_INCREASE 64

static process *ProcessList;
static process *NewProcessList;
static DWORD ProcessListSize = PROCLIST_BUF_INCREASE;

static DWORD *TaggedProcessList;
static DWORD TaggedProcessListCount;
static DWORD TaggedProcessListSize = PROCLIST_BUF_INCREASE;

static void IncreaseProcListSize(void)
{
	ProcessListSize += PROCLIST_BUF_INCREASE;
	NewProcessList = xrealloc(NewProcessList, ProcessListSize * sizeof *NewProcessList);
	ProcessList = xrealloc(ProcessList, ProcessListSize * sizeof *ProcessList);
}

static void ToggleTaggedProcess(DWORD ID)
{
	for(DWORD i = 0; i < TaggedProcessListCount; i++) {
		if(TaggedProcessList[i] == ID) {
			for(; i < TaggedProcessListCount-1;i++) {
				TaggedProcessList[i] = TaggedProcessList[i+1];
			}
			TaggedProcessListCount--;
			return;
		}
	}

	TaggedProcessList[TaggedProcessListCount++] = ID;

	if(TaggedProcessListCount >= TaggedProcessListSize) {
		TaggedProcessListSize += PROCLIST_BUF_INCREASE;
		TaggedProcessList = xrealloc(TaggedProcessList, PROCLIST_BUF_INCREASE * sizeof *TaggedProcessList);
	}
}

static BOOL IsProcessTagged(DWORD ID)
{
	for(DWORD i = 0; i < TaggedProcessListCount; i++) {
		if(TaggedProcessList[i] == ID)
			return TRUE;
	}
	return FALSE;
}

static DWORD ProcessCount = 0;
static DWORD RunningProcessCount = 0;
static DWORD ProcessIndex = 0;
static DWORD SelectedProcessIndex = 0;
static DWORD SelectedProcessID = 0;
static BOOL FollowProcess = FALSE;
static DWORD FollowProcessID = 0;

typedef enum sort_order {
	ASCENDING,
	DESCENDING
} sort_order;

static sort_order SortOrder = DESCENDING;

typedef int (*process_sort_fn_t)(const void *, const void *);

static int SortProcessByID(const void *A, const void *B)
{
	if(SortOrder == ASCENDING)
		return ((const process *)A)->ID - ((const process *)B)->ID;
	else
		return ((const process *)B)->ID - ((const process *)A)->ID;
}

static int SortProcessByExe(const void *A, const void *B)
{
	if(SortOrder == ASCENDING)
		return _tcsncicmp(((const process *)A)->ExeName, ((const process *)B)->ExeName, MAX_PATH);
	else
		return _tcsncicmp(((const process *)B)->ExeName, ((const process *)A)->ExeName, MAX_PATH);
}

static int SortProcessByUserName(const void *A, const void *B)
{
	if(SortOrder == ASCENDING)
		return _tcsncmp(((const process *)A)->UserName, ((const process *)B)->UserName, UNLEN);
	else
		return _tcsncmp(((const process *)B)->UserName, ((const process *)A)->UserName, UNLEN);
}

static int SortProcessByProcessorTime(const void *A, const void *B)
{
	if(SortOrder == ASCENDING)
		return (int)(((const process *)A)->PercentProcessorTime - ((const process *)A)->PercentProcessorTime);
	else
		return (int)(((const process *)B)->PercentProcessorTime - ((const process *)A)->PercentProcessorTime);
}

static int SortProcessByUsedMemory(const void *A, const void *B)
{
	if(SortOrder == ASCENDING)
		return (int)(((const process *)A)->UsedMemory - ((const process *)B)->UsedMemory);
	else
		return (int)(((const process *)B)->UsedMemory - ((const process *)A)->UsedMemory);
}

static int SortProcessByUpTime(const void *A, const void *B)
{
	if(SortOrder == ASCENDING)
		return (int)(((const process *)A)->UpTime - ((const process *)B)->UpTime);
	else
		return (int)(((const process *)B)->UpTime - ((const process *)A)->UpTime);
}

static int SortProcessByPriority(const void *A, const void *B)
{
	if(SortOrder == ASCENDING)
		return (int)(((const process *)A)->BasePriority - ((const process *)B)->BasePriority);
	else
		return (int)(((const process *)B)->BasePriority - ((const process *)A)->BasePriority);
}

static int SortProcessByThreadCount(const void *A, const void *B)
{
	if(SortOrder == ASCENDING)
		return (int)(((const process *)A)->ThreadCount - ((const process *)B)->ThreadCount);
	else
		return (int)(((const process *)B)->ThreadCount - ((const process *)A)->ThreadCount);
}

static int SortProcessByParentPID(const void *A, const void *B)
{
	return (int)(((const process *)A)->ParentPID - ((const process *)B)->ParentPID);
}

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
	if(ProcessSortType != SORT_BY_TREE)
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
	} else {
		qsort(ProcessList, ProcessCount, sizeof(*ProcessList), SortProcessByParentPID);

		for(DWORD i = 0; i < ProcessCount; i++) {
			ProcessList[i].TreeDepth = 0;
		}

		for(DWORD i = 0; i < ProcessCount; i++) {
			DWORD ID = ProcessList[i].ID;
			DWORD Start = 0;
			DWORD End = 0;

			for(DWORD j = i + 1; j < ProcessCount; j++) {
				if(ProcessList[j].ParentPID == ID) {
					Start = j;
					while(++j < ProcessCount && ProcessList[j].ParentPID == ID)
						;
					End = j;
					break;
				}

				if(Start != 0)
					break;
			}

			if(Start != 0) {
				DWORD Size = End - Start;

				/* We want to insert the process group right after this process, thus i+1 */
				DWORD InsLoc = i + 1;

				/* We have to move all processes that come after i by Size items */
				DWORD TmpLoc = InsLoc + Size;

				if(Start == InsLoc)
					/* Process group is already right where we want it to be */
					goto Next;


				/* Make sure process list size has enough left-over to move the data to */
				while(ProcessCount+Size > ProcessListSize) {
					IncreaseProcListSize();
				}

				/* Move all processes */
				for(DWORD j = ProcessCount+Size-1; j >= TmpLoc && j != 0; j--) {
					ProcessList[j] = ProcessList[j-Size];
				}

#ifdef DEBUG_TREESORT
				/* Memory at InsLoc->InsLoc+Size and TmpLoc+Size should be identical now */
				for(DWORD j = InsLoc; j < TmpLoc; j++) {
					assert(ProcessList[j].ID == ProcessList[j+Size].ID);
				}
#endif

				/* Copy process group in */
				for(DWORD j = 0; j < Size; j++) {
					ProcessList[j+InsLoc] = ProcessList[Size+Start+j];
				}

				/* Fill gap at Start -> End */
				for(DWORD j = Start+Size; j < ProcessCount; j++) {
					ProcessList[j] = ProcessList[j+Size];
				}

#ifdef DEBUG_TREESORT
				/* Check no mistakes */
				for(DWORD j = 0; j < Size-1; j++) {
					assert(ProcessList[j+InsLoc].ParentPID == ID);
				}

				/* Check no dups */
				for(DWORD j = 0; j < ProcessCount; j++) {
					for(DWORD k = 0; k < ProcessCount; k++) {
						if(j != k) {
							assert(ProcessList[j].ID != ProcessList[k].ID);
						}
					}
				}
#endif

Next:
				/* Set tree depth */
				for(DWORD j = 0; j < Size; j++) {
					ProcessList[j+InsLoc].TreeDepth = ProcessList[i].TreeDepth+1;
				}
			}
		}
	}
}

static BOOL FilterByUserName = FALSE;
static TCHAR FilterUserName[UNLEN];

static BOOL FilterByPID = FALSE;
static DWORD PidFilterList[1024];
static DWORD PidFilterCount;

static void ReadjustCursor(void)
{
	if(FollowProcess) {
		BOOL Found = FALSE;
		for(DWORD i = 0; i < ProcessCount; i++) {
			if(ProcessList[i].ID == FollowProcessID) {
				SelectedProcessIndex = i;
				if(SelectedProcessIndex <= ProcessIndex - 1 && ProcessIndex != 0) {
					ProcessIndex = SelectedProcessIndex;
				}
				if(SelectedProcessIndex - ProcessIndex >= VisibleProcessCount) {
					ProcessIndex = min(ProcessCount - VisibleProcessCount, SelectedProcessIndex);
				}
				Found = TRUE;
				break;
			}
		}

		if(!Found) {
			/* Process got lost, might as well disable this now */
			FollowProcess = FALSE;
		}
	} else {
		/* After process list update ProcessIndex and SelectedProcessIndex may become out of range */
		ProcessIndex = min(ProcessIndex, ProcessCount - VisibleProcessCount);
		SelectedProcessIndex = min(SelectedProcessIndex, ProcessCount - 1);
	}
}

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
		if(Process.ID == 0)
			continue;
		Process.ThreadCount = Entry.cntThreads;
		Process.BasePriority = Entry.pcPriClassBase;
		Process.ParentPID = Entry.th32ParentProcessID;

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
				if(GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
					PTOKEN_USER TokenUserStruct = xmalloc(ReturnLength);

					if(GetTokenInformation(ProcessTokenHandle, TokenUser, TokenUserStruct, ReturnLength, &ReturnLength)) {
						SID_NAME_USE NameUse;
						DWORD NameLength = UNLEN;
						TCHAR DomainName[MAX_PATH];
						DWORD DomainLength = MAX_PATH;

						LookupAccountSid(NULL, TokenUserStruct->User.Sid, Process.UserName, &NameLength, DomainName, &DomainLength, &NameUse);
					}
					free(TokenUserStruct);
				}
				CloseHandle(ProcessTokenHandle);
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

		if(i >= ProcessListSize) {
			IncreaseProcListSize();
		}
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

	process_times *ProcessTimes = (process_times *)xmalloc(NewProcessCount * sizeof(*ProcessTimes));

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

	RunningProcessCount = 0;

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
				if(Process->PercentProcessorTime >= 0.01) {
					RunningProcessCount++;
				}
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

	memcpy(ProcessList, NewProcessList, NewProcessCount * sizeof *ProcessList);
	ProcessCount = NewProcessCount;
	SortProcessList();
	ReadjustCursor();

	LeaveCriticalSection(&SyncLock);
}

static void DisableCursor(void)
{
	CONSOLE_CURSOR_INFO CursorInfo;
	GetConsoleCursorInfo(ConsoleHandle, &CursorInfo);
	CursorInfo.bVisible = FALSE; /* Note: this has to be set every time console buffer resizes! */
	SetConsoleCursorInfo(ConsoleHandle, &CursorInfo);
}

/*
 * Returns TRUE on screen buffer resize.
 */
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
	ConPrintf(_T("%*c"), Width, ' ');
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
		CharsWritten += ConPrintf(_T("%*s  "), Columns[i].Width, Columns[i].Name);
	}

	for(; CharsWritten < Width; CharsWritten++) {
		ConPutc(' ');
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
		CharsWritten += ConPrintf(_T("%s"), Columns[i].Key);
		SetColor(Config.BGHighlightColor);
		CharsWritten += ConPrintf(_T("%-4s"), Columns[i].Name);
	}

	for(; CharsWritten < Width; CharsWritten++) {
		ConPutc(' ');
	}
}

#define BAR_WIDTH 25

static int DrawPercentageBar(TCHAR *Name, double Percentage, WORD Color)
{
	int CharsWritten = 0;

	SetColor(Config.FGHighlightColor);
	CharsWritten += ConPrintf(_T("  %s"), Name);
	SetColor(Config.FGColor);
	ConPutc('[');
	CharsWritten++;

	int Bars = (int)((double)BAR_WIDTH * Percentage);
	SetColor(Color);
	for(int i = 0; i < Bars; i++) {
		ConPutc('|');
	}
	CharsWritten+= Bars;
	SetColor(Config.FGColor);
	for(int i = 0; i < BAR_WIDTH - Bars; i++) {
		ConPutc(' ');
	}
	CharsWritten += BAR_WIDTH - Bars;
	SetColor(Config.BGColor);
	CharsWritten += ConPrintf(_T("%04.1f%%"), 100.0 * Percentage);
	SetColor(Config.FGColor);
	ConPutc(']');
	CharsWritten++;
	return CharsWritten;
}

static void RestoreConsole(void)
{
	SetConsoleActiveScreenBuffer(OldConsoleHandle);
}

static void WriteProcessInfo(const process *Process, BOOL Highlighted)
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

	int CharsWritten = 0;
	TCHAR UpTimeStr[TIME_STR_SIZE];
	FormatTimeString(UpTimeStr, Process->UpTime);

	if(ProcessSortType == SORT_BY_TREE) {
		TCHAR OffsetStr[256] = { 0 };
		if(Process->TreeDepth > 0) {
			for(DWORD i = 0; i < Process->TreeDepth-1; i++) {
				_tcscat_s(OffsetStr, _countof(OffsetStr), _T("|  "));
			}
			_tcscat_s(OffsetStr, _countof(OffsetStr), _T("`- "));
		}

		CharsWritten = ConPrintf(_T("\n%6u  %9s  %3u  %04.1f%%  % 6.1f MB  %4u  %s"),
				Process->ID,
				Process->UserName,
				Process->BasePriority,
				Process->PercentProcessorTime,
				(double)Process->UsedMemory / 1000000.0,
				Process->ThreadCount,
				UpTimeStr
				);
		WORD Color = CurrentColor;

		if(!Highlighted) {
			SetColor(Config.FGHighlightColor);
		}

		CharsWritten += ConPrintf(_T("  %s"), OffsetStr);
		SetColor(Color);

		CharsWritten += ConPrintf(_T("%s"), Process->ExeName);
	} else {
		CharsWritten = ConPrintf(_T("\n%6u  %9s  %3u  %04.1f%%  % 6.1f MB  %4u  %s  %s"),
				Process->ID,
				Process->UserName,
				Process->BasePriority,
				Process->PercentProcessorTime,
				(double)Process->UsedMemory / 1000000.0,
				Process->ThreadCount,
				UpTimeStr,
				Process->ExeName
				);
	}


	ConPrintf(_T("%*c"), Width-CharsWritten+1, ' ');
}

static ULONGLONG KeyPressStart = 0;
static ULONGLONG LastKeyPress = 0;
static BOOL KeyPress = FALSE;
static DWORD OldSelectedProcessIndex = 0;
static BOOL RedrawAtCursor = FALSE;

typedef enum scroll_type {
	SCROLL_UP,
	SCROLL_DOWN,
	SCROLL_PAGE_UP,
	SCROLL_PAGE_DOWN,
} scroll_type;

static void DoScroll(scroll_type ScrollType, BOOL *Redraw)
{
	ULONGLONG Now = GetTickCount64();

	if(!KeyPress) {
		KeyPress = TRUE;
		KeyPressStart = Now;
	}

	/*
	 * In order to enforce a one-time delay when scrolling, scroll only
	 * if this is the instant we pressed the key or at any time 500ms thereafter.
	 */
	if(Now == KeyPressStart || Now - KeyPressStart > 500) {
		BOOL Scrolled = FALSE;

		OldSelectedProcessIndex = SelectedProcessIndex;
		switch(ScrollType) {
		case SCROLL_UP:
			if(SelectedProcessIndex != 0) {
				Scrolled = TRUE;
				OldSelectedProcessIndex = SelectedProcessIndex;
				SelectedProcessIndex--;
				if(SelectedProcessIndex <= ProcessIndex - 1 && ProcessIndex != 0) {
					ProcessIndex--;
					*Redraw = TRUE;
				}
			}
			break;
		case SCROLL_DOWN:
			if(SelectedProcessIndex != ProcessCount-1) {
				Scrolled = TRUE;
				OldSelectedProcessIndex = SelectedProcessIndex;
				SelectedProcessIndex++;
				if(SelectedProcessIndex - ProcessIndex >= VisibleProcessCount) {
					if(ProcessIndex <= ProcessCount - ProcessWindowHeight + 1) {
						ProcessIndex++;
						*Redraw = TRUE;
					}
				}
			}
			break;
		case SCROLL_PAGE_UP:
			if(SelectedProcessIndex != 0) {
				Scrolled = TRUE;
				OldSelectedProcessIndex = SelectedProcessIndex;
				if(SelectedProcessIndex > VisibleProcessCount)
					SelectedProcessIndex -= VisibleProcessCount;
				else
					SelectedProcessIndex = 0;
				if(SelectedProcessIndex <= ProcessIndex - 1 && ProcessIndex != 0) {
					ProcessIndex = max(0, SelectedProcessIndex);
					*Redraw = TRUE;
				}
			}
			break;
		case SCROLL_PAGE_DOWN:
			if(SelectedProcessIndex != ProcessCount-1) {
				Scrolled = TRUE;
				OldSelectedProcessIndex = SelectedProcessIndex;
				SelectedProcessIndex += VisibleProcessCount;
				if(SelectedProcessIndex > ProcessCount - 1)
					SelectedProcessIndex = ProcessCount - 1;
				if(SelectedProcessIndex - ProcessIndex >= VisibleProcessCount) {
					ProcessIndex = min(ProcessCount - VisibleProcessCount, SelectedProcessIndex);
					*Redraw = TRUE;
				}
			}
			break;
		}

		if(Scrolled) {
			RedrawAtCursor = TRUE;
			LastKeyPress = GetTickCount64();
		}
	}
}

static void PrintVersion(void)
{
	ConPrintf(_T("NTop " NTOP_VER " - (C) 2017 Gian Sass\n"));
	ConPrintf(_T("Compiled on " __DATE__ " " __TIME__ "\n"));

#ifdef _UNICODE
	TCHAR UnicodeEnabled[] = _T("Yes");
#else
	TCHAR UnicodeEnabled[] = _T("No");
#endif
	ConPrintf(_T("Unicode version: %s\n\n"), UnicodeEnabled);
}

typedef struct help_entry {
	TCHAR *Key;
	TCHAR *Explanation;
} help_entry;

static void PrintHelpEntries(const TCHAR *Name, int Count, const help_entry *Entries)
{
	SetColor(0xE);
	ConPrintf(_T("%s\n"), Name);

	for(int i = 0; i < Count; i++) {
		help_entry Entry = Entries[i];
		SetColor(FOREGROUND_CYAN);
		ConPrintf(_T("\t%s"), Entry.Key);
		SetColor(FOREGROUND_WHITE);
		ConPrintf(_T("\t%s\n"), Entry.Explanation);
	}

	ConPutc('\n');
}

static void PrintHelp(const TCHAR *argv0)
{
	PrintVersion();

	SetColor(0xE);
	ConPrintf(_T("USAGE\n"));

	SetColor(FOREGROUND_WHITE);
	ConPrintf(_T("\t%s [OPTIONS]\n\n"), argv0);

	static help_entry Options[] = {
		{ _T("-C"), _T("Use a monochrome color scheme.") },
		{ _T("-h"), _T("Display this help info.") },
		{ _T("-p PID,PID...\n"), _T("\tShow only the given PIDs.") },
		{ _T("-s COLUMN\n"), _T("\tSort by this column.") },
		{ _T("-u USERNAME\n"), _T("\tDisplay only processes of this user.") },
		{ _T("-v"), _T("Print version.") },
	};
	PrintHelpEntries(_T("OPTIONS"), _countof(Options), Options);

	static help_entry InteractiveCommands[] = {
		{ _T("Up and Down Arrows, PgUp and PgDown\n"), _T("\tScroll through the process list.") },
		{ _T("CTRL + Left and Right Arrows\n"), _T("\tChange the process sort column.") },
		{ _T("g"), _T("Go to the top of the process list.") },
		{ _T("G"), _T("Go to the bottom of the process list.") },
		{ _T("Space"), _T("Tag or untag selected process.") },
		{ _T("U"), _T("Untag all selected processes.") },
		{ _T("k"), _T("Kill all tagged processes.") },
		{ _T("I"), _T("Invert the sort order.") },
		{ _T("F"), _T("Follow process: if the sort order causes the currently selected\n"
			      "\t\tprocess to move in the list, make the selection bar follow it.\n"
			      "\t\tMoving the cursor manually automatically disables this feature."
				) },
	};
	PrintHelpEntries(_T("INTERACTIVE COMMANDS"), _countof(InteractiveCommands), InteractiveCommands);

	static help_entry ViCommands[] = {
		{ _T(":exec CMD\n"), _T("\tExecutes the given Windows command.") },
		{ _T(":kill PID(s)\n"), _T("\tKill all given processes.") },
		{ _T(":q, :quit\n"), _T("\tQuit NTop.") },
		{ _T(":sort COLUMN\n"), _T("\tSort the process list after the given column.") },
		{ _T(":tree"), _T("View process tree.") },
	};
	PrintHelpEntries(_T("VI COMMANDS"), _countof(ViCommands), ViCommands);
}

int GetProcessSortTypeFromName(const TCHAR *Name, process_sort_type *Dest)
{
	if(!lstrcmpi(Name, _T("ID"))) {
		*Dest = SORT_BY_ID;
		return TRUE;
	} else if(!lstrcmpi(Name, _T("USER"))) {
		*Dest = SORT_BY_USER_NAME;
		return TRUE;
	} else if(!lstrcmpi(Name, _T("PRI"))) {
		*Dest = SORT_BY_PRIORITY;
		return TRUE;
	} else if(!lstrcmpi(Name, _T("CPU%"))) {
		*Dest = SORT_BY_PROCESSOR_TIME;
		return TRUE;
	} else if(!lstrcmpi(Name, _T("MEM"))) {
		*Dest = SORT_BY_USED_MEMORY;
		return TRUE;
	} else if(!lstrcmpi(Name, _T("THRD"))) {
		*Dest = SORT_BY_THREAD_COUNT;
		return TRUE;
	} else if(!lstrcmpi(Name, _T("TIME"))) {
		*Dest = SORT_BY_UPTIME;
		return TRUE;
	} else if(!lstrcmpi(Name, _T("EXE"))) {
		*Dest = SORT_BY_EXE;
		return TRUE;
	}

	return FALSE;
}

void ChangeProcessSortType(process_sort_type NewProcessSortType)
{
	EnterCriticalSection(&SyncLock);
	ProcessSortType = NewProcessSortType;
	SortProcessList();
	ReadjustCursor();
	LeaveCriticalSection(&SyncLock);
}

static BOOL ViErrorActive(void)
{
	return _tcslen(ViErrorMessage) > 0;
}

static void WriteVi(void)
{
	SetConCursorPos(0, (SHORT)Height-2);

	if(ViErrorActive()) {
		/* TODO: This color should be put into the config */
		SetColor(Config.ErrorColor);
		int CharsWritten = ConPrintf(_T("\n%s"), ViErrorMessage);

		for (; CharsWritten < Width + 1; CharsWritten++) {
			ConPutc(' ');
		}
	} else {
		SetColor(FOREGROUND_WHITE);
		ConPutc('\n');
		WriteBlankLine();
	}
}

static void HideViErrorMessage(void)
{
	ViErrorMessage[0] = _T('\0');
	WriteVi();
}

static void ClearViMessage(void)
{
	if(ViErrorActive()) {
		SetConsoleMode(ConsoleHandle, ENABLE_PROCESSED_INPUT | DISABLE_NEWLINE_AUTO_RETURN);
		HideViErrorMessage();
		SetConsoleMode(ConsoleHandle, ENABLE_PROCESSED_INPUT | ENABLE_WRAP_AT_EOL_OUTPUT);
	}
}

static void KillTaggedProcesses(void)
{
	for(DWORD i = 0; i < TaggedProcessListCount; ++i) {
		HANDLE Handle = OpenProcess(PROCESS_TERMINATE, FALSE, TaggedProcessList[i]);
		if(Handle) {
			TerminateProcess(Handle, 9);
			CloseHandle(Handle);
		}
	}

	TaggedProcessListCount = 0;
}

static ULONGLONG CaretTicks;
static BOOLEAN CaretState;

static void ResetCaret(void)
{
	CaretTicks = GetTickCount64();
	CaretState = FALSE;
}

static BOOL CTRLState;

static void ProcessInput(BOOL *Redraw)
{
	DWORD NumEvents, Num;

	GetNumberOfConsoleInputEvents(GetStdHandle(STD_INPUT_HANDLE), &NumEvents);

	if(NumEvents == 0) {
		return;
	}

	INPUT_RECORD *Records = xmalloc(NumEvents * sizeof(*Records));
	ReadConsoleInput(GetStdHandle(STD_INPUT_HANDLE), Records, NumEvents, &Num);

	CTRLState = FALSE;

	for(DWORD i = 0; i < Num; i++) {
		INPUT_RECORD InputRecord = Records[i];
		if (InputRecord.EventType == KEY_EVENT) {
			if (InputRecord.Event.KeyEvent.bKeyDown) {
				if(!InInputMode) {
					switch(InputRecord.Event.KeyEvent.wVirtualKeyCode) {
					case VK_UP:
						DoScroll(SCROLL_UP, Redraw);
						break;
					case VK_DOWN:
						DoScroll(SCROLL_DOWN, Redraw);
						break;
					case VK_PRIOR:
						DoScroll(SCROLL_PAGE_UP, Redraw);
						break;
					case VK_NEXT:
						DoScroll(SCROLL_PAGE_DOWN, Redraw);
						break;
					case VK_SPACE:
						ToggleTaggedProcess(ProcessList[SelectedProcessIndex].ID);
						RedrawAtCursor = TRUE;
						OldSelectedProcessIndex = SelectedProcessIndex;
						DoScroll(SCROLL_DOWN, Redraw);
						break;
					case VK_CONTROL:
						CTRLState = TRUE;
						break;
					case VK_LEFT:
						if(ProcessSortType == 0) {
							ProcessSortType = SORT_TYPE_MAX - 1;
						} else {
							--ProcessSortType;
						}
						ChangeProcessSortType(ProcessSortType);
						*Redraw = TRUE;
						break;
					case VK_RIGHT:
						if(ProcessSortType == SORT_TYPE_MAX - 1) {
							ProcessSortType = 0;
						} else {
							++ProcessSortType;
						}
						ChangeProcessSortType(ProcessSortType);
						*Redraw = TRUE;
						break;
					default:
						switch(InputRecord.Event.KeyEvent.uChar.AsciiChar) {
						case 'k':
							KillTaggedProcesses();
							break;
						case 'g':
							ProcessIndex = SelectedProcessIndex = 0;
							ClearViMessage();
							*Redraw = TRUE;
							break;
						case 'G':
							SelectedProcessIndex = ProcessCount - 1;
							ProcessIndex = SelectedProcessIndex - VisibleProcessCount + 1; 
							ClearViMessage();
							*Redraw = TRUE;
							break;
						case 'I':
							EnterCriticalSection(&SyncLock);
							if(SortOrder == ASCENDING)
								SortOrder = DESCENDING;
							else
								SortOrder = ASCENDING;
							SortProcessList();
							ReadjustCursor();
							LeaveCriticalSection(&SyncLock);
							*Redraw = TRUE;
							break;
						case 'F':
							FollowProcess = TRUE;
							FollowProcessID = ProcessList[SelectedProcessIndex].ID;
							break;
						case 'U':
							TaggedProcessListCount = 0;
							*Redraw = TRUE;
							break;
						case ':':
							ViEnableInput();
							ResetCaret();
							*Redraw = TRUE;
							break;
						}
						break;
					}
				} else {
					if(ViHandleInputKey(&InputRecord.Event.KeyEvent)) {
						*Redraw = TRUE;
					}
				}
			}
		}
	}

	free(Records);
}

int _tmain(int argc, TCHAR *argv[])
{
	BOOL Monochrome = FALSE;

	/* Only set this temporarily for command-line processing */
	ConsoleHandle = GetStdHandle(STD_OUTPUT_HANDLE);

	for(int i = 1; i < argc; i++) {
		if(argv[i][0] == _T('-') && _tcslen(argv[i]) == 2) {
			switch(argv[i][1]) {
			case 'C':
				Monochrome = TRUE;
				break;
			case 'h':
				PrintHelp(argv[0]);
				return EXIT_SUCCESS;
			case 's':
				if(++i < argc) {
					if(!GetProcessSortTypeFromName(argv[i], &ProcessSortType)) {
						ConPrintf(_T("Unknown column: '%s'\n"), argv[i]);
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
				ConPrintf(_T("Unknown option: '%c'"), argv[i][1]);
				return EXIT_FAILURE;
			}
		}
	}

	InitializeCriticalSection(&SyncLock);
	SetConsoleCtrlHandler(CtrlHandler, TRUE);
	OldConsoleHandle = ConsoleHandle;

	ConsoleHandle = CreateConsoleScreenBuffer(GENERIC_READ|GENERIC_WRITE,
						   FILE_SHARE_READ|FILE_SHARE_WRITE,
						  NULL,
						  CONSOLE_TEXTMODE_BUFFER,
						  NULL);

	if(ConsoleHandle == INVALID_HANDLE_VALUE) {
		Die(_T("Could not create console screen buffer: %ld\n"), GetLastError());
	}

	if(!SetConsoleActiveScreenBuffer(ConsoleHandle)) {
		Die(_T("Could not set active console screen buffer: %ld\n"), GetLastError());
	}

	SetConsoleMode(ConsoleHandle, ENABLE_PROCESSED_INPUT|ENABLE_WRAP_AT_EOL_OUTPUT);

	atexit(RestoreConsole);

	if(Monochrome) {
		Config = MonochromeConfig;
	} else {
		ReadConfigFile();
	}

	ProcessList = xmalloc(ProcessListSize * sizeof *ProcessList);
	NewProcessList = xmalloc(ProcessListSize * sizeof *ProcessList);
	TaggedProcessList = xmalloc(TaggedProcessListSize * sizeof *TaggedProcessList);

	ViInit();

	PollConsoleInfo();
	PollInitialSystemInfo();
	PollSystemInfo();
	PollProcessList();

	TCHAR MenuBar[256] = { 0 };
	wsprintf(MenuBar, _T("NTop on %s"), ComputerName);

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
			ConPutc(' ');
		}

		ConPrintf(_T("%s"), MenuBar);

		for(int i = 0; i < Width - MenuBarOffsetX - (int)_tcslen(MenuBar); i++) {
			ConPutc(' ');
		}

		SetColor(Config.FGColor);
		WriteBlankLine();

		/* CPU */
		int CharsWritten = 0;

		CharsWritten += DrawPercentageBar(_T("CPU"), CPUUsage, Config.CPUBarColor);

		int CPUInfoChars = 0;

		TCHAR CPUNameBuf[] = _T("  Name: ");
		CPUInfoChars += _tcslen(CPUNameBuf);

		TCHAR CPUInfoBuf[256];
		CPUInfoChars += wsprintf(CPUInfoBuf, _T("%s (%u Cores)"), CPUName, CPUCoreCount);

		int TaskInfoChars = 0;

		TCHAR TasksNameBuf[] = _T("  Tasks: ");
		TaskInfoChars += _tcsclen(TasksNameBuf);

		TCHAR TasksInfoBuf[256];
		TaskInfoChars += wsprintf(TasksInfoBuf, _T("%u total, %u running"), ProcessCount, RunningProcessCount);

		if(CharsWritten + CPUInfoChars + TaskInfoChars < Width) {
			SetColor(Config.FGHighlightColor);
			ConPrintf(_T("%s"), CPUNameBuf);
			SetColor(Config.FGColor);
			ConPrintf(_T("%s"), CPUInfoBuf);
			CharsWritten += CPUInfoChars;
		}

		SetColor(Config.FGHighlightColor);
		ConPrintf(_T("%s"), TasksNameBuf);
		SetColor(Config.FGColor);
		ConPrintf(_T("%s"), TasksInfoBuf);
		CharsWritten += TaskInfoChars;

		for(; CharsWritten < Width; CharsWritten++) {
			ConPutc(' ');
		}

		/* Memory */
		CharsWritten = DrawPercentageBar(_T("Mem"), UsedMemoryPerc, Config.MemoryBarColor);

		SetColor(Config.FGHighlightColor);
		CharsWritten += ConPrintf(_T("  Size: "));
		SetColor(Config.FGColor);
		CharsWritten += ConPrintf(_T("%d GB"), (int)TotalMemory/1000);

		for(; CharsWritten < Width; CharsWritten++) {
			ConPutc(' ');
		}

		CharsWritten = DrawPercentageBar(_T("Pge"), UsedPageMemoryPerc, Config.PageMemoryBarColor);

		SetColor(Config.FGHighlightColor);
		CharsWritten += ConPrintf(_T("  Uptime: "));

		TCHAR Buffer[TIME_STR_SIZE];
		FormatTimeString(Buffer, UpTime);
		SetColor(Config.FGColor);
		CharsWritten +=	ConPrintf(_T("%s"), Buffer);
		SetColor(Config.FGColor);

		for(; CharsWritten < Width; CharsWritten++) {
			ConPutc(' ');
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
				SetConCursorPos(0, (SHORT)(i + ProcessWindowPosY));
				WriteProcessInfo(Process, PID == SelectedProcessIndex);
				Count++;
			}
		}
		LeaveCriticalSection(&SyncLock);

		SetColor(0);
		for(DWORD i = Count; i < VisibleProcessCount - 1; i++) {
			WriteBlankLine();
		}

		SetConCursorPos(0, (SHORT)Height-2);

		SetColor(FOREGROUND_WHITE);

		/* Disable auto newline here. This allows us to fill the last row entirely
		 * without scrolling the screen buffer accidentally which is really annoying. */
		SetConsoleMode(ConsoleHandle, ENABLE_PROCESSED_INPUT|DISABLE_NEWLINE_AUTO_RETURN);
		if (InInputMode) {
			CharsWritten = ConPrintf(_T("\n%s"), CurrentInputStr);
			if(CaretState) {
				ConPutc((char)219);
				++CharsWritten;
			}

			for(; CharsWritten < Width; CharsWritten++) {
				ConPutc(' ');
			}
		} else if(_tcslen(ViErrorMessage) > 0) {
			WriteVi();
		} else {
			ConPutc('\n');
			WriteBlankLine();
		}
		SetConsoleMode(ConsoleHandle, ENABLE_PROCESSED_INPUT|ENABLE_WRAP_AT_EOL_OUTPUT);

#ifdef _DEBUG
		ULONGLONG T2 = GetTickCount64();

		ULONG Diff = (ULONG)(T2 - T1);

		TCHAR DebugBuffer[256];
		wsprintf(DebugBuffer, _T("Drawing took: %lu ms\n"), Diff);
		OutputDebugString(DebugBuffer);
#endif

		process_sort_type NewProcessSortType = ProcessSortType;
		BOOL SortOrderChanged = FALSE;
		ULONGLONG StartTicks = GetTickCount64();

		/*
		 * Input loop. Breaks after REDRAW_INTERVAL ms or if forced.
		 */

		while(1) {
			/*
			 * Whether or not to redraw the selected process
			 * (and the process at OldSelectedProcessIndex).
			 *
			 * This allows us not having to redraw the whole buffer
			 * at each scroll.
			 */
			RedrawAtCursor = FALSE;
			BOOL Redraw = FALSE;

			ProcessInput(&Redraw);

			if(Redraw) {
				break;
			}

			if(RedrawAtCursor) {
				SetConCursorPos(0, (SHORT)(ProcessWindowPosY + SelectedProcessIndex - ProcessIndex));
				WriteProcessInfo(&ProcessList[SelectedProcessIndex], TRUE);

				if(OldSelectedProcessIndex != SelectedProcessIndex) {
					SetConCursorPos(0, (SHORT)(ProcessWindowPosY + OldSelectedProcessIndex - ProcessIndex));
					WriteProcessInfo(&ProcessList[OldSelectedProcessIndex], FALSE);
				}
			}

			if(PollConsoleInfo()) {
				break;
			}

			ULONGLONG Now = GetTickCount64();

			if(Now - StartTicks >= REDRAW_INTERVAL) {
				PollSystemInfo();
				StartTicks = Now;
				break;
			}

			if(Now - CaretTicks >= CARET_INTERVAL) {
				CaretTicks = Now;
				CaretState = !CaretState;
				break;
			}

			Sleep(INPUT_LOOP_DELAY);
		}
	}

	WaitForSingleObject(ProcessListThread, INFINITE);
	return EXIT_SUCCESS;
}
