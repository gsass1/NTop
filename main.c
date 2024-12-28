#include "wmi.h"

#include <stdio.h>

#include <windows.h>
#include <synchapi.h>

int main()
{
        struct WMI* wmi = NewWMIService();

        int* loads = NULL;
        size_t count = 0;
        for(int i = 0; i < 1000 ; ++i)
        {
            WMIQueryProcessorTime(wmi, &loads, &count);
            for(size_t c = 0 ; c < count; ++c) printf("%d\n", loads[c]);
            printf("\n"); fflush(NULL);
            Sleep(500);
        }

        CloseWMIService(wmi);
}