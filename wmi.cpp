// Configuration defines
#define _WIN32_DCOM
#define WIL_ENABLE_EXCEPTIONS

#include <wil/resource.h>

#include <windows.h>

// C++/WinRT includes
#include <winrt/base.h>

#include <WbemIdl.h>

#include <iostream>
#include <exception>

struct WMI
{
    winrt::impl::com_ref<IWbemLocator> locator;
    winrt::com_ptr<IWbemServices> services;

    std::vector<int> loads;
};

extern "C"
{
    WMI* NewWMIService()
    {
        winrt::init_apartment();

        WMI* wmi = new WMI{};

        wmi->locator = winrt::create_instance<IWbemLocator>(CLSID_WbemLocator);
        winrt::check_hresult(wmi->locator->ConnectServer(
            wil::make_bstr(LR"(ROOT\CIMV2)").get(),
            nullptr, nullptr,
            0, 0, 0, 0,
            wmi->services.put())
        );

        winrt::check_hresult(CoSetProxyBlanket(
            wmi->services.get(),
            RPC_C_AUTHN_WINNT,
            RPC_C_AUTHZ_NONE,
            nullptr,
            RPC_C_AUTHN_LEVEL_CALL,
            RPC_C_IMP_LEVEL_IMPERSONATE,
            nullptr,
            EOAC_NONE)
        );

        wmi->loads.resize(std::thread::hardware_concurrency());

        return wmi;
    }

    int WMIQueryProcessorTime(WMI* wmi, int** loads, size_t* count)
    {
        winrt::com_ptr<IEnumWbemClassObject> enumerator;
        winrt::check_hresult(wmi->services->ExecQuery(
            L"WQL",
            L"SELECT PercentProcessorTime FROM Win32_PerfFormattedData_PerfOS_Processor WHERE Name <> '_Total'",
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
            nullptr,
            enumerator.put())
        );

        for (size_t i = 0 ; i < wmi->loads.size() ; ++i)
        {
            winrt::com_ptr<IWbemClassObject> class_object;
            ULONG ret;
            winrt::check_hresult(enumerator->Next(
                WBEM_INFINITE,
                1,
                class_object.put(),
                &ret)
            );

            if (ret == 0) { break; }

            wil::unique_variant percent;
            winrt::check_hresult(class_object->Get( L"PercentProcessorTime", 0, percent.addressof(), nullptr, nullptr));

            wmi->loads.at(i) = std::stoi(percent.bstrVal);

            *loads = wmi->loads.data();
            *count = wmi->loads.size();
        }

        return 0;
    }

    void CloseWMIService(WMI* wmi)
    {
        delete wmi;
    }
}
