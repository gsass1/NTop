#include "pdh.h"
#include "util.h"

#include <Windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <tchar.h>
#include <stdio.h>

static HANDLE      hEvent;
static LPTSTR      szCounterListBuffer;
static DWORD       dwCounterListSize;
static LPTSTR      szInstanceListBuffer;
static DWORD       dwInstanceListSize;
static LPTSTR      szThisInstance;
static int          nCPU;
static HCOUNTER *hIdleCounters;
static HCOUNTER *hPrivelegedCounters;
static HCOUNTER *hProcessorCounters;

void InitPDH(void)
{
    PDH_STATUS Status = ERROR_SUCCESS;

    hEvent = CreateEvent(NULL, FALSE, FALSE, TEXT("NTop"));

    // Determine the required buffer size for the data. 
    Status = PdhEnumObjectItems(
            NULL,                   // real time source
            NULL,                   // local machine
            TEXT("Processor"),        // object to enumerate
            szCounterListBuffer,    // pass NULL and 0
            &dwCounterListSize,     // to get length required
            szInstanceListBuffer,   // buffer size 
            &dwInstanceListSize,    // 
            PERF_DETAIL_WIZARD,     // counter detail level
            0);

    if (Status == PDH_MORE_DATA)
    {
        // Allocate the buffers and try the call again.
        szCounterListBuffer = xmalloc(
                (dwCounterListSize * sizeof(TCHAR)));
        szInstanceListBuffer = xmalloc(
                (dwInstanceListSize * sizeof(TCHAR)));

        if ((szCounterListBuffer != NULL) &&
                (szInstanceListBuffer != NULL))
        {
            Status = PdhEnumObjectItems(
                    NULL,                 // real time source
                    NULL,                 // local machine
                    TEXT("Processor"),      // object to enumerate
                    szCounterListBuffer,  // buffer to receive counter list
                    &dwCounterListSize,
                    szInstanceListBuffer, // buffer to receive instance list 
                    &dwInstanceListSize,
                    PERF_DETAIL_WIZARD,   // counter detail level
                    0);

            if(Status != ERROR_SUCCESS) {
                Die("Cannot initialize PDH! %d\n", Status);
            }

            // is followed by a second null-terminator.
            for (szThisInstance = szInstanceListBuffer;
                    *szThisInstance != 0;
                    szThisInstance += lstrlen(szThisInstance) + 1)
            {
                // _tprintf (TEXT("\n  %s"), szThisInstance);
                if (0 != _tcscmp(szThisInstance, TEXT("_Total")))
                {
                    // it's not the toalizer, so count it
                    nCPU++;
                }
            }

            hIdleCounters = xmalloc(sizeof(HCOUNTER) * nCPU);
            hPrivelegedCounters = xmalloc(sizeof(HCOUNTER) * nCPU);
            hProcessorCounters = xmalloc(sizeof(HCOUNTER) * nCPU);

            HQUERY hQuery;
            Status = PdhOpenQuery(NULL, 1, &hQuery);

            for (int n = 0; n < nCPU; n++)
            {
                TCHAR szCounterPath[255];
                _stprintf_s(szCounterPath, 255, TEXT("\\Processor(%d)\\%% Processor Time"), n);
                Status = PdhAddCounter(hQuery,
                        szCounterPath,
                        n,
                        &hProcessorCounters[n]);
                if (Status != ERROR_SUCCESS)
                {
                    Die("oof");
                    /* _tprintf(TEXT("Couldn't add counter \"%s\": 0x8.8X\n"), */
                    /*         szCounterPath, Status); */
                    break;
                }

                _stprintf_s(szCounterPath, 255, TEXT("\\Processor(%d)\\%% Idle Time"), n);
                Status = PdhAddCounter(hQuery,
                        szCounterPath,
                        n,
                        &hIdleCounters[n]);
                if (Status != ERROR_SUCCESS)
                {
                    Die("oof");
                    /* _tprintf(TEXT("Couldn't add counter \"%s\": 0x8.8X\n"), */
                    /*         szCounterPath, Status); */
                    break;
                }

                _stprintf_s(szCounterPath, 255, TEXT("\\Processor(%d)\\%% Privileged Time"), n);
                Status = PdhAddCounter(hQuery,
                        szCounterPath,
                        n,
                        &hPrivelegedCounters[n]);
                if (Status != ERROR_SUCCESS)
                {
                    Die("oof");
                    /* _tprintf(TEXT("Couldn't add counter \"%s\": 0x8.8X\n"), */
                    /*         szCounterPath, Status); */
                    break;
                }
            }

            Status = PdhCollectQueryDataEx(hQuery, 1, hEvent);
            DWORD dwWaitResult = WaitForSingleObject(hEvent, INFINITE);
        }
    }
}

cpu_counters GetCPUUsage(void)
{
    PDH_STATUS Status = ERROR_SUCCESS;
    cpu_counters CC;
    DWORD dwWaitResult = WaitForSingleObject(hEvent, INFINITE);

    DWORD dwCounterType = 0;

    CC.nCPU = nCPU;

    CC.usage = malloc(sizeof(double) * nCPU);

    for (int n = 0; n < nCPU; n++)
    {
        PDH_FMT_COUNTERVALUE cvIdle, cvPriveleged, cvProcessor;
        Status = PdhGetFormattedCounterValue(hIdleCounters[n], PDH_FMT_DOUBLE, &dwCounterType, &cvIdle);
        if (Status != ERROR_SUCCESS)
        {
            _tprintf(TEXT("0: Error 0x%8.8X\n"), Status);
            break;
        }

        Status = PdhGetFormattedCounterValue(hPrivelegedCounters[n], PDH_FMT_DOUBLE, &dwCounterType, &cvPriveleged);
        if (Status != ERROR_SUCCESS)
        {
            _tprintf(TEXT("0: Error 0x%8.8X\n"), Status);
            break;
        }

        Status = PdhGetFormattedCounterValue(hProcessorCounters[n], PDH_FMT_DOUBLE, &dwCounterType, &cvProcessor);
        if (Status != ERROR_SUCCESS)
        {
            _tprintf(TEXT("0: Error 0x%8.8X\n"), Status);
            break;
        }

        //_tprintf(TEXT("%-4d %#7.3f %#7.3f %#7.3f\n"), n, cvIdle.doubleValue, cvPriveleged.doubleValue, cvProcessor.doubleValue);
        double time = (cvPriveleged.doubleValue + cvProcessor.doubleValue)/100.0;
        double percentage = time;
        if(percentage > 1.0) {
            percentage = 1.0;
        }

        CC.usage[n] = percentage;
    }

    return CC;
}
