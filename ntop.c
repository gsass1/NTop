/* 
 * NTop - an htop clone for Windows
 * Copyright (c) 2019 Gian Sass
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
#include <math.h>
#include "ntop.h"
#include "util.h"
#include "vi.h"

#ifndef NTOP_VER
#define NTOP_VER "dev"
#endif

#define STRINGIZE(x) #x
#define STRINGIZE_VALUE_OF(x) STRINGIZE(x)

#define SCROLL_INTERVAL 20ULL
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
	WriteConsole(ConsoleHandle, Buffer, CharsWritten, &Dummy, 0);

	return CharsWritten;
}

static void ConPutc(char c)
{
	DWORD Dummy;
	WriteConsole(ConsoleHandle, &c, 1, &Dummy, 0);
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
	ULONGLONG RedrawInterval;
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
	1000
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
	1000
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
	char *Value = strtok_s(0, Delimeter, &Context);
	if(!Value)
		return;

	WORD Num = (WORD)strtol(Value, 0, 0);

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
	} else if(_strcmpi(Key, "RedrawInterval") == 0) {
		Config.RedrawInterval = (ULONGLONG)Num;
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
		if(!fgets(Offset + Buffer, (int)(BufferSize - Offset), File)) {
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
static WORD ColorOverride;

static void SetColor(WORD Color)
{
	Color |= ColorOverride;
	CurrentColor = Color;
	SetConsoleTextAttribute(ConsoleHandle, Color);
}

static void SetConCursorPos(SHORT X, SHORT Y)
{
	COORD Coord;
	Coord.X = X;
	Coord.Y = Y;
	SetConsoleCursorPosition(ConsoleHandle, Coord);
}

typedef struct process {
	HANDLE Handle;
	DWORD ID;
	TCHAR UserName[UNLEN];
	DWORD BasePriority;
	double PercentProcessorTime;
	unsigned __int64 UsedMemory;
	DWORD ThreadCount;
	ULONGLONG UpTime;
	TCHAR ExeName[MAX_PATH];
	DWORD ParentPID;
	ULONGLONG DiskOperationsPrev;
	ULONGLONG DiskOperations;
	DWORD DiskUsage;
	DWORD TreeDepth;

	struct process *Next;
	struct process *Parent;
	struct process *FirstChild;
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

#define SORT_PROCESS_BY_INTEGER(Attribute)							\
static int SortProcessBy##Attribute(const void *A, const void *B)				\
{												\
	int Compare = (int)(((const process *)A)->Attribute - ((const process *)B)->Attribute); \
	return (SortOrder == ASCENDING) ? Compare : -Compare;					\
}												\

#define SORT_PROCESS_BY_UINT64(Attribute)							\
static int SortProcessBy##Attribute(const void *A, const void *B)				\
{												\
	__int64 Compare = (__int64)(((const process *)A)->Attribute - ((const process *)B)->Attribute); \
	int Result = (Compare > 1) ? 1 : -1; 							\
	return (SortOrder == ASCENDING) ? Result : -Result;					\
}												\

#define SORT_PROCESS_BY_DOUBLE(Attribute)                                               \
static int SortProcessBy##Attribute(const void *A, const void *B)                       \
{												                                        \
	double Diff = (((const process *)A)->Attribute - ((const process *)B)->Attribute);  \
	if (Diff > 0.0) return (SortOrder == ASCENDING) ? 1 : -1;                      		\
	if (Diff < 0.0) return (SortOrder == ASCENDING) ? -1 : 1;                           \
	return 0;                                                                           \
}												                                        \

#define SORT_PROCESS_BY_STRING(Attribute, MaxLength)								\
static int SortProcessBy##Attribute(const void *A, const void *B)						\
{														\
	int Compare = _tcsncicmp(((const process *)A)->Attribute, ((const process *)B)->Attribute, MaxLength);	\
	return (SortOrder == ASCENDING) ? Compare : -Compare;							\
}														\

SORT_PROCESS_BY_INTEGER(ID);
SORT_PROCESS_BY_DOUBLE(PercentProcessorTime);
SORT_PROCESS_BY_UINT64(UsedMemory);
SORT_PROCESS_BY_INTEGER(UpTime);
SORT_PROCESS_BY_INTEGER(BasePriority);
SORT_PROCESS_BY_INTEGER(ThreadCount);
SORT_PROCESS_BY_INTEGER(ParentPID);
SORT_PROCESS_BY_INTEGER(DiskUsage);
SORT_PROCESS_BY_STRING(ExeName, MAX_PATH);
SORT_PROCESS_BY_STRING(UserName, UNLEN);

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

void AddChildProcess(process *ParentProcess, process *Process)
{
	if(ParentProcess == Process) return;

	if (ParentProcess->FirstChild == NULL) {
		ParentProcess->FirstChild = Process;
	} else {
		process *ChildProcess = ParentProcess->FirstChild;
		while (ChildProcess->Next != NULL) {
			ChildProcess = ChildProcess->Next;
			if(ChildProcess == Process) return;
		}
		ChildProcess->Next = Process;
	}

	Process->Parent = ParentProcess;
}

/* The "root" process which does not actually exist, acts as process tree root */
static process RootProcess;

