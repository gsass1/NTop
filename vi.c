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

#include "vi.h"
#include "ntop.h"
#include "util.h"
#include <conio.h>
#include <stdio.h>
#include <windows.h>

TCHAR *CurrentInputStr;
int InInputMode = 0;
static DWORD InputIndex;

typedef int (*cmd_fn)(DWORD Argc, TCHAR **Argv);

typedef struct cmd {
	TCHAR *Name;
	cmd_fn CmdFunc;
} cmd;

#define COMMAND_FUNC(Name) static int Name##_func(DWORD Argc, TCHAR **Argv)
#define COMMAND_ALIAS(Name, AliasName) static int Name##_func(DWORD Argc, TCHAR **Argv) { return AliasName##_func(Argc, Argv); }
#define COMMAND(Name) { _T(#Name), Name##_func }

static TCHAR *History[256];
static DWORD HistoryCount;
static DWORD HistoryBufSize;
static DWORD HistoryIndex;

static void PushToHistory(TCHAR *Str)
{
	int Length = (int)_tcsclen(Str);
	History[HistoryCount] = xmalloc(sizeof **History * (Length + 1));
	_tcscpy_s(History[HistoryCount], Length+1, Str);
	HistoryCount++;
	HistoryIndex++;
}

static void HistoryPrevious(TCHAR **Str)
{
	HistoryIndex--;
	_tcscpy_s(*Str, DEFAULT_STR_SIZE, History[HistoryIndex]);
	InputIndex = (DWORD)_tcsclen(*Str);
}

static void HistoryNext(TCHAR **Str)
{
	HistoryIndex++;
	_tcscpy_s(*Str, DEFAULT_STR_SIZE, History[HistoryIndex]);
	InputIndex = (DWORD)_tcsclen(*Str);
}

COMMAND_FUNC(kill)
{
	if(Argc == 0) {
		SetViMessage(VI_ERROR, _T("Usage: kill PID(s)"));
		return 1;
	}

	for(DWORD i = 0; i < Argc; i++) {
		DWORD Pid = _tcstoul(Argv[i], 0, 10);

		/*
		 * strtoul returns 0 when conversion failed and we cannot kill pid 0 anyway
		 */
		if(Pid == 0) {
			SetViMessage(VI_ERROR, _T("Not a valid pid: %s"), Argv[i]);
			continue;
		}

		HANDLE Handle = OpenProcess(PROCESS_TERMINATE, FALSE, Pid);
		if(!Handle) {
			SetViMessage(VI_ERROR, _T("Could not open process: %ld: 0x%08x"), Pid, GetLastError());
			continue;
		}

		if(!TerminateProcess(Handle, 9)) {
			SetViMessage(VI_ERROR, _T("Failed to kill process: %ld: 0x%08x"), Pid, GetLastError());
			CloseHandle(Handle);
			return 1;
		}

		CloseHandle(Handle);
	}

	return 0;
}


COMMAND_FUNC(tree)
{
	UNREFERENCED_PARAMETER(Argv);

	if(Argc != 0) {
		SetViMessage(VI_ERROR, _T("Error: trailing characters"));
		return 1;
	}

	ChangeProcessSortType(SORT_BY_TREE);

	return 0;
}

COMMAND_FUNC(exec)
{
	if(Argc == 0) {
		SetViMessage(VI_ERROR, _T("Usage: exec COMMAND"));
		return 1;
	}

	TCHAR CommandLine[4096] = { 0 };

	for(DWORD i = 0; i < Argc; ++i) {
		_tcscat_s(CommandLine, _countof(CommandLine), Argv[i]);

		if(i != Argc - 1) {
			DWORD Length = (DWORD)_tcslen(CommandLine);
			CommandLine[Length] = L' ';
		}
	}

	STARTUPINFO StartupInfo;
	PROCESS_INFORMATION ProcInfo;
	ZeroMemory(&StartupInfo, sizeof(StartupInfo));
	ZeroMemory(&ProcInfo, sizeof(ProcInfo));
	StartupInfo.cb = sizeof(StartupInfo);

	BOOL Ret = CreateProcess(0, CommandLine, 0, 0, FALSE, 0, 0, 0, &StartupInfo, &ProcInfo);

	if(!Ret) {
		SetViMessage(VI_ERROR, _T("Failed to create process: 0x%08x"), GetLastError());
		return 1;
	}

	return 1;
}

