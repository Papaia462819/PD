#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <devpkey.h>
#include <objbase.h>
#include <wtsapi32.h>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#define WIDEN2(x) L##x
#define WIDEN(x) WIDEN2(x)

namespace
{
    struct RegistryTarget
    {
        HKEY root = nullptr;
        std::wstring rootName;
        std::wstring subkey;
    };

    class RegistryKey
    {
    public:
        RegistryKey() = default;

        explicit RegistryKey(HKEY key) noexcept : key_(key)
        {
        }

        RegistryKey(const RegistryKey&) = delete;
        RegistryKey& operator=(const RegistryKey&) = delete;

        RegistryKey(RegistryKey&& other) noexcept : key_(other.key_)
        {
            other.key_ = nullptr;
        }

        RegistryKey& operator=(RegistryKey&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                key_ = other.key_;
                other.key_ = nullptr;
            }
            return *this;
        }

        ~RegistryKey()
        {
            reset();
        }

        HKEY get() const noexcept
        {
            return key_;
        }

        void reset(HKEY key = nullptr) noexcept
        {
            if (key_ != nullptr)
            {
                RegCloseKey(key_);
            }
            key_ = key;
        }

    private:
        HKEY key_ = nullptr;
    };

    class DeviceInfoSet
    {
    public:
        explicit DeviceInfoSet(HDEVINFO handle) noexcept : handle_(handle)
        {
        }

        DeviceInfoSet(const DeviceInfoSet&) = delete;
        DeviceInfoSet& operator=(const DeviceInfoSet&) = delete;

        DeviceInfoSet(DeviceInfoSet&& other) noexcept : handle_(other.handle_)
        {
            other.handle_ = INVALID_HANDLE_VALUE;
        }

        DeviceInfoSet& operator=(DeviceInfoSet&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                handle_ = other.handle_;
                other.handle_ = INVALID_HANDLE_VALUE;
            }
            return *this;
        }

        ~DeviceInfoSet()
        {
            reset();
        }

        HDEVINFO get() const noexcept
        {
            return handle_;
        }

        explicit operator bool() const noexcept
        {
            return handle_ != INVALID_HANDLE_VALUE;
        }

        void reset(HDEVINFO handle = INVALID_HANDLE_VALUE) noexcept
        {
            if (handle_ != INVALID_HANDLE_VALUE)
            {
                SetupDiDestroyDeviceInfoList(handle_);
            }
            handle_ = handle;
        }

    private:
        HDEVINFO handle_ = INVALID_HANDLE_VALUE;
    };

    struct DeviceSelection
    {
        DWORD index = 0;
        SP_DEVINFO_DATA deviceInfo{};
    };

    class ServiceHandle
    {
    public:
        ServiceHandle() = default;

        explicit ServiceHandle(SC_HANDLE handle) noexcept : handle_(handle)
        {
        }

        ServiceHandle(const ServiceHandle&) = delete;
        ServiceHandle& operator=(const ServiceHandle&) = delete;

        ServiceHandle(ServiceHandle&& other) noexcept : handle_(other.handle_)
        {
            other.handle_ = nullptr;
        }

        ServiceHandle& operator=(ServiceHandle&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                handle_ = other.handle_;
                other.handle_ = nullptr;
            }
            return *this;
        }

        ~ServiceHandle()
        {
            reset();
        }

        SC_HANDLE get() const noexcept
        {
            return handle_;
        }

        explicit operator bool() const noexcept
        {
            return handle_ != nullptr;
        }

        void reset(SC_HANDLE handle = nullptr) noexcept
        {
            if (handle_ != nullptr)
            {
                CloseServiceHandle(handle_);
            }
            handle_ = handle;
        }

    private:
        SC_HANDLE handle_ = nullptr;
    };

    constexpr const wchar_t* kHelloWorldServiceName = L"PDHelloWorldService";
    constexpr const wchar_t* kHelloWorldServiceDisplayName = L"PD Hello World Service";
    constexpr DWORD kServiceCommandTimeoutMs = 30000;

    SERVICE_STATUS g_helloWorldServiceStatus{};
    SERVICE_STATUS_HANDLE g_helloWorldServiceStatusHandle = nullptr;
    HANDLE g_helloWorldServiceStopEvent = nullptr;

    std::wstring ToUpper(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch)
        {
            return static_cast<wchar_t>(std::towupper(ch));
        });
        return value;
    }

    std::wstring GetWin32ErrorMessage(DWORD errorCode)
    {
        LPWSTR rawMessage = nullptr;
        const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
        const DWORD length = FormatMessageW(
            flags,
            nullptr,
            errorCode,
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            reinterpret_cast<LPWSTR>(&rawMessage),
            0,
            nullptr);

        std::wstring message = L"Unknown error";
        if (length != 0U && rawMessage != nullptr)
        {
            message.assign(rawMessage, length);
            while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n'))
            {
                message.pop_back();
            }
        }

        if (rawMessage != nullptr)
        {
            LocalFree(rawMessage);
        }

        return message;
    }

    bool IsSamePropertyKey(const DEVPROPKEY& lhs, const DEVPROPKEY& rhs)
    {
        return IsEqualGUID(lhs.fmtid, rhs.fmtid) && lhs.pid == rhs.pid;
    }

    std::wstring GuidToString(REFGUID guid)
    {
        wchar_t buffer[64] = {};
        if (StringFromGUID2(guid, buffer, ARRAYSIZE(buffer)) == 0)
        {
            return L"{invalid-guid}";
        }
        return buffer;
    }

    std::wstring BytesToHexString(const BYTE* data, const size_t size)
    {
        std::wostringstream stream;
        stream << std::uppercase << std::hex << std::setfill(L'0');

        for (size_t index = 0; index < size; ++index)
        {
            if (index != 0)
            {
                stream << L' ';
            }
            stream << std::setw(2) << static_cast<unsigned int>(data[index]);
        }

        return stream.str();
    }

    template <typename T>
    std::optional<T> ReadScalar(const std::vector<BYTE>& data)
    {
        if (data.size() < sizeof(T))
        {
            return std::nullopt;
        }

        T value{};
        std::memcpy(&value, data.data(), sizeof(T));
        return value;
    }

    std::vector<std::wstring> ParseWideMultiString(const BYTE* data, const size_t sizeInBytes)
    {
        std::vector<std::wstring> items;
        if (data == nullptr || sizeInBytes < sizeof(wchar_t))
        {
            return items;
        }

        const auto* current = reinterpret_cast<const wchar_t*>(data);
        const auto* end = reinterpret_cast<const wchar_t*>(data + sizeInBytes);

        while (current < end && *current != L'\0')
        {
            const wchar_t* next = current;
            while (next < end && *next != L'\0')
            {
                ++next;
            }

            if (next == end)
            {
                break;
            }

            items.emplace_back(current, static_cast<size_t>(next - current));
            current = next + 1;
        }

        return items;
    }

    std::wstring JoinLines(const std::vector<std::wstring>& lines)
    {
        if (lines.empty())
        {
            return L"(none)";
        }

        std::wostringstream stream;
        for (size_t index = 0; index < lines.size(); ++index)
        {
            if (index != 0)
            {
                stream << L"\n    - ";
            }
            stream << lines[index];
        }
        return stream.str();
    }

    std::wstring RegistryTypeToString(const DWORD type)
    {
        switch (type)
        {
        case REG_NONE: return L"REG_NONE";
        case REG_SZ: return L"REG_SZ";
        case REG_EXPAND_SZ: return L"REG_EXPAND_SZ";
        case REG_BINARY: return L"REG_BINARY";
        case REG_DWORD: return L"REG_DWORD";
        case REG_DWORD_BIG_ENDIAN: return L"REG_DWORD_BIG_ENDIAN";
        case REG_LINK: return L"REG_LINK";
        case REG_MULTI_SZ: return L"REG_MULTI_SZ";
        case REG_RESOURCE_LIST: return L"REG_RESOURCE_LIST";
        case REG_FULL_RESOURCE_DESCRIPTOR: return L"REG_FULL_RESOURCE_DESCRIPTOR";
        case REG_RESOURCE_REQUIREMENTS_LIST: return L"REG_RESOURCE_REQUIREMENTS_LIST";
        case REG_QWORD: return L"REG_QWORD";
        default: return L"REG_UNKNOWN";
        }
    }

    std::wstring FormatRegistryValueData(const DWORD type, const std::vector<BYTE>& data)
    {
        switch (type)
        {
        case REG_NONE:
            return data.empty() ? L"(empty)" : BytesToHexString(data.data(), data.size());

        case REG_SZ:
        case REG_EXPAND_SZ:
        case REG_LINK:
        {
            if (data.empty())
            {
                return L"";
            }

            const auto* value = reinterpret_cast<const wchar_t*>(data.data());
            const size_t charCount = data.size() / sizeof(wchar_t);
            size_t length = 0;
            while (length < charCount && value[length] != L'\0')
            {
                ++length;
            }
            return std::wstring(value, length);
        }

        case REG_MULTI_SZ:
        {
            const auto values = ParseWideMultiString(data.data(), data.size());
            return JoinLines(values);
        }

        case REG_DWORD:
        {
            const auto value = ReadScalar<std::uint32_t>(data);
            if (!value)
            {
                return BytesToHexString(data.data(), data.size());
            }

            std::wostringstream stream;
            stream << L"0x" << std::uppercase << std::hex << *value << L" (" << std::dec << *value << L')';
            return stream.str();
        }

        case REG_DWORD_BIG_ENDIAN:
        {
            const auto value = ReadScalar<std::uint32_t>(data);
            if (!value)
            {
                return BytesToHexString(data.data(), data.size());
            }

            const auto raw = *value;
            const std::uint32_t converted =
                ((raw & 0x000000FFU) << 24U) |
                ((raw & 0x0000FF00U) << 8U) |
                ((raw & 0x00FF0000U) >> 8U) |
                ((raw & 0xFF000000U) >> 24U);

            std::wostringstream stream;
            stream << L"0x" << std::uppercase << std::hex << converted << L" (" << std::dec << converted << L')';
            return stream.str();
        }

        case REG_QWORD:
        {
            const auto value = ReadScalar<std::uint64_t>(data);
            if (!value)
            {
                return BytesToHexString(data.data(), data.size());
            }

            std::wostringstream stream;
            stream << L"0x" << std::uppercase << std::hex << *value << L" (" << std::dec << *value << L')';
            return stream.str();
        }

        default:
            return data.empty() ? L"(empty)" : BytesToHexString(data.data(), data.size());
        }
    }

    std::wstring DevicePropertyName(const DEVPROPKEY& key)
    {
#define MATCH_PROPERTY(name) if (IsSamePropertyKey(key, name)) return WIDEN(#name)
        MATCH_PROPERTY(DEVPKEY_Device_DeviceDesc);
        MATCH_PROPERTY(DEVPKEY_Device_HardwareIds);
        MATCH_PROPERTY(DEVPKEY_Device_CompatibleIds);
        MATCH_PROPERTY(DEVPKEY_Device_Service);
        MATCH_PROPERTY(DEVPKEY_Device_Class);
        MATCH_PROPERTY(DEVPKEY_Device_ClassGuid);
        MATCH_PROPERTY(DEVPKEY_Device_Driver);
        MATCH_PROPERTY(DEVPKEY_Device_ConfigFlags);
        MATCH_PROPERTY(DEVPKEY_Device_Manufacturer);
        MATCH_PROPERTY(DEVPKEY_Device_FriendlyName);
        MATCH_PROPERTY(DEVPKEY_Device_LocationInfo);
        MATCH_PROPERTY(DEVPKEY_Device_PDOName);
        MATCH_PROPERTY(DEVPKEY_Device_Capabilities);
        MATCH_PROPERTY(DEVPKEY_Device_UINumber);
        MATCH_PROPERTY(DEVPKEY_Device_UpperFilters);
        MATCH_PROPERTY(DEVPKEY_Device_LowerFilters);
        MATCH_PROPERTY(DEVPKEY_Device_BusTypeGuid);
        MATCH_PROPERTY(DEVPKEY_Device_LegacyBusType);
        MATCH_PROPERTY(DEVPKEY_Device_BusNumber);
        MATCH_PROPERTY(DEVPKEY_Device_EnumeratorName);
        MATCH_PROPERTY(DEVPKEY_Device_Security);
        MATCH_PROPERTY(DEVPKEY_Device_SecuritySDS);
        MATCH_PROPERTY(DEVPKEY_Device_DevType);
        MATCH_PROPERTY(DEVPKEY_Device_Exclusive);
        MATCH_PROPERTY(DEVPKEY_Device_Characteristics);
        MATCH_PROPERTY(DEVPKEY_Device_Address);
        MATCH_PROPERTY(DEVPKEY_Device_UINumberDescFormat);
        MATCH_PROPERTY(DEVPKEY_Device_PowerData);
        MATCH_PROPERTY(DEVPKEY_Device_RemovalPolicy);
        MATCH_PROPERTY(DEVPKEY_Device_RemovalPolicyDefault);
        MATCH_PROPERTY(DEVPKEY_Device_RemovalPolicyOverride);
        MATCH_PROPERTY(DEVPKEY_Device_InstallState);
        MATCH_PROPERTY(DEVPKEY_Device_LocationPaths);
        MATCH_PROPERTY(DEVPKEY_Device_BaseContainerId);
        MATCH_PROPERTY(DEVPKEY_Device_ContainerId);
        MATCH_PROPERTY(DEVPKEY_Device_ProblemCode);
        MATCH_PROPERTY(DEVPKEY_Device_EjectionRelations);
        MATCH_PROPERTY(DEVPKEY_Device_RemovalRelations);
        MATCH_PROPERTY(DEVPKEY_Device_PowerRelations);
        MATCH_PROPERTY(DEVPKEY_Device_Children);
        MATCH_PROPERTY(DEVPKEY_Device_Parent);
        MATCH_PROPERTY(DEVPKEY_Device_Siblings);
        MATCH_PROPERTY(DEVPKEY_Device_TransportRelations);
#undef MATCH_PROPERTY

        std::wostringstream stream;
        stream << L"FMTID=" << GuidToString(key.fmtid) << L", PID=" << key.pid;
        return stream.str();
    }

    std::wstring DevicePropertyKeyToString(const DEVPROPKEY& key)
    {
        std::wostringstream stream;
        stream << DevicePropertyName(key) << L" [" << GuidToString(key.fmtid) << L"; " << key.pid << L']';
        return stream.str();
    }

    std::wstring DevicePropertyTypeToString(const DEVPROPTYPE type)
    {
        const DEVPROPTYPE baseType = type & DEVPROP_MASK_TYPE;
        const bool isArray = (type & DEVPROP_TYPEMOD_ARRAY) != 0;
        const wchar_t* name = L"Unknown";

        switch (baseType)
        {
        case DEVPROP_TYPE_EMPTY: name = L"Empty"; break;
        case DEVPROP_TYPE_NULL: name = L"Null"; break;
        case DEVPROP_TYPE_SBYTE: name = L"Int8"; break;
        case DEVPROP_TYPE_BYTE: name = L"UInt8"; break;
        case DEVPROP_TYPE_INT16: name = L"Int16"; break;
        case DEVPROP_TYPE_UINT16: name = L"UInt16"; break;
        case DEVPROP_TYPE_INT32: name = L"Int32"; break;
        case DEVPROP_TYPE_UINT32: name = L"UInt32"; break;
        case DEVPROP_TYPE_INT64: name = L"Int64"; break;
        case DEVPROP_TYPE_UINT64: name = L"UInt64"; break;
        case DEVPROP_TYPE_FLOAT: name = L"Float"; break;
        case DEVPROP_TYPE_DOUBLE: name = L"Double"; break;
        case DEVPROP_TYPE_DECIMAL: name = L"Decimal"; break;
        case DEVPROP_TYPE_GUID: name = L"Guid"; break;
        case DEVPROP_TYPE_CURRENCY: name = L"Currency"; break;
        case DEVPROP_TYPE_DATE: name = L"Date"; break;
        case DEVPROP_TYPE_FILETIME: name = L"FileTime"; break;
        case DEVPROP_TYPE_BOOLEAN: name = L"Boolean"; break;
        case DEVPROP_TYPE_STRING: name = L"String"; break;
        case DEVPROP_TYPE_STRING_LIST: name = L"StringList"; break;
        case DEVPROP_TYPE_SECURITY_DESCRIPTOR: name = L"SecurityDescriptor"; break;
        case DEVPROP_TYPE_SECURITY_DESCRIPTOR_STRING: name = L"SecurityDescriptorString"; break;
        case DEVPROP_TYPE_DEVPROPKEY: name = L"PropertyKey"; break;
        case DEVPROP_TYPE_DEVPROPTYPE: name = L"PropertyType"; break;
        case DEVPROP_TYPE_BINARY: name = L"Binary"; break;
        case DEVPROP_TYPE_ERROR: name = L"Error"; break;
        case DEVPROP_TYPE_NTSTATUS: name = L"NTSTATUS"; break;
        case DEVPROP_TYPE_STRING_INDIRECT: name = L"IndirectString"; break;
        default: break;
        }

        std::wstring result = name;
        if (isArray)
        {
            result += L"[]";
        }
        return result;
    }

    std::wstring FormatUnsigned(std::uint64_t value)
    {
        std::wostringstream stream;
        stream << L"0x" << std::uppercase << std::hex << value << L" (" << std::dec << value << L')';
        return stream.str();
    }

    std::wstring FormatSigned(std::int64_t value)
    {
        std::wostringstream stream;
        stream << value;
        return stream.str();
    }

    template <typename T>
    std::wstring FormatNumericArray(const std::vector<BYTE>& data)
    {
        if (data.empty() || data.size() % sizeof(T) != 0)
        {
            return BytesToHexString(data.data(), data.size());
        }

        const size_t itemCount = data.size() / sizeof(T);
        std::wostringstream stream;
        for (size_t index = 0; index < itemCount; ++index)
        {
            T value{};
            std::memcpy(&value, data.data() + (index * sizeof(T)), sizeof(T));
            if (index != 0)
            {
                stream << L"\n    - ";
            }
            stream << value;
        }
        return stream.str();
    }

    std::wstring FormatFileTime(const FILETIME& fileTime)
    {
        SYSTEMTIME systemTime{};
        if (!FileTimeToSystemTime(&fileTime, &systemTime))
        {
            return L"(invalid FILETIME)";
        }

        std::wostringstream stream;
        stream << std::setfill(L'0')
               << std::setw(4) << systemTime.wYear << L'-'
               << std::setw(2) << systemTime.wMonth << L'-'
               << std::setw(2) << systemTime.wDay << L' '
               << std::setw(2) << systemTime.wHour << L':'
               << std::setw(2) << systemTime.wMinute << L':'
               << std::setw(2) << systemTime.wSecond << L" UTC";
        return stream.str();
    }

    std::wstring FormatDevicePropertyValue(const DEVPROPTYPE type, const std::vector<BYTE>& data)
    {
        const DEVPROPTYPE baseType = type & DEVPROP_MASK_TYPE;
        const bool isArray = (type & DEVPROP_TYPEMOD_ARRAY) != 0;

        if (isArray)
        {
            switch (baseType)
            {
            case DEVPROP_TYPE_BYTE:
                return BytesToHexString(data.data(), data.size());
            case DEVPROP_TYPE_UINT16:
                return FormatNumericArray<std::uint16_t>(data);
            case DEVPROP_TYPE_UINT32:
                return FormatNumericArray<std::uint32_t>(data);
            case DEVPROP_TYPE_UINT64:
                return FormatNumericArray<std::uint64_t>(data);
            case DEVPROP_TYPE_INT16:
                return FormatNumericArray<std::int16_t>(data);
            case DEVPROP_TYPE_INT32:
                return FormatNumericArray<std::int32_t>(data);
            case DEVPROP_TYPE_INT64:
                return FormatNumericArray<std::int64_t>(data);
            default:
                return BytesToHexString(data.data(), data.size());
            }
        }

        switch (baseType)
        {
        case DEVPROP_TYPE_EMPTY:
        case DEVPROP_TYPE_NULL:
            return L"(empty)";

        case DEVPROP_TYPE_STRING:
        case DEVPROP_TYPE_SECURITY_DESCRIPTOR_STRING:
        case DEVPROP_TYPE_STRING_INDIRECT:
        {
            if (data.empty())
            {
                return L"";
            }

            const auto* value = reinterpret_cast<const wchar_t*>(data.data());
            const size_t charCount = data.size() / sizeof(wchar_t);
            size_t length = 0;
            while (length < charCount && value[length] != L'\0')
            {
                ++length;
            }
            return std::wstring(value, length);
        }

        case DEVPROP_TYPE_STRING_LIST:
        {
            const auto items = ParseWideMultiString(data.data(), data.size());
            return JoinLines(items);
        }

        case DEVPROP_TYPE_BOOLEAN:
            return (!data.empty() && data.front() != 0) ? L"true" : L"false";

        case DEVPROP_TYPE_BYTE:
        {
            const auto value = ReadScalar<std::uint8_t>(data);
            return value ? FormatUnsigned(*value) : BytesToHexString(data.data(), data.size());
        }

        case DEVPROP_TYPE_UINT16:
        {
            const auto value = ReadScalar<std::uint16_t>(data);
            return value ? FormatUnsigned(*value) : BytesToHexString(data.data(), data.size());
        }

        case DEVPROP_TYPE_UINT32:
        {
            const auto value = ReadScalar<std::uint32_t>(data);
            return value ? FormatUnsigned(*value) : BytesToHexString(data.data(), data.size());
        }

        case DEVPROP_TYPE_UINT64:
        {
            const auto value = ReadScalar<std::uint64_t>(data);
            return value ? FormatUnsigned(*value) : BytesToHexString(data.data(), data.size());
        }

        case DEVPROP_TYPE_SBYTE:
        {
            const auto value = ReadScalar<std::int8_t>(data);
            return value ? FormatSigned(*value) : BytesToHexString(data.data(), data.size());
        }

        case DEVPROP_TYPE_INT16:
        {
            const auto value = ReadScalar<std::int16_t>(data);
            return value ? FormatSigned(*value) : BytesToHexString(data.data(), data.size());
        }

        case DEVPROP_TYPE_INT32:
        {
            const auto value = ReadScalar<std::int32_t>(data);
            return value ? FormatSigned(*value) : BytesToHexString(data.data(), data.size());
        }

        case DEVPROP_TYPE_INT64:
        {
            const auto value = ReadScalar<std::int64_t>(data);
            return value ? FormatSigned(*value) : BytesToHexString(data.data(), data.size());
        }

        case DEVPROP_TYPE_GUID:
        {
            const auto value = ReadScalar<GUID>(data);
            return value ? GuidToString(*value) : BytesToHexString(data.data(), data.size());
        }

        case DEVPROP_TYPE_FILETIME:
        {
            const auto value = ReadScalar<FILETIME>(data);
            return value ? FormatFileTime(*value) : BytesToHexString(data.data(), data.size());
        }

        case DEVPROP_TYPE_DEVPROPKEY:
        {
            const auto value = ReadScalar<DEVPROPKEY>(data);
            return value ? DevicePropertyKeyToString(*value) : BytesToHexString(data.data(), data.size());
        }

        case DEVPROP_TYPE_DEVPROPTYPE:
        {
            const auto value = ReadScalar<DEVPROPTYPE>(data);
            return value ? DevicePropertyTypeToString(*value) : BytesToHexString(data.data(), data.size());
        }

        case DEVPROP_TYPE_ERROR:
        {
            const auto value = ReadScalar<ULONG>(data);
            if (!value)
            {
                return BytesToHexString(data.data(), data.size());
            }
            return L"Win32 error " + std::to_wstring(*value) + L": " + GetWin32ErrorMessage(*value);
        }

        case DEVPROP_TYPE_NTSTATUS:
        case DEVPROP_TYPE_BINARY:
        case DEVPROP_TYPE_SECURITY_DESCRIPTOR:
        default:
            return data.empty() ? L"(empty)" : BytesToHexString(data.data(), data.size());
        }
    }

    std::optional<RegistryTarget> ParseRegistryTarget(const int argc, wchar_t* argv[])
    {
        std::wstring rootName;
        std::wstring subkey;

        if (argc == 4)
        {
            rootName = argv[2];
            subkey = argv[3];
        }
        else if (argc == 3)
        {
            const std::wstring fullPath = argv[2];
            const size_t separator = fullPath.find(L'\\');
            if (separator == std::wstring::npos)
            {
                return std::nullopt;
            }

            rootName = fullPath.substr(0, separator);
            subkey = fullPath.substr(separator + 1);
        }
        else
        {
            return std::nullopt;
        }

        const std::wstring upperRoot = ToUpper(rootName);
        RegistryTarget target{};
        target.rootName = upperRoot;
        target.subkey = subkey;

        if (upperRoot == L"HKLM" || upperRoot == L"HKEY_LOCAL_MACHINE")
        {
            target.root = HKEY_LOCAL_MACHINE;
        }
        else if (upperRoot == L"HKCU" || upperRoot == L"HKEY_CURRENT_USER")
        {
            target.root = HKEY_CURRENT_USER;
        }
        else if (upperRoot == L"HKCR" || upperRoot == L"HKEY_CLASSES_ROOT")
        {
            target.root = HKEY_CLASSES_ROOT;
        }
        else if (upperRoot == L"HKU" || upperRoot == L"HKEY_USERS")
        {
            target.root = HKEY_USERS;
        }
        else if (upperRoot == L"HKCC" || upperRoot == L"HKEY_CURRENT_CONFIG")
        {
            target.root = HKEY_CURRENT_CONFIG;
        }
        else
        {
            return std::nullopt;
        }

        return target;
    }

    int DumpRegistrySubkeyValues(const RegistryTarget& target)
    {
        HKEY openedKey = nullptr;
        const LSTATUS openStatus = RegOpenKeyExW(target.root, target.subkey.c_str(), 0, KEY_READ, &openedKey);
        if (openStatus != ERROR_SUCCESS)
        {
            std::wcerr << L"Eroare la deschiderea subcheii de Registry: " << GetWin32ErrorMessage(openStatus) << L'\n';
            return 1;
        }

        RegistryKey key(openedKey);

        DWORD valueCount = 0;
        DWORD maxValueNameLength = 0;
        DWORD maxValueDataLength = 0;
        const LSTATUS queryStatus = RegQueryInfoKeyW(
            key.get(),
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            &valueCount,
            &maxValueNameLength,
            &maxValueDataLength,
            nullptr,
            nullptr);

        if (queryStatus != ERROR_SUCCESS)
        {
            std::wcerr << L"Eroare la interogarea subcheii de Registry: " << GetWin32ErrorMessage(queryStatus) << L'\n';
            return 1;
        }

        std::wcout << L"Subcheie: " << target.rootName << L'\\' << target.subkey << L'\n';
        std::wcout << L"Numar valori: " << valueCount << L"\n\n";

        if (valueCount == 0)
        {
            std::wcout << L"Subcheia nu contine valori.\n";
            return 0;
        }

        std::vector<wchar_t> valueName(maxValueNameLength + 1, L'\0');
        std::vector<BYTE> valueData(maxValueDataLength == 0 ? 1U : maxValueDataLength);

        for (DWORD index = 0; index < valueCount; ++index)
        {
            DWORD currentNameLength = maxValueNameLength + 1;
            DWORD currentDataLength = static_cast<DWORD>(valueData.size());
            DWORD valueType = 0;

            const LSTATUS enumStatus = RegEnumValueW(
                key.get(),
                index,
                valueName.data(),
                &currentNameLength,
                nullptr,
                &valueType,
                valueData.data(),
                &currentDataLength);

            if (enumStatus != ERROR_SUCCESS)
            {
                std::wcerr << L"Eroare la citirea valorii cu index " << index << L": " << GetWin32ErrorMessage(enumStatus) << L'\n';
                continue;
            }

            std::vector<BYTE> actualData(valueData.begin(), valueData.begin() + currentDataLength);
            const std::wstring name = (currentNameLength == 0) ? L"(Default)" : std::wstring(valueName.data(), currentNameLength);

            std::wcout << L'[' << index << L"] " << name << L'\n';
            std::wcout << L"  Tip    : " << RegistryTypeToString(valueType) << L'\n';
            std::wcout << L"  Valoare: " << FormatRegistryValueData(valueType, actualData) << L"\n\n";
        }

        return 0;
    }

    DeviceInfoSet CreatePresentDevicesSet()
    {
        return DeviceInfoSet(SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT));
    }

    bool TryGetDeviceInstanceId(const HDEVINFO deviceSet, SP_DEVINFO_DATA& deviceInfo, std::wstring& instanceId)
    {
        DWORD requiredSize = 0;
        std::vector<wchar_t> buffer(256, L'\0');

        if (!SetupDiGetDeviceInstanceIdW(deviceSet, &deviceInfo, buffer.data(), static_cast<DWORD>(buffer.size()), &requiredSize))
        {
            const DWORD error = GetLastError();
            if (error != ERROR_INSUFFICIENT_BUFFER)
            {
                return false;
            }

            buffer.assign(requiredSize, L'\0');
            if (!SetupDiGetDeviceInstanceIdW(deviceSet, &deviceInfo, buffer.data(), static_cast<DWORD>(buffer.size()), &requiredSize))
            {
                return false;
            }
        }

        instanceId.assign(buffer.data());
        return true;
    }

    std::optional<std::wstring> TryGetDevicePropertyString(const HDEVINFO deviceSet, SP_DEVINFO_DATA& deviceInfo, const DEVPROPKEY& key)
    {
        DEVPROPTYPE propertyType = 0;
        DWORD requiredSize = 0;

        if (!SetupDiGetDevicePropertyW(deviceSet, &deviceInfo, &key, &propertyType, nullptr, 0, &requiredSize, 0))
        {
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || requiredSize == 0)
            {
                return std::nullopt;
            }
        }

        std::vector<BYTE> buffer(requiredSize);
        if (!SetupDiGetDevicePropertyW(deviceSet, &deviceInfo, &key, &propertyType, buffer.data(), requiredSize, &requiredSize, 0))
        {
            return std::nullopt;
        }

        if (propertyType != DEVPROP_TYPE_STRING && propertyType != DEVPROP_TYPE_SECURITY_DESCRIPTOR_STRING && propertyType != DEVPROP_TYPE_STRING_INDIRECT)
        {
            return std::nullopt;
        }

        const auto* value = reinterpret_cast<const wchar_t*>(buffer.data());
        return std::wstring(value);
    }

    std::optional<std::wstring> TryGetRegistryPropertyString(const HDEVINFO deviceSet, SP_DEVINFO_DATA& deviceInfo, const DWORD property)
    {
        DWORD propertyType = 0;
        DWORD requiredSize = 0;
        std::vector<wchar_t> buffer(256, L'\0');

        if (!SetupDiGetDeviceRegistryPropertyW(
                deviceSet,
                &deviceInfo,
                property,
                &propertyType,
                reinterpret_cast<PBYTE>(buffer.data()),
                static_cast<DWORD>(buffer.size() * sizeof(wchar_t)),
                &requiredSize))
        {
            const DWORD error = GetLastError();
            if (error != ERROR_INSUFFICIENT_BUFFER)
            {
                return std::nullopt;
            }

            buffer.assign((requiredSize / sizeof(wchar_t)) + 1, L'\0');
            if (!SetupDiGetDeviceRegistryPropertyW(
                    deviceSet,
                    &deviceInfo,
                    property,
                    &propertyType,
                    reinterpret_cast<PBYTE>(buffer.data()),
                    static_cast<DWORD>(buffer.size() * sizeof(wchar_t)),
                    &requiredSize))
            {
                return std::nullopt;
            }
        }

        if (propertyType != REG_SZ && propertyType != REG_EXPAND_SZ)
        {
            return std::nullopt;
        }

        return std::wstring(buffer.data());
    }

    std::wstring GetBestDeviceDisplayName(const HDEVINFO deviceSet, SP_DEVINFO_DATA& deviceInfo)
    {
        if (const auto friendlyName = TryGetDevicePropertyString(deviceSet, deviceInfo, DEVPKEY_Device_FriendlyName))
        {
            return *friendlyName;
        }

        if (const auto description = TryGetDevicePropertyString(deviceSet, deviceInfo, DEVPKEY_Device_DeviceDesc))
        {
            return *description;
        }

        if (const auto friendlyName = TryGetRegistryPropertyString(deviceSet, deviceInfo, SPDRP_FRIENDLYNAME))
        {
            return *friendlyName;
        }

        if (const auto description = TryGetRegistryPropertyString(deviceSet, deviceInfo, SPDRP_DEVICEDESC))
        {
            return *description;
        }

        return L"(fara nume disponibil)";
    }

    std::optional<DeviceSelection> FindDeviceByIndex(const HDEVINFO deviceSet, const DWORD requestedIndex)
    {
        for (DWORD index = 0;; ++index)
        {
            SP_DEVINFO_DATA deviceInfo{};
            deviceInfo.cbSize = sizeof(deviceInfo);

            if (!SetupDiEnumDeviceInfo(deviceSet, index, &deviceInfo))
            {
                return std::nullopt;
            }

            if (index == requestedIndex)
            {
                return DeviceSelection{ index, deviceInfo };
            }
        }
    }

    std::optional<DeviceSelection> FindDeviceByInstanceId(const HDEVINFO deviceSet, const std::wstring& requestedInstanceId)
    {
        const std::wstring requestedUpper = ToUpper(requestedInstanceId);

        for (DWORD index = 0;; ++index)
        {
            SP_DEVINFO_DATA deviceInfo{};
            deviceInfo.cbSize = sizeof(deviceInfo);

            if (!SetupDiEnumDeviceInfo(deviceSet, index, &deviceInfo))
            {
                return std::nullopt;
            }

            std::wstring instanceId;
            if (TryGetDeviceInstanceId(deviceSet, deviceInfo, instanceId) && ToUpper(instanceId) == requestedUpper)
            {
                return DeviceSelection{ index, deviceInfo };
            }
        }
    }

    int ListConnectedDevices()
    {
        DeviceInfoSet deviceSet = CreatePresentDevicesSet();
        if (!deviceSet)
        {
            std::wcerr << L"Eroare la obtinerea listei de device-uri: " << GetWin32ErrorMessage(GetLastError()) << L'\n';
            return 1;
        }

        DWORD count = 0;
        for (DWORD index = 0;; ++index)
        {
            SP_DEVINFO_DATA deviceInfo{};
            deviceInfo.cbSize = sizeof(deviceInfo);

            if (!SetupDiEnumDeviceInfo(deviceSet.get(), index, &deviceInfo))
            {
                const DWORD error = GetLastError();
                if (error == ERROR_NO_MORE_ITEMS)
                {
                    break;
                }

                std::wcerr << L"Eroare la enumerarea device-urilor: " << GetWin32ErrorMessage(error) << L'\n';
                return 1;
            }

            std::wstring instanceId = L"(Instance ID indisponibil)";
            const bool hasInstanceId = TryGetDeviceInstanceId(deviceSet.get(), deviceInfo, instanceId);
            const std::wstring displayName = GetBestDeviceDisplayName(deviceSet.get(), deviceInfo);

            std::wcout << L'[' << index << L"] " << displayName << L'\n';
            std::wcout << L"    Instance ID: " << (hasInstanceId ? instanceId : L"(indisponibil)") << L"\n\n";
            ++count;
        }

        std::wcout << L"Total device-uri prezente: " << count << L'\n';
        return 0;
    }

    std::optional<std::vector<DEVPROPKEY>> GetDevicePropertyKeys(const HDEVINFO deviceSet, SP_DEVINFO_DATA& deviceInfo)
    {
        DWORD requiredCount = 0;
        if (!SetupDiGetDevicePropertyKeys(deviceSet, &deviceInfo, nullptr, 0, &requiredCount, 0))
        {
            const DWORD error = GetLastError();
            if (error != ERROR_INSUFFICIENT_BUFFER)
            {
                if (error == ERROR_NOT_FOUND)
                {
                    return std::vector<DEVPROPKEY>{};
                }
                return std::nullopt;
            }
        }

        std::vector<DEVPROPKEY> keys(requiredCount);
        if (requiredCount == 0)
        {
            return keys;
        }

        if (!SetupDiGetDevicePropertyKeys(deviceSet, &deviceInfo, keys.data(), requiredCount, &requiredCount, 0))
        {
            return std::nullopt;
        }

        return keys;
    }

    bool TryGetDevicePropertyValue(
        const HDEVINFO deviceSet,
        SP_DEVINFO_DATA& deviceInfo,
        const DEVPROPKEY& key,
        DEVPROPTYPE& propertyType,
        std::vector<BYTE>& data)
    {
        DWORD requiredSize = 0;
        if (!SetupDiGetDevicePropertyW(deviceSet, &deviceInfo, &key, &propertyType, nullptr, 0, &requiredSize, 0))
        {
            const DWORD error = GetLastError();
            if (error != ERROR_INSUFFICIENT_BUFFER)
            {
                return false;
            }
        }

        data.assign(requiredSize == 0 ? 1U : requiredSize, 0);
        return SetupDiGetDevicePropertyW(
                   deviceSet,
                   &deviceInfo,
                   &key,
                   &propertyType,
                   data.data(),
                   static_cast<DWORD>(data.size()),
                   &requiredSize,
                   0) != FALSE;
    }

    int ShowDeviceMetadataBySelection(const DeviceSelection& selection)
    {
        DeviceInfoSet deviceSet = CreatePresentDevicesSet();
        if (!deviceSet)
        {
            std::wcerr << L"Eroare la obtinerea listei de device-uri: " << GetWin32ErrorMessage(GetLastError()) << L'\n';
            return 1;
        }

        auto refreshed = FindDeviceByIndex(deviceSet.get(), selection.index);
        if (!refreshed)
        {
            std::wcerr << L"Device-ul selectat nu mai este prezent.\n";
            return 1;
        }

        SP_DEVINFO_DATA deviceInfo = refreshed->deviceInfo;
        std::wstring instanceId = L"(indisponibil)";
        TryGetDeviceInstanceId(deviceSet.get(), deviceInfo, instanceId);

        std::wcout << L"Device index : " << refreshed->index << L'\n';
        std::wcout << L"Nume         : " << GetBestDeviceDisplayName(deviceSet.get(), deviceInfo) << L'\n';
        std::wcout << L"Instance ID  : " << instanceId << L"\n\n";

        const auto keys = GetDevicePropertyKeys(deviceSet.get(), deviceInfo);
        if (!keys)
        {
            std::wcerr << L"Eroare la enumerarea metaparametrilor.\n";
            return 1;
        }

        std::wcout << L"Numar metaparametri: " << keys->size() << L"\n\n";

        for (const DEVPROPKEY& key : *keys)
        {
            DEVPROPTYPE propertyType = 0;
            std::vector<BYTE> data;

            if (!TryGetDevicePropertyValue(deviceSet.get(), deviceInfo, key, propertyType, data))
            {
                std::wcout << DevicePropertyKeyToString(key) << L'\n';
                std::wcout << L"  Tip    : (indisponibil)\n";
                std::wcout << L"  Valoare: Nu s-a putut citi.\n\n";
                continue;
            }

            std::wcout << DevicePropertyKeyToString(key) << L'\n';
            std::wcout << L"  Tip    : " << DevicePropertyTypeToString(propertyType) << L'\n';
            std::wcout << L"  Valoare: " << FormatDevicePropertyValue(propertyType, data) << L"\n\n";
        }

        return 0;
    }

    int PrintWin32Error(const std::wstring& context, const DWORD errorCode = GetLastError())
    {
        std::wcerr << context << L": " << GetWin32ErrorMessage(errorCode) << L'\n';
        return 1;
    }

    std::optional<std::wstring> GetCurrentExecutablePath()
    {
        std::vector<wchar_t> buffer(MAX_PATH, L'\0');

        for (;;)
        {
            const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0)
            {
                return std::nullopt;
            }

            if (length < buffer.size())
            {
                return std::wstring(buffer.data(), length);
            }

            if (buffer.size() >= 32768)
            {
                return std::nullopt;
            }

            buffer.assign(buffer.size() * 2, L'\0');
        }
    }

    std::wstring BuildServiceBinaryPath(const std::wstring& executablePath)
    {
        return L"\"" + executablePath + L"\" service run";
    }

    std::wstring ServiceStateToString(const DWORD state)
    {
        switch (state)
        {
        case SERVICE_STOPPED: return L"STOPPED";
        case SERVICE_START_PENDING: return L"START_PENDING";
        case SERVICE_STOP_PENDING: return L"STOP_PENDING";
        case SERVICE_RUNNING: return L"RUNNING";
        case SERVICE_CONTINUE_PENDING: return L"CONTINUE_PENDING";
        case SERVICE_PAUSE_PENDING: return L"PAUSE_PENDING";
        case SERVICE_PAUSED: return L"PAUSED";
        default: return L"UNKNOWN";
        }
    }

    bool QueryServiceStatusProcess(const SC_HANDLE service, SERVICE_STATUS_PROCESS& status)
    {
        DWORD bytesNeeded = 0;
        return QueryServiceStatusEx(
                   service,
                   SC_STATUS_PROCESS_INFO,
                   reinterpret_cast<LPBYTE>(&status),
                   sizeof(status),
                   &bytesNeeded) != FALSE;
    }

    bool WaitForServiceState(const SC_HANDLE service, const DWORD desiredState, const DWORD timeoutMs)
    {
        const ULONGLONG startTick = GetTickCount64();

        for (;;)
        {
            SERVICE_STATUS_PROCESS status{};
            if (!QueryServiceStatusProcess(service, status))
            {
                return false;
            }

            if (status.dwCurrentState == desiredState)
            {
                return true;
            }

            if (status.dwCurrentState == SERVICE_STOPPED && desiredState != SERVICE_STOPPED)
            {
                return false;
            }

            const ULONGLONG elapsedMs = GetTickCount64() - startTick;
            if (elapsedMs >= timeoutMs)
            {
                return false;
            }

            const DWORD suggestedWait = status.dwWaitHint == 0 ? 1000 : status.dwWaitHint / 10;
            const DWORD waitMs = std::clamp(suggestedWait, 250UL, 2000UL);
            Sleep(waitMs);
        }
    }

    bool TryShowHelloWorldMessageToActiveSession(const std::wstring& message)
    {
        const DWORD sessionId = WTSGetActiveConsoleSessionId();
        if (sessionId == 0xFFFFFFFF)
        {
            return false;
        }

        std::wstring title = kHelloWorldServiceDisplayName;
        DWORD response = 0;
        return WTSSendMessageW(
                   WTS_CURRENT_SERVER_HANDLE,
                   sessionId,
                   const_cast<LPWSTR>(title.c_str()),
                   static_cast<DWORD>(title.size() * sizeof(wchar_t)),
                   const_cast<LPWSTR>(message.c_str()),
                   static_cast<DWORD>(message.size() * sizeof(wchar_t)),
                   MB_OK | MB_ICONINFORMATION,
                   30,
                   &response,
                   FALSE) != FALSE;
    }

    void LogServiceEvent(const std::wstring& message, const WORD eventType = EVENTLOG_INFORMATION_TYPE)
    {
        HANDLE eventSource = RegisterEventSourceW(nullptr, kHelloWorldServiceName);
        if (eventSource == nullptr)
        {
            return;
        }

        LPCWSTR strings[] = { message.c_str() };
        ReportEventW(eventSource, eventType, 0, 1000, nullptr, 1, 0, strings, nullptr);
        DeregisterEventSource(eventSource);
    }

    void PublishHelloWorldInitializationMessage()
    {
        const std::wstring message = L"Hello World!";
        const std::wstring debugMessage = std::wstring(kHelloWorldServiceName) + L": " + message + L"\n";
        OutputDebugStringW(debugMessage.c_str());

        const bool shownToUser = TryShowHelloWorldMessageToActiveSession(message);
        if (shownToUser)
        {
            LogServiceEvent(L"Hello World! Mesajul a fost afisat in sesiunea activa.");
        }
        else
        {
            LogServiceEvent(L"Hello World! Nu exista o sesiune activa pentru afisare; mesajul a fost publicat in Event Log si debug output.");
        }
    }

    void ReportHelloWorldServiceStatus(const DWORD currentState, const DWORD win32ExitCode, const DWORD waitHint)
    {
        static DWORD checkPoint = 1;

        if (g_helloWorldServiceStatusHandle == nullptr)
        {
            return;
        }

        g_helloWorldServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        g_helloWorldServiceStatus.dwCurrentState = currentState;
        g_helloWorldServiceStatus.dwWin32ExitCode = win32ExitCode;
        g_helloWorldServiceStatus.dwWaitHint = waitHint;
        g_helloWorldServiceStatus.dwControlsAccepted =
            (currentState == SERVICE_RUNNING) ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN) : 0;

        if (currentState == SERVICE_RUNNING || currentState == SERVICE_STOPPED)
        {
            g_helloWorldServiceStatus.dwCheckPoint = 0;
            checkPoint = 1;
        }
        else
        {
            g_helloWorldServiceStatus.dwCheckPoint = checkPoint++;
        }

        SetServiceStatus(g_helloWorldServiceStatusHandle, &g_helloWorldServiceStatus);
    }

    DWORD WINAPI HelloWorldServiceControlHandler(
        const DWORD control,
        const DWORD eventType,
        LPVOID eventData,
        LPVOID context)
    {
        (void)eventType;
        (void)eventData;
        (void)context;

        switch (control)
        {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            ReportHelloWorldServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 3000);
            if (g_helloWorldServiceStopEvent != nullptr)
            {
                SetEvent(g_helloWorldServiceStopEvent);
            }
            return NO_ERROR;

        case SERVICE_CONTROL_INTERROGATE:
            SetServiceStatus(g_helloWorldServiceStatusHandle, &g_helloWorldServiceStatus);
            return NO_ERROR;

        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
        }
    }

    void WINAPI HelloWorldServiceMain(DWORD argc, LPWSTR* argv)
    {
        (void)argc;
        (void)argv;

        g_helloWorldServiceStatusHandle = RegisterServiceCtrlHandlerExW(
            kHelloWorldServiceName,
            HelloWorldServiceControlHandler,
            nullptr);

        if (g_helloWorldServiceStatusHandle == nullptr)
        {
            return;
        }

        ReportHelloWorldServiceStatus(SERVICE_START_PENDING, NO_ERROR, 3000);

        g_helloWorldServiceStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (g_helloWorldServiceStopEvent == nullptr)
        {
            ReportHelloWorldServiceStatus(SERVICE_STOPPED, GetLastError(), 0);
            return;
        }

        PublishHelloWorldInitializationMessage();
        ReportHelloWorldServiceStatus(SERVICE_RUNNING, NO_ERROR, 0);

        WaitForSingleObject(g_helloWorldServiceStopEvent, INFINITE);

        ReportHelloWorldServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 3000);
        CloseHandle(g_helloWorldServiceStopEvent);
        g_helloWorldServiceStopEvent = nullptr;
        ReportHelloWorldServiceStatus(SERVICE_STOPPED, NO_ERROR, 0);
    }

    int RunHelloWorldServiceDispatcher()
    {
        SERVICE_TABLE_ENTRYW serviceTable[] =
        {
            { const_cast<LPWSTR>(kHelloWorldServiceName), HelloWorldServiceMain },
            { nullptr, nullptr }
        };

        if (!StartServiceCtrlDispatcherW(serviceTable))
        {
            const DWORD error = GetLastError();
            if (error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
            {
                std::wcerr << L"Comanda 'service run' este destinata Service Control Manager. "
                              L"Foloseste 'service install', apoi 'service start'.\n";
                return 1;
            }

            return PrintWin32Error(L"Eroare la pornirea dispatcher-ului de service", error);
        }

        return 0;
    }

    int InstallHelloWorldService()
    {
        const auto executablePath = GetCurrentExecutablePath();
        if (!executablePath)
        {
            return PrintWin32Error(L"Nu pot determina calea executabilului");
        }

        ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE));
        if (!manager)
        {
            return PrintWin32Error(L"Eroare la deschiderea Service Control Manager");
        }

        const std::wstring binaryPath = BuildServiceBinaryPath(*executablePath);
        ServiceHandle service(CreateServiceW(
            manager.get(),
            kHelloWorldServiceName,
            kHelloWorldServiceDisplayName,
            SERVICE_QUERY_STATUS | SERVICE_START | SERVICE_STOP | DELETE,
            SERVICE_WIN32_OWN_PROCESS,
            SERVICE_DEMAND_START,
            SERVICE_ERROR_NORMAL,
            binaryPath.c_str(),
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr));

        if (!service)
        {
            const DWORD error = GetLastError();
            if (error == ERROR_SERVICE_EXISTS)
            {
                std::wcerr << L"Serviciul exista deja: " << kHelloWorldServiceName << L'\n';
                return 1;
            }

            return PrintWin32Error(L"Eroare la crearea serviciului", error);
        }

        std::wcout << L"Serviciu instalat: " << kHelloWorldServiceName << L'\n';
        std::wcout << L"Cale pornire: " << binaryPath << L'\n';
        return 0;
    }

    int StartHelloWorldService()
    {
        ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
        if (!manager)
        {
            return PrintWin32Error(L"Eroare la deschiderea Service Control Manager");
        }

        ServiceHandle service(OpenServiceW(manager.get(), kHelloWorldServiceName, SERVICE_START | SERVICE_QUERY_STATUS));
        if (!service)
        {
            return PrintWin32Error(L"Eroare la deschiderea serviciului");
        }

        if (!StartServiceW(service.get(), 0, nullptr))
        {
            const DWORD error = GetLastError();
            if (error == ERROR_SERVICE_ALREADY_RUNNING)
            {
                std::wcout << L"Serviciul ruleaza deja.\n";
                return 0;
            }

            return PrintWin32Error(L"Eroare la pornirea serviciului", error);
        }

        if (WaitForServiceState(service.get(), SERVICE_RUNNING, kServiceCommandTimeoutMs))
        {
            std::wcout << L"Serviciul ruleaza. Mesajul 'Hello World!' este trimis la initializare.\n";
            return 0;
        }

        std::wcerr << L"Serviciul a fost pornit, dar nu a ajuns in starea RUNNING in timpul asteptat.\n";
        return 1;
    }

    int StopHelloWorldService()
    {
        ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
        if (!manager)
        {
            return PrintWin32Error(L"Eroare la deschiderea Service Control Manager");
        }

        ServiceHandle service(OpenServiceW(manager.get(), kHelloWorldServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS));
        if (!service)
        {
            return PrintWin32Error(L"Eroare la deschiderea serviciului");
        }

        SERVICE_STATUS_PROCESS status{};
        if (!QueryServiceStatusProcess(service.get(), status))
        {
            return PrintWin32Error(L"Eroare la citirea starii serviciului");
        }

        if (status.dwCurrentState == SERVICE_STOPPED)
        {
            std::wcout << L"Serviciul este deja oprit.\n";
            return 0;
        }

        SERVICE_STATUS stopStatus{};
        if (!ControlService(service.get(), SERVICE_CONTROL_STOP, &stopStatus))
        {
            const DWORD error = GetLastError();
            if (error == ERROR_SERVICE_NOT_ACTIVE)
            {
                std::wcout << L"Serviciul este deja oprit.\n";
                return 0;
            }

            return PrintWin32Error(L"Eroare la oprirea serviciului", error);
        }

        if (WaitForServiceState(service.get(), SERVICE_STOPPED, kServiceCommandTimeoutMs))
        {
            std::wcout << L"Serviciul a fost oprit.\n";
            return 0;
        }

        std::wcerr << L"Serviciul nu a ajuns in starea STOPPED in timpul asteptat.\n";
        return 1;
    }

    int ShowHelloWorldServiceStatus()
    {
        ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
        if (!manager)
        {
            return PrintWin32Error(L"Eroare la deschiderea Service Control Manager");
        }

        ServiceHandle service(OpenServiceW(manager.get(), kHelloWorldServiceName, SERVICE_QUERY_STATUS));
        if (!service)
        {
            return PrintWin32Error(L"Eroare la deschiderea serviciului");
        }

        SERVICE_STATUS_PROCESS status{};
        if (!QueryServiceStatusProcess(service.get(), status))
        {
            return PrintWin32Error(L"Eroare la citirea starii serviciului");
        }

        std::wcout << L"Serviciu: " << kHelloWorldServiceName << L'\n';
        std::wcout << L"Stare   : " << ServiceStateToString(status.dwCurrentState) << L'\n';
        return 0;
    }

    int UninstallHelloWorldService()
    {
        ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
        if (!manager)
        {
            return PrintWin32Error(L"Eroare la deschiderea Service Control Manager");
        }

        ServiceHandle service(OpenServiceW(
            manager.get(),
            kHelloWorldServiceName,
            DELETE | SERVICE_STOP | SERVICE_QUERY_STATUS));

        if (!service)
        {
            return PrintWin32Error(L"Eroare la deschiderea serviciului");
        }

        SERVICE_STATUS_PROCESS status{};
        if (QueryServiceStatusProcess(service.get(), status) && status.dwCurrentState != SERVICE_STOPPED)
        {
            SERVICE_STATUS stopStatus{};
            if (ControlService(service.get(), SERVICE_CONTROL_STOP, &stopStatus))
            {
                WaitForServiceState(service.get(), SERVICE_STOPPED, kServiceCommandTimeoutMs);
            }
        }

        if (!DeleteService(service.get()))
        {
            return PrintWin32Error(L"Eroare la stergerea serviciului");
        }

        std::wcout << L"Serviciul a fost dezinstalat: " << kHelloWorldServiceName << L'\n';
        return 0;
    }

    void PrintServiceUsage()
    {
        std::wcout
            << L"Comenzi service:\n"
            << L"  PDApp service install\n"
            << L"  PDApp service start\n"
            << L"  PDApp service status\n"
            << L"  PDApp service stop\n"
            << L"  PDApp service uninstall\n";
    }

    int HandleHelloWorldServiceCommand(const int argc, wchar_t* argv[])
    {
        if (argc != 3)
        {
            PrintServiceUsage();
            return 1;
        }

        const std::wstring action = ToUpper(argv[2]);

        if (action == L"INSTALL")
        {
            return InstallHelloWorldService();
        }

        if (action == L"START")
        {
            return StartHelloWorldService();
        }

        if (action == L"STATUS")
        {
            return ShowHelloWorldServiceStatus();
        }

        if (action == L"STOP")
        {
            return StopHelloWorldService();
        }

        if (action == L"UNINSTALL" || action == L"DELETE")
        {
            return UninstallHelloWorldService();
        }

        if (action == L"RUN")
        {
            return RunHelloWorldServiceDispatcher();
        }

        PrintServiceUsage();
        return 1;
    }

    void PrintUsage()
    {
        std::wcout
            << L"Utilizare:\n"
            << L"  PDApp registry <HKLM|HKCU|HKCR|HKU|HKCC> <subcheie>\n"
            << L"  PDApp registry <cale-completa-registry>\n"
            << L"  PDApp devices\n"
            << L"  PDApp device <index>\n"
            << L"  PDApp device-id <instance-id>\n"
            << L"  PDApp service <install|start|status|stop|uninstall>\n\n"
            << L"Exemple:\n"
            << L"  PDApp registry HKLM \"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\"\n"
            << L"  PDApp devices\n"
            << L"  PDApp device 0\n"
            << L"  PDApp service install\n"
            << L"  PDApp service start\n";
    }
}

