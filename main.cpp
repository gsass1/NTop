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
        ULONG processor_enum_count = [&]() // IILE
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
        auto obtain_property_handle = [&](const wchar_t* property_name)
        {
            std::vector<winrt::com_ptr<IWbemObjectAccess>> enum_accessors(processor_enum_count);
            ULONG count;
            winrt::check_hresult(enumerator->GetObjects(
                0L,
                processor_enum_count,
                reinterpret_cast<IWbemObjectAccess**>(enum_accessors.data()),
                &count));

            CIMTYPE type;
            long handle;
            winrt::check_hresult(enum_accessors[0]->GetPropertyHandle(
                property_name,
                &type,
                &handle));

            return handle;
        };
        long percent_user_time_handle = obtain_property_handle(L"PercentUserTime"),
             percent_privileged_time_handle = obtain_property_handle(L"PercentPrivilegedTime"),
             percent_processor_time_handle = obtain_property_handle(L"PercentProcessorTime");

        std::vector<winrt::com_ptr<IWbemObjectAccess>> enum_accessors(processor_enum_count);
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

            // Loop over core loads. (Last entry seems to be an avarage of all cores.)
            for (size_t i = 0 ; i < std::thread::hardware_concurrency() ; ++i)
            {
                auto read_uint64_t_property = [&](long handle)
                {
                    long bytes_read;
                    std::uint64_t result;
                    winrt::check_hresult(enum_accessors[i]->ReadPropertyValue(
                        handle,
                        sizeof(std::uint64_t),
                        &bytes_read,
                        reinterpret_cast<byte*>(&result)));

                    return result;
                };

                std::cout <<
                    read_uint64_t_property(percent_user_time_handle) << "\t" <<
                    read_uint64_t_property(percent_privileged_time_handle) << "\t" <<
                    read_uint64_t_property(percent_processor_time_handle) << "\t" <<
                    std::endl;
            }

            std::cout << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds{1});
        }
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