COMMAND_FUNC(q)
{
	UNREFERENCED_PARAMETER(Argc);
	UNREFERENCED_PARAMETER(Argv);

	exit(EXIT_SUCCESS);
}
COMMAND_ALIAS(quit, q);

COMMAND_FUNC(sort)
{
	if(Argc != 1) {
		SetViMessage(VI_ERROR, _T("Usage: sort COLUMN"));
		return 1;
	}

	process_sort_type NewSortType;
	if(!GetProcessSortTypeFromName(Argv[0], &NewSortType)) {
		SetViMessage(VI_ERROR, _T("Unknown column: %s"), Argv[0]);
		return 1;
	}

	ChangeProcessSortType(NewSortType);

	return 0;
}

COMMAND_FUNC(search)
{
	if(Argc != 1) {
		SetViMessage(VI_ERROR, _T("Usage: search pattern"));
		return 1;
	}

	if(Argv[0][0] != _T('\0')) {
		StartSearch(Argv[0]);
	}

	return 0;
}

static cmd Commands[] = {
	COMMAND(exec),
	COMMAND(kill),
	COMMAND(q),
	COMMAND(quit),
	COMMAND(sort),
	COMMAND(tree),
	COMMAND(search),
};

typedef struct cmd_parse_result {
	TCHAR *Name;
	DWORD Argc;
	TCHAR **Args;
} cmd_parse_result;

static void PushArg(cmd_parse_result *ParseResult)
{
	++ParseResult->Argc;

	if(ParseResult->Args == 0) {
		ParseResult->Args = xmalloc(1 * sizeof *ParseResult->Args);
	} else {
		ParseResult->Args = xrealloc(ParseResult->Args, ParseResult->Argc * sizeof *ParseResult->Args);
	}

	ParseResult->Args[ParseResult->Argc-1] = xcalloc(DEFAULT_STR_SIZE, sizeof(TCHAR));
}

static void FreeCmdParseResult(cmd_parse_result *ParseResult)
{
	free(ParseResult->Name);
	for(DWORD i = 0; i < ParseResult->Argc; i++) {
		free(ParseResult->Args[i]);
	}
	free(ParseResult->Args);
}

static TCHAR *EatSpaces(TCHAR *Str)
{
	while(_istspace(*Str)) {
		Str++;
	}
	return Str;
}

static BOOL IsValidCharacter(TCHAR c)
{
	return (_istalnum(c) || c == _T('%')) || c == '/' || c == '.';
}

static BOOL ParseCommand(TCHAR *Str, cmd_parse_result *Result)
{
	Result->Argc = 0;
	Result->Args = 0;

	Result->Name = xmalloc(DEFAULT_STR_SIZE * sizeof *Result->Name);

	Str = EatSpaces(Str);

	int i = 0;

	/*
	 * TODO: should allow for commands to contain digits but not start with them
	 */
	while(_istalpha(*Str)) {
		Result->Name[i++] = *Str++;
	}

	Result->Name[i] = '\0';

	/*
	 * Do error when we stopped on an non-alphanumeric character and not on a
	 * terminator or space
	 */
	if(*Str != _T('\0') && !_istspace(*Str)) {
		/* parse error */
		free(Result->Name);
		return FALSE;
	}

	int InQuotes = FALSE;

	if(*Str != '\0') {
		i = 0;

		/*
		 * Read arguments
		 */
		while(*Str != '\0') {
			Str = EatSpaces(Str);

			if(*Str == '\0') {
				break;
			}

			if(*Str == _T('\"')) {
				InQuotes = TRUE;
				Str++;
			}

			PushArg(Result);

			int j = 0;
			if(InQuotes) {
				/*
				 * When inside quotes read tokens and spaces inbetween
				 * until we hit a closing quote or terminator
				 */
				do {
					while(_istspace(*Str) || IsValidCharacter(*Str)) {
						Result->Args[i][j++] = *Str++;
					}

					if(*Str == '\"') {
						++Str;
						InQuotes = FALSE;
						break;
					}
				} while(*Str != '\0');
			} else {
				/*
				 * When not inside quotes then only read single token
				 */
				while(IsValidCharacter(*Str)) {
					Result->Args[i][j++] = *Str++;
				}
			}

			Result->Args[i][j] = '\0';
			++i;

			/*
			 * Do error and clean up if either the string wasn't closed
			 * or if we hit a non-alphanumeric character or non space
			 */
			if(InQuotes || (*Str != _T('\0') && !_istspace(*Str))) {
				/* parse error */
				FreeCmdParseResult(Result);
				return FALSE;
			}
		}
	}

	return TRUE;
}

