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

int main()
{
    try
    {
        winrt::init_apartment();

        auto locator = winrt::create_instance<IWbemLocator>(CLSID_WbemLocator);
        winrt::com_ptr<IWbemServices> services;
        winrt::check_hresult(locator->ConnectServer(
            wil::make_bstr(LR"(ROOT\CIMV2)").get(),
            nullptr, nullptr,
            0, 0, 0, 0,
            services.put()));

        winrt::check_hresult(CoSetProxyBlanket(
            services.get(),
            RPC_C_AUTHN_WINNT,
            RPC_C_AUTHZ_NONE,
            nullptr,
            RPC_C_AUTHN_LEVEL_CALL,
            RPC_C_IMP_LEVEL_IMPERSONATE,
            nullptr,
            EOAC_NONE));

        //std::vector<int> loads(std::thread::hardware_concurrency());

        auto refresher = winrt::create_instance<IWbemRefresher>(CLSID_WbemRefresher);
        winrt::com_ptr<IWbemConfigureRefresher> config;
        winrt::check_hresult(refresher->QueryInterface(config.put()));

        winrt::com_ptr<IWbemHiPerfEnum> enumerator;
        long id;
        winrt::check_hresult(config->AddEnum(
            services.get(),
            L"Win32_PerfFormattedData_PerfOS_Processor",
            0,
            nullptr,
            enumerator.put(),
            &id));

        // Obtain queried object count (using empty buffer for result storage)
        winrt::check_hresult(refresher->Refresh(0L));
        ULONG logical_core_count = [&]() // IILE
            {
                ULONG count;
                auto hresult = enumerator->GetObjects(
                    0L,
                    0,
                    nullptr,
                    &count
                );
                if (hresult != WBEM_E_BUFFER_TOO_SMALL)
                    throw std::runtime_error{"Expecting WBEM_E_BUFFER_TOO_SMALL obtaining object count, got something else."};

                return count;
            }();

        // Obtain and remember property handles
        CIMTYPE percent_processor_time_handle = [&]() // IILE
            {
                std::vector<winrt::com_ptr<IWbemObjectAccess>> enum_accessors(logical_core_count);
                ULONG count;
                winrt::check_hresult(enumerator->GetObjects(
                    0L,
                    logical_core_count,
                    reinterpret_cast<IWbemObjectAccess**>(enum_accessors.data()),
                    &count));

                //if (hresult != WBEM_E_BUFFER_TOO_SMALL)
                //    throw std::runtime_error{"Expecting hresult == WBEM_E_BUFFER_TOO_SMALL and count == 1 while obtaining PercentProcessorTime property handle."};

                CIMTYPE handle, type;
                winrt::check_hresult(enum_accessors[0]->GetPropertyHandle(
                    L"PercentProcessorTime",
                    &type,
                    &handle));

                return handle;
            }();

        std::vector<winrt::com_ptr<IWbemObjectAccess>> enum_accessors(logical_core_count);
        //std::wstring percent_processor_time_string(4, '\0');
        std::uint64_t percent_processor_time;
        for(int I = 0 ; I < 10 ; ++I)
        {
            winrt::check_hresult(refresher->Refresh(0L));

            ULONG count;
            // Assumes that the layout of winrt::com_ptr<T> is the same as the stored winrt::impl::abi_t<T>
            winrt::check_hresult(enumerator->GetObjects(
                0L,
                static_cast<ULONG>(enum_accessors.size()),
                reinterpret_cast<IWbemObjectAccess**>(enum_accessors.data()),
                &count));

            for (auto& enum_access : enum_accessors)
            {
                BSTR* type_name = nullptr;
                CIMTYPE prop_type;
                winrt::check_hresult(enum_access->GetPropertyInfoByHandle(
                    percent_processor_time_handle,
                    type_name,
                    &prop_type
                ));

                long bytes_read;
                winrt::check_hresult(enum_access->ReadPropertyValue(
                    percent_processor_time_handle,
                    sizeof(std::uint64_t),
                    &bytes_read,
                    reinterpret_cast<byte*>(&percent_processor_time)));

                std::cout << percent_processor_time << std::endl;

                //wil::unique_variant percent;
                //winrt::check_hresult(enum_access->Get( L"PercentProcessorTime", 0, percent.addressof(), nullptr, nullptr));
                //std::cout << std::stoi(percent.bstrVal) << std::endl;

            }

            std::cout << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds{1});
        }
/*
        winrt::com_ptr<IEnumWbemClassObject> enumerator;
        winrt::check_hresult(services->ExecQuery(
            L"WQL",
            L"SELECT PercentProcessorTime FROM Win32_PerfFormattedData_PerfOS_Processor WHERE Name <> '_Total'",
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
            nullptr,
            enumerator.put())
        );

        //while (enumerator)
        for (size_t i = 0 ; i < loads.size() ; ++i)
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

            loads.at(i) = std::stoi(percent.bstrVal);
        }

        for(auto& load : loads) std::cout << load << "\n";
        std::cout << std::endl;

        std::this_thread::sleep_for(std::chrono::milliseconds{500});
*/
    }
    catch(winrt::hresult_error& err)
    {
        std::wcerr << err.message().c_str() << " (" << err.code() << ")" << std::endl;
        std::exit(err.code());
    }
    catch(std::exception& err)
    {
        std::cerr << err.what() << std::endl;
        std::exit(-1);
    }

    return 0;
}