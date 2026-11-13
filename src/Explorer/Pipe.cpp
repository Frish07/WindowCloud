#include "Explorer.hpp"

namespace {
constexpr DWORD timeoutMs = 35000;

bool pipePath(REFGUID account, wchar_t (&path)[100]) {
    wchar_t key[] = L"Software\\Classes\\CLSID\\{00000000-0000-0000-0000-000000000000}";
    if (!StringFromGUID2(account, key + ARRAYSIZE(key) - 39, 39)) return false;
    wchar_t name[64]{};
    DWORD bytes = sizeof(name);
    if (RegGetValueW(HKEY_CURRENT_USER, key, L"WindowCloud.Account", RRF_RT_REG_SZ,
        nullptr, name, &bytes) != ERROR_SUCCESS) return false;
    constexpr std::wstring_view prefix = L"WindowCloud-";
    std::wstring_view text(name);
    if (text.size() != prefix.size() + 32 || !text.starts_with(prefix) ||
        text.find_first_not_of(L"0123456789abcdef", prefix.size()) != std::wstring_view::npos) return false;
    return swprintf_s(path, L"\\\\.\\pipe\\%ls", name) > 0;
}

template<class T>
void append(BYTE*& position, const T& value) {
    std::memcpy(position, &value, sizeof(T));
    position += sizeof(T);
}

struct Reader {
    const BYTE* current;
    const BYTE* end;
    template<class T> bool value(T& value) {
        if (end - current < sizeof(T)) return false;
        std::memcpy(&value, current, sizeof(T));
        current += sizeof(T);
        return true;
    }
    bool text(std::wstring& value) {
        uint16_t bytes{};
        if (!this->value(bytes) || bytes % sizeof(wchar_t) || end - current < bytes) return false;
        value.resize(bytes / sizeof(wchar_t));
        std::memcpy(value.data(), current, bytes);
        current += bytes;
        return value.find(L'\0') == std::wstring::npos;
    }
};
}

bool Pipe::open(REFGUID account, BYTE command, std::wstring_view id, uint64_t offset) {
    close();
    if (id.size() > 2048) return false;
    wchar_t path[100]{};
    if (!pipePath(account, path)) return false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        handle = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (handle != INVALID_HANDLE_VALUE) break;
        DWORD error = GetLastError();
        if (error == ERROR_PIPE_BUSY) WaitNamedPipeW(path, 100);
        else if (error == ERROR_FILE_NOT_FOUND) Sleep(25);
        else return false;
    }
    if (handle == INVALID_HANDLE_VALUE) return false;
    event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) { close(); return false; }
    std::array<BYTE, 29> header{};
    BYTE* position = header.data();
    auto length = static_cast<uint32_t>(id.size() * sizeof(wchar_t));
    append(position, command);
    append(position, account);
    append(position, length);
    append(position, offset);
    BYTE status = 1;
    if (!transfer(header.data(), static_cast<DWORD>(header.size()), false) ||
        !transfer(const_cast<wchar_t*>(id.data()), length, false) || !read(status) || status != 0) {
        close();
        return false;
    }
    return true;
}

void Pipe::close() noexcept {
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(std::exchange(handle, INVALID_HANDLE_VALUE));
    if (event) CloseHandle(std::exchange(event, nullptr));
}

bool Pipe::transfer(void* buffer, DWORD length, bool reading) {
    auto bytes = static_cast<BYTE*>(buffer);
    while (length) {
        OVERLAPPED operation{};
        operation.hEvent = event;
        ResetEvent(event);
        DWORD done = 0;
        BOOL started = reading ? ReadFile(handle, bytes, length, &done, &operation)
            : WriteFile(handle, bytes, length, &done, &operation);
        if (!started) {
            if (GetLastError() != ERROR_IO_PENDING) return false;
            if (WaitForSingleObject(event, timeoutMs) != WAIT_OBJECT_0) {
                CancelIoEx(handle, &operation);
                GetOverlappedResult(handle, &operation, &done, TRUE);
                return false;
            }
            if (!GetOverlappedResult(handle, &operation, &done, FALSE)) return false;
        }
        if (!done) return false;
        bytes += done;
        length -= done;
    }
    return true;
}

bool Pipe::text(std::wstring& value) {
    uint16_t bytes{};
    if (!read(bytes) || bytes % sizeof(wchar_t)) return false;
    value.resize(bytes / sizeof(wchar_t));
    return read(value.data(), bytes) && value.find(L'\0') == std::wstring::npos;
}

HRESULT list(REFGUID account, std::wstring_view parent, std::vector<Pidl>& output) {
    Pipe pipe;
    uint32_t count{};
    if (!pipe.open(account, 1, parent) || !pipe.read(count)) return HRESULT_FROM_WIN32(ERROR_NETWORK_UNREACHABLE);
    for (uint32_t i = 0; i < count; ++i) {
        Item item;
        if (!pipe.read(item.flags) || !pipe.read(item.size) || !pipe.read(item.modified) ||
            !pipe.text(item.id) || !pipe.text(item.name) || !pipe.text(item.copyName)) return E_FAIL;
        auto pidl = makePidl(item);
        if (!pidl) return E_OUTOFMEMORY;
        output.push_back(std::move(pidl));
    }
    return S_OK;
}

Pidl makePidl(const Item& item) {
    size_t size = 2 + 4 + 1 + 8 + 8 + 6 +
        sizeof(wchar_t) * (item.id.size() + item.name.size() + item.copyName.size());
    if (size > USHRT_MAX) return {};
    Pidl pidl(static_cast<ITEMIDLIST*>(CoTaskMemAlloc(size + 2)));
    if (!pidl) return {};
    auto position = reinterpret_cast<BYTE*>(pidl.get());
    append(position, static_cast<uint16_t>(size));
    append(position, uint32_t{0x57434C31});
    append(position, item.flags);
    append(position, item.size);
    append(position, item.modified);
    for (const auto* text : {&item.id, &item.name, &item.copyName}) {
        auto bytes = static_cast<uint16_t>(text->size() * sizeof(wchar_t));
        append(position, bytes);
        std::memcpy(position, text->data(), bytes);
        position += bytes;
    }
    append(position, uint16_t{});
    return pidl;
}

bool decode(PCUIDLIST_RELATIVE pidl, Item& item) {
    if (!pidl || pidl->mkid.cb < 29) return false;
    auto begin = reinterpret_cast<const BYTE*>(pidl);
    Reader reader{begin + 2, begin + pidl->mkid.cb};
    uint32_t magic{};
    return reader.value(magic) && magic == 0x57434C31 &&
        reader.value(item.flags) && reader.value(item.size) && reader.value(item.modified) &&
        reader.text(item.id) && reader.text(item.name) && reader.text(item.copyName) &&
        !item.id.empty() && reader.current == reader.end;
}