int wmain(const int argc, wchar_t* argv[])
{
    if (argc < 2)
    {
        PrintUsage();
        return 1;
    }

    const std::wstring command = ToUpper(argv[1]);

    if (command == L"REGISTRY")
    {
        const auto target = ParseRegistryTarget(argc, argv);
        if (!target)
        {
            PrintUsage();
            return 1;
        }

        return DumpRegistrySubkeyValues(*target);
    }

    if (command == L"DEVICES")
    {
        return ListConnectedDevices();
    }

    if (command == L"DEVICE")
    {
        if (argc != 3)
        {
            PrintUsage();
            return 1;
        }

        unsigned long index = 0;
        try
        {
            index = std::stoul(argv[2]);
        }
        catch (...)
        {
            std::wcerr << L"Index de device invalid.\n";
            return 1;
        }

        DeviceInfoSet deviceSet = CreatePresentDevicesSet();
        if (!deviceSet)
        {
            std::wcerr << L"Eroare la obtinerea listei de device-uri: " << GetWin32ErrorMessage(GetLastError()) << L'\n';
            return 1;
        }

        const auto selection = FindDeviceByIndex(deviceSet.get(), static_cast<DWORD>(index));
        if (!selection)
        {
            std::wcerr << L"Nu exista un device prezent cu indexul " << index << L".\n";
            return 1;
        }

        return ShowDeviceMetadataBySelection(*selection);
    }

    if (command == L"DEVICE-ID")
    {
        if (argc != 3)
        {
            PrintUsage();
            return 1;
        }

        DeviceInfoSet deviceSet = CreatePresentDevicesSet();
        if (!deviceSet)
        {
            std::wcerr << L"Eroare la obtinerea listei de device-uri: " << GetWin32ErrorMessage(GetLastError()) << L'\n';
            return 1;
        }

        const auto selection = FindDeviceByInstanceId(deviceSet.get(), argv[2]);
        if (!selection)
        {
            std::wcerr << L"Nu am gasit niciun device prezent cu Instance ID-ul indicat.\n";
            return 1;
        }

        return ShowDeviceMetadataBySelection(*selection);
    }

    if (command == L"SERVICE")
    {
        return HandleHelloWorldServiceCommand(argc, argv);
    }

    PrintUsage();
    return 1;
}