/* Finds and assigns parent and child processes */
void FindParentChildProcesses(void)
{
	memset(&RootProcess, 0, sizeof(RootProcess));

	/* Find and assign parent and child processes that have ParentPID=0 */
	for (DWORD i = 0; i < ProcessCount; i++)
	{
		ProcessList[i].TreeDepth = 0;
		ProcessList[i].Next = 0;
		ProcessList[i].Parent = 0;
		ProcessList[i].FirstChild = 0;

		// if (ProcessList[i].ParentPID == 0)
		// {
		// 	AddChildProcess(&RootProcess, &ProcessList[i]);
		// }
	}

	/* Find and assign parent and child processes */
	for (DWORD i = 0; i < ProcessCount; i++)
	{
		for (DWORD j = 0; j < ProcessCount; j++)
		{
			if (i == j)
				continue;

			if (ProcessList[j].ParentPID == ProcessList[i].ID && ProcessList[j].ParentPID != 0 && ProcessList[i].ID != ProcessList[j].ID && ProcessList[j].Next == NULL)
			{
				/* i is a parent of j */
				AddChildProcess(&ProcessList[i], &ProcessList[j]);
				ProcessList[j].TreeDepth = ProcessList[i].TreeDepth + 1;
			}
		}
	}

	/* Add processes with PIDs that couldn't be found to RootProcess or else
		 * they won't be shown */
	for (DWORD i = 0; i < ProcessCount; i++)
	{
		process *Process = &ProcessList[i];

		if (Process->Parent == NULL)
		{
			AddChildProcess(&RootProcess, Process);
		}
	}
}

void ProcessTreeToList(process *Process, process *Dest, int *Index)
{
	process *ProcessNode = Process->FirstChild;
	if(ProcessNode == NULL) return;

	while(ProcessNode != NULL) {
		Dest[*Index] = *ProcessNode;
		*Index = *Index + 1;
		ProcessTreeToList(ProcessNode, Dest, Index);
		ProcessNode = ProcessNode->Next;
	}
}

static void SortProcessList(void)
{
	if(ProcessSortType != SORT_BY_TREE)
	{
		process_sort_fn_t SortFn = 0;

		switch(ProcessSortType) {
		case SORT_BY_ID:
			SortFn = SortProcessByID;
			break;
		case SORT_BY_PROCESS:
			SortFn = SortProcessByExeName;
			break;
		case SORT_BY_USER_NAME:
			SortFn = SortProcessByUserName;
			break;
		case SORT_BY_PROCESSOR_TIME:
			SortFn = SortProcessByPercentProcessorTime;
			break;
		case SORT_BY_USED_MEMORY:
			SortFn = SortProcessByUsedMemory;
			break;
		case SORT_BY_UPTIME:
			SortFn = SortProcessByUpTime;
			break;
		case SORT_BY_PRIORITY:
			SortFn = SortProcessByBasePriority;
			break;
		case SORT_BY_THREAD_COUNT:
			SortFn = SortProcessByThreadCount;
			break;
		case SORT_BY_DISK_USAGE:
			SortFn = SortProcessByDiskUsage;
			break;
		}

		if(SortFn) {
			qsort(ProcessList, ProcessCount, sizeof(*ProcessList), SortFn);
		}

		FindParentChildProcesses();
	} else {
		SortOrder = ASCENDING;
		qsort(ProcessList, ProcessCount, sizeof(*ProcessList), SortProcessByParentPID);

		FindParentChildProcesses();

		process *TreeProcessList = xmalloc(ProcessCount * sizeof(*ProcessList));
		int Index = 0;
		ProcessTreeToList(&RootProcess, TreeProcessList, &Index);

		ProcessCount = Index;
		memcpy(ProcessList, TreeProcessList, ProcessCount * sizeof(*ProcessList));

		free(TreeProcessList);
	}
}