static void TryExec(TCHAR *Str)
{
	cmd_parse_result ParseResult;

	/* Search has an alias */
	if(Str[0] == '/') {
		TCHAR *Args[1];
		Args[0] = Str + 1;
		search_func(1, Args);
		return;
	}

	if(!ParseCommand(Str, &ParseResult)) {
		SetViMessage(VI_ERROR, _T("parse error"));
		return;
	}

	for(DWORD i = 0; i < _countof(Commands); i++) {
		cmd *Command = &Commands[i];

		if(_tcsicmp(Command->Name, ParseResult.Name) == 0) {
			Command->CmdFunc(ParseResult.Argc, ParseResult.Args);
			FreeCmdParseResult(&ParseResult);
			return;
		}
	}

	SetViMessage(VI_ERROR, _T("Not an editor command: %s"), ParseResult.Name);
	FreeCmdParseResult(&ParseResult);
}

void ViInit(void)
{
	CurrentInputStr = xcalloc(DEFAULT_STR_SIZE, 1);
}

void ViEnableInput(TCHAR InitialKey)
{
	memset(CurrentInputStr, 0, DEFAULT_STR_SIZE * sizeof *CurrentInputStr);
	CurrentInputStr[0] = InitialKey;

	ClearViMessage();
	InInputMode = 1;
	InputIndex = 1;
}

void ViDisableInput(void)
{
	CurrentInputStr[0] = 0;
	InInputMode = 0;

	FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE));
}

int ViHandleInputKey(KEY_EVENT_RECORD *KeyEvent)
{
	switch(KeyEvent->wVirtualKeyCode) {
	case VK_UP:
		if(HistoryCount != 0 && HistoryIndex != 0) {
			HistoryPrevious(&CurrentInputStr);
			return 1;
		}
		break;
	case VK_DOWN:
		if(HistoryIndex < HistoryCount-1) {
			HistoryNext(&CurrentInputStr);
			return 1;
		} 
		break;
	case VK_BACK:
		if(InputIndex != 0) {
			CurrentInputStr[--InputIndex] = '\0';
			if(InputIndex == 0) {
				ViDisableInput();
			}
			return 1;
		}
		break;
	case VK_ESCAPE:
		ViDisableInput();
		return 1;
	case VK_RETURN:
		ViExecInput();
		return 1;
	default:
		if(isprint(KeyEvent->uChar.AsciiChar)) {
			CurrentInputStr[InputIndex++] = KeyEvent->uChar.AsciiChar;
			return 1;
		}
	}

	return 0;
}

void ViExecInput(void)
{
	TCHAR *Cmd = CurrentInputStr;

	/*
	 * Ignore all preceeding colons and spaces
	 */
	while(*Cmd == ':' || _istspace(*Cmd)) {
		Cmd++;
	}

	/*
	 * We check for empty string here because empty strings shouldn't throw errors
	 */
	if(*Cmd != '\0') {
		TryExec(Cmd);
		PushToHistory(CurrentInputStr);
	}

	ViDisableInput();
}
