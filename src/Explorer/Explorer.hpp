#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <propkey.h>
#include <commctrl.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

inline std::atomic<long> moduleReferences{};

class ComObject {
    std::atomic<ULONG> references{1};
protected:
    ComObject() { ++moduleReferences; }
    virtual ~ComObject() { --moduleReferences; }
    ULONG retain() noexcept { return ++references; }
    ULONG release() noexcept {
        auto remaining = --references;
        if (!remaining) delete this;
        return remaining;
    }
};

#define COM_REFCOUNT \
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return retain(); } \
    ULONG STDMETHODCALLTYPE Release() noexcept override { return release(); }

template<class Operation>
HRESULT guarded(Operation operation) noexcept {
    try { return operation(); }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}

struct PidlFree { void operator()(ITEMIDLIST* value) const noexcept { CoTaskMemFree(value); } };
using Pidl = std::unique_ptr<ITEMIDLIST, PidlFree>;
struct Item {
    BYTE flags{};
    int64_t size{-1};
    uint64_t modified{};
    std::wstring id, name, copyName;
    bool folder() const { return (flags & 1) != 0; }
    bool copyable() const { return (flags & 2) != 0; }
};

class Pipe {
    HANDLE handle = INVALID_HANDLE_VALUE;
    HANDLE event = nullptr;
    bool transfer(void* buffer, DWORD length, bool reading);
public:
    Pipe() = default;
    Pipe(const Pipe&) = delete;
    ~Pipe() { close(); }
    bool open(REFGUID account, BYTE command, std::wstring_view id, uint64_t offset = 0);
    void close() noexcept;
    bool read(void* buffer, DWORD length) { return transfer(buffer, length, true); }
    template<class T> bool read(T& value) { return read(&value, sizeof(value)); }
    bool text(std::wstring& value);
};

HRESULT list(REFGUID account, std::wstring_view parent, std::vector<Pidl>& output);
Pidl makePidl(const Item& item);
bool decode(PCUIDLIST_RELATIVE pidl, Item& item);
HRESULT makeDataObject(REFGUID account, std::vector<Item> items, REFIID iid, void** out);
HRESULT makeShellDataObject(REFGUID account, std::vector<Item> items, PCIDLIST_ABSOLUTE parent,
    UINT count, PCUITEMID_CHILD_ARRAY children, REFIID iid, void** out);
HRESULT makeStream(REFGUID account, const Item& item, REFIID iid, void** out);
HRESULT makeMenu(REFGUID account, std::vector<Item> items, PCIDLIST_ABSOLUTE parent,
    UINT count, PCUITEMID_CHILD_ARRAY children, REFIID iid, void** out);
