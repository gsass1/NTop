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

#ifndef NTOP_H
#define NTOP_H

#include <tchar.h>

#define DEFAULT_STR_SIZE 1024

typedef enum process_sort_type {
	SORT_BY_ID,
	SORT_BY_USER_NAME,
	SORT_BY_PRIORITY,
	SORT_BY_PROCESSOR_TIME,
	SORT_BY_USED_MEMORY,
	SORT_BY_THREAD_COUNT,
	SORT_BY_DISK_USAGE,
	SORT_BY_UPTIME,
	SORT_BY_PROCESS,
	// NOTE: declaring SORT_TYPE_MAX before SORT_BY_TREE is there solely
	// because we do not actually treat it as an actual sort type, but the
	// code in ProcessInput thinks it is.
	SORT_TYPE_MAX,
	SORT_BY_TREE,
} process_sort_type;

int GetProcessSortTypeFromName(const TCHAR *Name, process_sort_type *Dest);
void ChangeProcessSortType(process_sort_type NewProcessSortType);
void StartSearch(const TCHAR *Pattern);

typedef enum vi_message_type {
	VI_NOTICE,
	VI_ERROR,
} vi_message_type;

void SetViMessage(vi_message_type MessageType, TCHAR *Fmt, ...);
void ClearViMessage(void);

#endif