static BOOL FilterByUserName = FALSE;
static TCHAR FilterUserName[UNLEN];

static BOOL FilterByPID = FALSE;
static DWORD PidFilterList[1024];
static DWORD PidFilterCount;

static void SelectProcess(DWORD Index)
{
	SelectedProcessIndex = Index;
	if(SelectedProcessIndex <= ProcessIndex - 1 && ProcessIndex != 0) {
		ProcessIndex = SelectedProcessIndex;
	}
	if(SelectedProcessIndex - ProcessIndex >= VisibleProcessCount) {
		ProcessIndex = min(ProcessCount - VisibleProcessCount, SelectedProcessIndex);
	}
}

static void ReadjustCursor(void)
{
	if(FollowProcess) {
		BOOL Found = FALSE;
		for(DWORD i = 0; i < ProcessCount; i++) {
			if(ProcessList[i].ID == FollowProcessID) {
				SelectProcess(i);
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

static TCHAR SearchPattern[256];
static BOOLEAN SearchActive;

static void SearchNext(void);

void StartSearch(const TCHAR *Pattern)
{
	_tcsncpy_s(SearchPattern, 256, Pattern, 256);
	SearchActive = TRUE;
	SearchNext();
}

static BOOLEAN SearchMatchesProcess(const process *Process)
{
	return _tcsstr(Process->ExeName, SearchPattern) != 0;
}

static void SearchNext(void)
{
	if(!SearchActive) return;

	for(DWORD i = SelectedProcessIndex + 1; i < ProcessCount; ++i) {
		const process *Process = &ProcessList[i];
		if(SearchMatchesProcess(Process)) {
			SelectProcess(i);
			return;
		}
	}

	SetViMessage(VI_NOTICE, _T("search hit BOTTOM, continuing at TOP"));

	for(DWORD i = 0; i <= SelectedProcessIndex; ++i) {
		const process *Process = &ProcessList[i];
		if(SearchMatchesProcess(Process)) {
			SelectProcess(i);
			return;
		}
	}

	SetViMessage(VI_ERROR, _T("Pattern not found: %s"), SearchPattern);
}

static void SearchPrevious(void)
{
	if(!SearchActive) return;

	for(DWORD i = SelectedProcessIndex; i > 0; --i) {
		const process *Process = &ProcessList[i - 1];
		if(SearchMatchesProcess(Process)) {
			SelectProcess(i - 1);
			return;
		}
	}

	SetViMessage(VI_NOTICE, _T("search hit TOP, continuing at BOTTOM"));

	for(DWORD i = ProcessCount; i > SelectedProcessIndex; --i) {
		const process *Process = &ProcessList[i - 1];
		if(SearchMatchesProcess(Process)) {
			SelectProcess(i - 1);
			return;
		}
	}

	SetViMessage(VI_ERROR, _T("Pattern not found: %s"), SearchPattern);
}

static void PollProcessList(DWORD UpdateTime)
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
				Process.UsedMemory = (unsigned __int64)ProcMemCounters.WorkingSetSize;
			}

			HANDLE ProcessTokenHandle;
			if(OpenProcessToken(Process.Handle, TOKEN_READ, &ProcessTokenHandle)) {
				DWORD ReturnLength;

				GetTokenInformation(ProcessTokenHandle, TokenUser, 0, 0, &ReturnLength);
				if(GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
					PTOKEN_USER TokenUserStruct = xmalloc(ReturnLength);

					if(GetTokenInformation(ProcessTokenHandle, TokenUser, TokenUserStruct, ReturnLength, &ReturnLength)) {
						SID_NAME_USE NameUse;
						DWORD NameLength = UNLEN;
						TCHAR DomainName[MAX_PATH];
						DWORD DomainLength = MAX_PATH;

						LookupAccountSid(0, TokenUserStruct->User.Sid, Process.UserName, &NameLength, DomainName, &DomainLength, &NameUse);

						// FIXME: we cut user name here for display purposes because something like %9.9s does not work with MS's vsprintf function?
						Process.UserName[9] = 0;
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
			for(DWORD PidIndex = 0; PidIndex < PidFilterCount; PidIndex++) {
				if(PidFilterList[PidIndex] == Process.ID) {
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

	for(DWORD ProcIndex = 0; ProcIndex < NewProcessCount; ProcIndex++) {
		process *Process = &NewProcessList[ProcIndex];
		process_times *ProcessTime = &ProcessTimes[ProcIndex];
		if(Process->Handle) {
			GetProcessTimes(Process->Handle, &ProcessTime->CreationTime, &ProcessTime->ExitTime, &ProcessTime->KernelTime, &ProcessTime->UserTime);

			IO_COUNTERS IoCounters;
			if(GetProcessIoCounters(Process->Handle, &IoCounters)) {
				Process->DiskOperationsPrev = IoCounters.ReadTransferCount + IoCounters.WriteTransferCount;
			}
		}
	}

	Sleep(UpdateTime);

	system_times SysTimes;

	GetSystemTimes(&SysTimes.IdleTime, &SysTimes.KernelTime, &SysTimes.UserTime);

	ULONGLONG SysKernelDiff = SubtractTimes(&SysTimes.KernelTime, &PrevSysTimes.KernelTime);
	ULONGLONG SysUserDiff = SubtractTimes(&SysTimes.UserTime, &PrevSysTimes.UserTime);
	ULONGLONG SysIdleDiff = SubtractTimes(&SysTimes.IdleTime, &PrevSysTimes.IdleTime);

	RunningProcessCount = 0;

	for(DWORD ProcIndex = 0; ProcIndex < NewProcessCount; ProcIndex++) {
		process_times ProcessTime = { 0 };
		process_times *PrevProcessTime = &ProcessTimes[ProcIndex];
		process *Process = &NewProcessList[ProcIndex];
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

			IO_COUNTERS IoCounters;
			if(GetProcessIoCounters(Process->Handle, &IoCounters)) {
				Process->DiskOperations = IoCounters.ReadTransferCount + IoCounters.WriteTransferCount;
				Process->DiskUsage = (DWORD)((Process->DiskOperations - Process->DiskOperationsPrev) * (1000/UpdateTime));
			}

			CloseHandle(Process->Handle);
			Process->Handle = 0;
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

static WORD GetConsoleColor(void)
{
	CONSOLE_SCREEN_BUFFER_INFO Csbi;
	GetConsoleScreenBufferInfo(ConsoleHandle, &Csbi);
	return Csbi.wAttributes;
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
		COORD Size;
		Size.X = (USHORT)Width;
		Size.Y = (USHORT)Height;
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
		if(SUCCEEDED(RegQueryValueEx(Key, _T("ProcessorNameString"), 0, 0, (LPBYTE)&CPUName[0], &Count))) {
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
		PollProcessList(1000);
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

static void FormatMemoryString(TCHAR *Buffer, DWORD BufferSize, unsigned __int64 Memory)
{
	double Value;
	TCHAR Unit[255];

	if (Memory < 1000ULL*1000) {
		Value = Memory / 1000.0;
		_tcscpy_s(Unit, _countof(Unit), _T("KB"));
	} else if (Memory < 1000ULL*1000*1000) {
		Value = Memory / (1000*1000.0);
		_tcscpy_s(Unit, _countof(Unit), _T("MB"));
	} else if (Memory < 1000ULL*1000*1000*1000) {
		Value = Memory / (1000*1000*1000.0);
		_tcscpy_s(Unit, _countof(Unit), _T("GB"));
	} else {
		Value = Memory / (1000*1000*1000*1000.0);
		_tcscpy_s(Unit, _countof(Unit), _T("TB"));
	}

	sprintf_s(Buffer, BufferSize, _T("% 8.1f %s"), Value, Unit);
}

static void WriteBlankLine(void)
{
	ConPrintf(_T("%*c"), Width, ' ');
}

static BOOL WINAPI CtrlHandler(DWORD signal)
{
	UNREFERENCED_PARAMETER(signal);
	exit(EXIT_SUCCESS);
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

	TCHAR MemoryStr[256];
	FormatMemoryString(MemoryStr, _countof(MemoryStr), Process->UsedMemory);

	if(ProcessSortType == SORT_BY_TREE) {
		TCHAR OffsetStr[256] = { 0 };
		if(Process->TreeDepth > 0) {
			for(DWORD i = 0; i < Process->TreeDepth-1; i++) {
				_tcscat_s(OffsetStr, _countof(OffsetStr), _T("|  "));
			}
			_tcscat_s(OffsetStr, _countof(OffsetStr), _T("`- "));
		}

		CharsWritten = ConPrintf(_T("\n%7u  %9s  %3u  %04.1f%%  %s  %4u  % 03.1f MB/s  %s"),
				Process->ID,
				Process->UserName,
				Process->BasePriority,
				Process->PercentProcessorTime,
				MemoryStr,
				Process->ThreadCount,
				ceil((double)Process->DiskUsage / 1000000.0 * 10.0) / 10.0,
				UpTimeStr
				);
		Color = CurrentColor;

		if(!Highlighted) {
			SetColor(Config.FGHighlightColor);
		}

		CharsWritten += ConPrintf(_T("  %s"), OffsetStr);
		SetColor(Color);

		CharsWritten += ConPrintf(_T("%s"), Process->ExeName);
	} else {
		CharsWritten = ConPrintf(_T("\n%7u  %9s  %3u  %04.1f%%  %s  %4u  % 03.1f MB/s  %s  %s"),
				Process->ID,
				Process->UserName,
				Process->BasePriority,
				Process->PercentProcessorTime,
				MemoryStr,
				Process->ThreadCount,
				ceil((double)Process->DiskUsage / 1000000.0 * 10.0) / 10.0,
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
	FollowProcess = FALSE;

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
	ConPrintf(_T("NTop " STRINGIZE_VALUE_OF(NTOP_VER) " - (C) 2019 Gian Sass\n"));
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
	ColorOverride = GetConsoleColor();
	PrintVersion();

	SetColor(0xE);
	ConPrintf(_T("USAGE\n"));

	SetColor(FOREGROUND_WHITE);
	ConPrintf(_T("\t%s [OPTIONS]\n\n"), argv0);

	const help_entry Options[] = {
		{ _T("-C"), _T("Use a monochrome color scheme.") },
		{ _T("-h"), _T("Display this help info.") },
		{ _T("-p PID,PID...\n"), _T("\tShow only the given PIDs.") },
		{ _T("-s COLUMN\n"), _T("\tSort by this column.") },
		{ _T("-u USERNAME\n"), _T("\tDisplay only processes of this user.") },
		{ _T("-v"), _T("Print version.") },
	};
	PrintHelpEntries(_T("OPTIONS"), _countof(Options), Options);

	const help_entry InteractiveCommands[] = {
		{ _T("Up and Down Arrows, PgUp and PgDown, j and k\n"), _T("\tScroll through the process list.") },
		{ _T("CTRL + Left and Right Arrows\n"), _T("\tChange the process sort column.") },
		{ _T("g"), _T("Go to the top of the process list.") },
		{ _T("G"), _T("Go to the bottom of the process list.") },
		{ _T("Space"), _T("Tag or untag selected process.") },
		{ _T("U"), _T("Untag all selected processes.") },
		{ _T("K"), _T("Kill all tagged processes.") },
		{ _T("I"), _T("Invert the sort order.") },
		{ _T("F"), _T("Follow process: if the sort order causes the currently selected\n"
				"\t\tprocess to move in the list, make the selection bar follow it.\n"
				"\t\tMoving the cursor manually automatically disables this feature."
				) },
		{ _T("n"), _T("Next in search.") },
		{ _T("N"), _T("Previous in search.") },
		{ _T("F10, q"), _T("Quit") },
		{ _T("M"), _T("Sort by memory usage") },
		{ _T("P"), _T("Sort by processor usage") },
	};
	PrintHelpEntries(_T("INTERACTIVE COMMANDS"), _countof(InteractiveCommands), InteractiveCommands);

	const help_entry ViCommands[] = {
		{ _T(":exec CMD\n"), _T("\tExecutes the given Windows command.") },
		{ _T(":kill PID(s)\n"), _T("\tKill all given processes.") },
		{ _T(":q, :quit\n"), _T("\tQuit NTop.") },
		{ _T("/PATTERN, :search PATTERN\n"), _T("\tDo a search.") },
		{ _T(":sort COLUMN\n"), _T("\tSort the process list after the given column.") },
		{ _T(":tree"), _T("View process tree.") },
	};
	PrintHelpEntries(_T("VI COMMANDS"), _countof(ViCommands), ViCommands);

	SetColor(ColorOverride);
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
	} else if(!lstrcmpi(Name, _T("PROCESS"))) {
		*Dest = SORT_BY_PROCESS;
		return TRUE;
	} else if(!lstrcmpi(Name, _T("DISK"))) {
		*Dest = SORT_BY_DISK_USAGE;
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

static vi_message_type CurrentViMessageType = VI_NOTICE;
static TCHAR *ViMessage;

void SetViMessage(vi_message_type MessageType, TCHAR *Fmt, ...)
{
	CurrentViMessageType = MessageType;

	va_list VaList;

	va_start(VaList, Fmt);
	_vstprintf_s(ViMessage, DEFAULT_STR_SIZE, Fmt, VaList);
	va_end(VaList);
}

static BOOL ViMessageActive(void)
{
	return _tcslen(ViMessage) > 0;
}

static void WriteVi(void)
{
	SetConCursorPos(0, (SHORT)Height-2);

	if(ViMessageActive()) {
		switch(CurrentViMessageType) {
		case VI_NOTICE:
			// Default?
			break;
		case VI_ERROR:
			SetColor(Config.ErrorColor);
			break;
		}

		int CharsWritten = ConPrintf(_T("\n%s"), ViMessage);

		for (; CharsWritten < Width + 1; CharsWritten++) {
			ConPutc(' ');
		}
	} else {
		SetColor(FOREGROUND_WHITE);
		ConPutc('\n');
		WriteBlankLine();
	}
}

static void HideViMessage(void)
{
	ViMessage[0] = _T('\0');
	WriteVi();
}

void ClearViMessage(void)
{
	if(ViMessageActive()) {
		SetConsoleMode(ConsoleHandle, ENABLE_PROCESSED_INPUT | DISABLE_NEWLINE_AUTO_RETURN);
		HideViMessage();
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
	CaretState = TRUE;
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
						if(CTRLState) {
							if(ProcessSortType == 0) {
								ProcessSortType = SORT_TYPE_MAX - 1;
							} else {
								--ProcessSortType;
							}
							ChangeProcessSortType(ProcessSortType);
							*Redraw = TRUE;
						}
						break;
					case VK_RIGHT:
						if(CTRLState) {
							if(ProcessSortType == SORT_TYPE_MAX - 1) {
								ProcessSortType = 0;
							} else {
								++ProcessSortType;
							}
							ChangeProcessSortType(ProcessSortType);
							*Redraw = TRUE;
						}
						break;
					case VK_F10:
						exit(EXIT_SUCCESS);  // Top compatibility
					default:
						switch(InputRecord.Event.KeyEvent.uChar.AsciiChar) {
						case 'K':
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
						case '/':
							ViEnableInput('/');
							ResetCaret();
							*Redraw = TRUE;
							break;
						case ':':
							ViEnableInput(':');
							ResetCaret();
							*Redraw = TRUE;
							break;
						case 'n':
							SearchNext();
							*Redraw = TRUE;
							break;
						case 'N':
							SearchPrevious();
							*Redraw = TRUE;
							break;
						case 'j':
							DoScroll(SCROLL_DOWN, Redraw);
							break;
						case 'k':
							DoScroll(SCROLL_UP, Redraw);
							break;
						// Top compatibility keys:
						case 'M':
							ChangeProcessSortType(SORT_BY_USED_MEMORY);
							*Redraw = TRUE;
							break;
						case 'P':
							ChangeProcessSortType(SORT_BY_PROCESSOR_TIME);
							*Redraw = TRUE;
							break;
						case 'q':
							exit(EXIT_SUCCESS);
						}
						break;
					}
				} else {
					if(ViHandleInputKey(&InputRecord.Event.KeyEvent)) {
						*Redraw = TRUE;
					}
				}
			} else {
				if(!InInputMode) {
					if(InputRecord.Event.KeyEvent.wVirtualKeyCode == VK_CONTROL) {
						CTRLState = FALSE;
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
						Token = _tcstok_s(0, Delim, &Context);
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
						  0,
						  CONSOLE_TEXTMODE_BUFFER,
						  0);

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


	ViMessage = xcalloc(DEFAULT_STR_SIZE, 1);
	ViInit();

	PollConsoleInfo();
	PollInitialSystemInfo();
	PollSystemInfo();
	PollProcessList(50);

	TCHAR MenuBar[256] = { 0 };
	wsprintf(MenuBar, _T("NTop on %s"), ComputerName);

	ProcessListThread = CreateThread(0, 0, PollProcessListThreadProc, 0, 0, 0);

	ULONGLONG StartTicks = GetTickCount64();

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

		CPUInfoChars += (int)_tcslen(CPUNameBuf);
		TCHAR CPUInfoBuf[256];
		CPUInfoChars += wsprintf(CPUInfoBuf, _T("%s (%u Cores)"), CPUName, CPUCoreCount);

		int TaskInfoChars = 0;

		TCHAR TasksNameBuf[] = _T("  Tasks: ");

		TaskInfoChars += (int)_tcsclen(TasksNameBuf);
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

		const process_list_column ProcessListColumns[] = {
			{ _T("ID"),	7 },
			{ _T("USER"),	9 },
			{ _T("PRI"),	3 },
			{ _T("CPU%"),	5 },
			{ _T("MEM"),	11 },
			{ _T("THRD"),	4 },
			{ _T("DISK"),	9 },
			{ _T("TIME"),	TIME_STR_SIZE - 1 },
			{ _T("PROCESS"),	-1 },
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
			if (CaretState) {
				ConPutc('_');
				++CharsWritten;
			}

			for(; CharsWritten < Width; CharsWritten++) {
				ConPutc(' ');
			}
		} else if(ViMessageActive()) {
			WriteVi();
		} else {
			ConPrintf(_T("\n%*c"), Width - 1, ' ');
		}
		SetConsoleMode(ConsoleHandle, ENABLE_PROCESSED_INPUT|ENABLE_WRAP_AT_EOL_OUTPUT);

#ifdef _DEBUG
		ULONGLONG T2 = GetTickCount64();

		ULONG Diff = (ULONG)(T2 - T1);

		TCHAR DebugBuffer[256];
		wsprintf(DebugBuffer, _T("Drawing took: %lu ms\n"), Diff);
		OutputDebugString(DebugBuffer);
#endif

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

			if(Now - StartTicks >= Config.RedrawInterval) {
				PollSystemInfo();
				StartTicks = Now;
				break;
			}

            /* FIXME: ignore redrawing the caret for now because we can't do it without redrawing the entire screen yet. */

            /*
			if(Now - CaretTicks >= CARET_INTERVAL) {
				CaretTicks = Now;
				CaretState = !CaretState;
				break;
			}
            */

			Sleep(INPUT_LOOP_DELAY);
		}
	}

	WaitForSingleObject(ProcessListThread, INFINITE);
	return EXIT_SUCCESS;
}
