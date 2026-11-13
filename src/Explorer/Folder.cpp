#include "Explorer.hpp"

namespace {
constexpr DWORD folderFlags = SFGAO_FOLDER | SFGAO_BROWSABLE | SFGAO_HASSUBFOLDER | SFGAO_READONLY;
constexpr DWORD fileFlags = SFGAO_READONLY;

DWORD attributes(const Item& item) {
    return item.folder() ? folderFlags : fileFlags | (item.copyable() ? SFGAO_CANCOPY | SFGAO_STREAM : 0);
}

HRESULT display(STRRET* out, const wchar_t* value) {
    if (!out) return E_POINTER;
    out->uType = STRRET_WSTR;
    return SHStrDupW(value, &out->pOleStr);
}

struct Snapshot {
    GUID account;
    std::wstring parent;
    std::vector<Pidl> items;
    HRESULT state = E_PENDING;
    Snapshot(REFGUID account, std::wstring parent) : account(account), parent(std::move(parent)) {}
    HRESULT load() {
        if (state == E_PENDING) state = list(account, parent, items);
        return state;
    }
};

class Enumerator final : public IEnumIDList, public ComObject {
    std::shared_ptr<Snapshot> data;
    SHCONTF flags;
    size_t index{};
    bool included(PCUIDLIST_RELATIVE pidl) const {
        // The item flags immediately follow the length and signature in the PIDL.
        const auto isFolder = (reinterpret_cast<const BYTE*>(pidl)[6] & 1) != 0;
        return (flags & (isFolder ? SHCONTF_FOLDERS : SHCONTF_NONFOLDERS)) != 0;
    }
public:
    Enumerator(std::shared_ptr<Snapshot> data, SHCONTF flags) : data(std::move(data)), flags(flags) {}
    COM_REFCOUNT
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IEnumIDList) return E_NOINTERFACE;
        *out = static_cast<IEnumIDList*>(this);
        AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Next(ULONG count, PITEMID_CHILD* out, ULONG* fetched) override {
        if (fetched) *fetched = 0;
        if (!out || (count != 1 && !fetched)) return E_POINTER;
        return guarded([&] {
            auto hr = data->load();
            if (FAILED(hr)) return hr;
            ULONG copied = 0;
            while (copied < count && index < data->items.size()) {
                auto pidl = data->items[index++].get();
                if (!included(pidl)) continue;
                auto clone = ILCloneFirst(pidl);
                if (!clone) {
                    while (copied) CoTaskMemFree(out[--copied]);
                    return E_OUTOFMEMORY;
                }
                out[copied++] = clone;
            }
            if (fetched) *fetched = copied;
            return copied == count ? S_OK : S_FALSE;
        });
    }
    HRESULT STDMETHODCALLTYPE Skip(ULONG count) override {
        return guarded([&] {
            auto hr = data->load();
            if (FAILED(hr)) return hr;
            while (count && index < data->items.size()) if (included(data->items[index++].get())) --count;
            return count ? S_FALSE : S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE Reset() override { index = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE Clone(IEnumIDList** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        return guarded([&] {
            auto clone = new Enumerator(data, flags);
            clone->index = index;
            *out = clone;
            return S_OK;
        });
    }
};

const PROPERTYKEY* const properties[] = {&PKEY_ItemNameDisplay, &PKEY_Size, &PKEY_ItemTypeText, &PKEY_DateModified};
constexpr const wchar_t* columns[] = {L"Имя", L"Размер", L"Тип", L"Дата изменения"};

const wchar_t* type(const Item& item) {
    if (item.folder()) return L"Папка";
    return L"Облачный файл";
}

HRESULT icon(const Item& item, REFIID iid, void** out) {
    IDefaultExtractIconInit* initialization = nullptr;
    auto hr = SHCreateDefaultExtractIcon(IID_IDefaultExtractIconInit, reinterpret_cast<void**>(&initialization));
    if (FAILED(hr)) return hr;
    wchar_t path[MAX_PATH]{};
    const auto length = GetSystemDirectoryW(path, ARRAYSIZE(path));
    if (!length || length >= ARRAYSIZE(path) || wcscat_s(path, L"\\imageres.dll")) hr = E_FAIL;
    else hr = initialization->SetNormalIcon(path, item.folder() ? -3 : -2);
    if (SUCCEEDED(hr)) hr = initialization->QueryInterface(iid, out);
    initialization->Release();
    return hr;
}

class ViewCallback final : public IShellFolderViewCB, public ComObject {
    Pidl location;
public:
    explicit ViewCallback(Pidl location) : location(std::move(location)) {}
    COM_REFCOUNT
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IShellFolderViewCB) return E_NOINTERFACE;
        *out = static_cast<IShellFolderViewCB*>(this);
        AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE MessageSFVCB(UINT message, WPARAM w, LPARAM l) noexcept override {
        switch (message) {
        case SFVM_BACKGROUNDENUM: return S_OK;
        case SFVM_DEFVIEWMODE:
            *reinterpret_cast<FOLDERVIEWMODE*>(l) = FVM_DETAILS;
            return S_OK;
        case SFVM_GETNOTIFY:
            *reinterpret_cast<PCIDLIST_ABSOLUTE*>(w) = location.get();
            *reinterpret_cast<LONG*>(l) = SHCNE_UPDATEDIR | SHCNE_RMDIR;
            return location ? S_OK : E_NOTIMPL;
        default: return E_NOTIMPL;
        }
    }
};

class Folder final : public IShellFolder2, public IPersistFolder2, public ComObject {
    GUID account;
    std::wstring parent;
    Pidl absolute;
public:
    explicit Folder(REFGUID account) : account(account) {}
    COM_REFCOUNT
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IShellFolder || iid == IID_IShellFolder2)
            *out = static_cast<IShellFolder2*>(this);
        else if (iid == IID_IPersist || iid == IID_IPersistFolder || iid == IID_IPersistFolder2)
            *out = static_cast<IPersistFolder2*>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetClassID(CLSID* out) override { if (!out) return E_POINTER; *out = account; return S_OK; }
    HRESULT STDMETHODCALLTYPE Initialize(PCIDLIST_ABSOLUTE pidl) override {
        if (!pidl) return E_INVALIDARG;
        absolute.reset(ILCloneFull(pidl));
        return absolute ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetCurFolder(PIDLIST_ABSOLUTE* out) override {
        if (!out) return E_POINTER;
        *out = absolute ? ILCloneFull(absolute.get()) : nullptr;
        return !absolute || *out ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE ParseDisplayName(HWND hwnd, IBindCtx* context, LPWSTR text, ULONG* eaten,
        PIDLIST_RELATIVE* out, ULONG* requested) override {
        if (!out || !text) return E_POINTER;
        *out = nullptr;
        if (eaten) *eaten = 0;
        return guarded([&] {
            const std::wstring_view input(text);
            const auto slash = input.find(L'\\');
            const auto name = input.substr(0, slash);
            std::vector<Pidl> entries;
            auto hr = list(account, parent, entries);
            if (FAILED(hr)) return hr;
            for (const auto& entry : entries) {
                Item item;
                if (!decode(entry.get(), item)) continue;
                if (parsingName(item.id) != name && item.name != name) continue;
                if (slash == std::wstring_view::npos) {
                    *out = ILClone(entry.get());
                    if (!*out) return E_OUTOFMEMORY;
                    if (requested) *requested &= attributes(item);
                    if (eaten) *eaten = static_cast<ULONG>(input.size());
                    return S_OK;
                }
                IShellFolder* child = nullptr;
                hr = BindToObject(entry.get(), context, IID_IShellFolder, reinterpret_cast<void**>(&child));
                if (FAILED(hr)) return hr;
                PIDLIST_RELATIVE tail = nullptr;
                hr = child->ParseDisplayName(hwnd, context, text + slash + 1, eaten, &tail, requested);
                child->Release();
                Pidl relative(tail);
                if (FAILED(hr)) return hr;
                *out = ILCombine(entry.get(), relative.get());
                if (!*out) return E_OUTOFMEMORY;
                if (eaten) *eaten += static_cast<ULONG>(slash + 1);
                return S_OK;
            }
            return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        });
    }
    HRESULT STDMETHODCALLTYPE EnumObjects(HWND, SHCONTF flags, IEnumIDList** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        return guarded([&] {
            *out = new Enumerator(std::make_shared<Snapshot>(account, parent), flags);
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE BindToObject(PCUIDLIST_RELATIVE pidl, IBindCtx* context, REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        return guarded([&] {
            Item item;
            if (!decode(pidl, item) || !item.folder()) return E_NOINTERFACE;
            Pidl first(ILCloneFirst(pidl));
            if (!first) return E_OUTOFMEMORY;
            Pidl location(absolute ? ILCombine(absolute.get(), first.get()) : nullptr);
            if (absolute && !location) return E_OUTOFMEMORY;
            auto folder = new Folder(account);
            folder->parent = std::move(item.id);
            folder->absolute = std::move(location);
            auto next = ILNext(pidl);
            auto hr = next->mkid.cb ? folder->BindToObject(next, context, iid, out) : folder->QueryInterface(iid, out);
            folder->Release();
            return hr;
        });
    }
    HRESULT STDMETHODCALLTYPE BindToStorage(PCUIDLIST_RELATIVE pidl, IBindCtx*, REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        return guarded([&] {
            Item item;
            if (!decode(pidl, item) || ILNext(pidl)->mkid.cb) return E_NOINTERFACE;
            return makeStream(account, item, iid, out);
        });
    }
    HRESULT STDMETHODCALLTYPE CompareIDs(LPARAM parameters, PCUIDLIST_RELATIVE a, PCUIDLIST_RELATIVE b) override {
        return guarded([&] {
            Item left, right;
            if (!decode(a, left) || !decode(b, right)) return E_INVALIDARG;
            const auto compare = [](const auto& x, const auto& y) { return x < y ? -1 : x > y ? 1 : 0; };
            int order = 0;
            if (!(parameters & SHCIDS_CANONICALONLY)) {
                order = compare(right.folder(), left.folder());
                if (!order) {
                    switch (LOWORD(parameters)) {
                    case 1: order = compare(left.size, right.size); break;
                    case 2: order = StrCmpLogicalW(type(left), type(right)); break;
                    case 3: order = compare(left.modified, right.modified); break;
                    }
                }
                if (!order) order = StrCmpLogicalW(left.name.c_str(), right.name.c_str());
            }
            if (!order) order = left.id.compare(right.id);
            if (!order) {
                auto nextA = ILNext(a);
                auto nextB = ILNext(b);
                if (nextA->mkid.cb && nextB->mkid.cb) return CompareIDs(parameters, nextA, nextB);
                order = compare(nextA->mkid.cb != 0, nextB->mkid.cb != 0);
            }
            return MAKE_HRESULT(SEVERITY_SUCCESS, 0, static_cast<USHORT>(compare(order, 0)));
        });
    }
    HRESULT STDMETHODCALLTYPE CreateViewObject(HWND, REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IShellView) return E_NOINTERFACE;
        return guarded([&] {
            Pidl location(absolute ? ILCloneFull(absolute.get()) : nullptr);
            if (absolute && !location) return E_OUTOFMEMORY;
            auto callback = new ViewCallback(std::move(location));
            SFV_CREATE creation{sizeof(creation), this, nullptr, callback};
            auto hr = SHCreateShellFolderView(&creation, reinterpret_cast<IShellView**>(out));
            callback->Release();
            return hr;
        });
    }
    HRESULT STDMETHODCALLTYPE GetAttributesOf(UINT count, PCUITEMID_CHILD_ARRAY pidls, SFGAOF* flags) override {
        if (!flags || (count && !pidls)) return E_POINTER;
        return guarded([&] {
            if (!count) *flags &= folderFlags;
            for (UINT i = 0; i < count; ++i) {
                Item item;
                if (!decode(pidls[i], item)) return E_INVALIDARG;
                *flags &= attributes(item);
            }
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE GetUIObjectOf(HWND, UINT count, PCUITEMID_CHILD_ARRAY pidls,
        REFIID iid, UINT*, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!count || !pidls || (iid != IID_IDataObject && iid != IID_IContextMenu &&
            iid != IID_IExtractIconW && iid != IID_IExtractIconA)) return E_NOINTERFACE;
        return guarded([&] {
            std::vector<Item> items(count);
            for (UINT i = 0; i < count; ++i) if (!decode(pidls[i], items[i])) return E_INVALIDARG;
            if (iid == IID_IDataObject)
                return makeShellDataObject(account, std::move(items), absolute.get(), count, pidls, iid, out);
            if (iid == IID_IContextMenu) return makeMenu(account, std::move(items), absolute.get(), count, pidls, iid, out);
            return count == 1 ? icon(items.front(), iid, out) : E_NOINTERFACE;
        });
    }
    HRESULT STDMETHODCALLTYPE GetDisplayNameOf(PCUITEMID_CHILD pidl, SHGDNF flags, STRRET* out) override {
        return guarded([&] {
            Item item;
            if (!decode(pidl, item)) return E_INVALIDARG;
            if (!(flags & SHGDN_FORPARSING)) return display(out, item.name.c_str());
            std::wstring path;
            if (!(flags & SHGDN_INFOLDER)) {
                wchar_t clsid[39]{};
                if (!StringFromGUID2(account, clsid, ARRAYSIZE(clsid))) return E_FAIL;
                path = L"::";
                path += clsid;
                if (absolute) {
                    for (auto next = reinterpret_cast<PCUIDLIST_RELATIVE>(absolute.get()); next->mkid.cb; next = ILNext(next)) {
                        Item ancestor;
                        if (decode(next, ancestor)) path += L"\\" + parsingName(ancestor.id);
                    }
                }
                path += L"\\";
            }
            path += parsingName(item.id);
            return display(out, path.c_str());
        });
    }
    HRESULT STDMETHODCALLTYPE SetNameOf(HWND, PCUITEMID_CHILD, LPCWSTR, SHGDNF, PITEMID_CHILD*) override { return E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE GetDefaultSearchGUID(GUID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumSearches(IEnumExtraSearch**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDefaultColumn(DWORD, ULONG* sort, ULONG* display) override {
        if (!sort || !display) return E_POINTER;
        *sort = *display = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDefaultColumnState(UINT column, SHCOLSTATEF* out) override {
        if (!out) return E_POINTER;
        if (column >= ARRAYSIZE(columns)) return E_INVALIDARG;
        *out = SHCOLSTATE_ONBYDEFAULT | (column == 1 ? SHCOLSTATE_TYPE_INT : column == 3 ? SHCOLSTATE_TYPE_DATE : SHCOLSTATE_TYPE_STR);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDetailsEx(PCUITEMID_CHILD pidl, const SHCOLUMNID* key, VARIANT* out) override {
        if (!out || !key) return E_POINTER;
        VariantInit(out);
        return guarded([&] {
            Item item;
            if (!decode(pidl, item)) return E_INVALIDARG;
            if (IsEqualPropertyKey(*key, *properties[1])) {
                if (!item.folder() && item.size >= 0) { V_VT(out) = VT_UI8; V_UI8(out) = item.size; }
                return S_OK;
            }
            if (IsEqualPropertyKey(*key, *properties[3])) {
                if (item.modified) {
                    FILETIME file{static_cast<DWORD>(item.modified), static_cast<DWORD>(item.modified >> 32)}, local{};
                    SYSTEMTIME system{};
                    double date = 0;
                    if (FileTimeToLocalFileTime(&file, &local) && FileTimeToSystemTime(&local, &system) &&
                        SystemTimeToVariantTime(&system, &date)) { V_VT(out) = VT_DATE; V_DATE(out) = date; }
                }
                return S_OK;
            }
            const wchar_t* text = IsEqualPropertyKey(*key, *properties[0]) ? item.name.c_str() :
                IsEqualPropertyKey(*key, *properties[2]) ? type(item) : nullptr;
            if (!text) return E_NOTIMPL;
            V_BSTR(out) = SysAllocString(text);
            if (!V_BSTR(out)) return E_OUTOFMEMORY;
            V_VT(out) = VT_BSTR;
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE GetDetailsOf(PCUITEMID_CHILD pidl, UINT column, SHELLDETAILS* out) override {
        if (!out) return E_POINTER;
        if (column >= ARRAYSIZE(columns)) return E_INVALIDARG;
        out->fmt = column == 1 ? LVCFMT_RIGHT : LVCFMT_LEFT;
        out->cxChar = column == 0 ? 36 : column == 3 ? 24 : 16;
        if (!pidl) return display(&out->str, columns[column]);
        return guarded([&] {
            Item item;
            if (!decode(pidl, item)) return E_INVALIDARG;
            wchar_t formatted[96]{};
            switch (column) {
            case 0: return display(&out->str, item.name.c_str());
            case 1:
                if (!item.folder() && item.size >= 0) StrFormatByteSizeW(item.size, formatted, ARRAYSIZE(formatted));
                break;
            case 2: return display(&out->str, type(item));
            case 3:
                if (item.modified) {
                    FILETIME file{static_cast<DWORD>(item.modified), static_cast<DWORD>(item.modified >> 32)};
                    SHFormatDateTimeW(&file, nullptr, formatted, ARRAYSIZE(formatted));
                }
                break;
            }
            return display(&out->str, formatted);
        });
    }
    HRESULT STDMETHODCALLTYPE MapColumnToSCID(UINT column, SHCOLUMNID* out) override {
        if (!out) return E_POINTER;
        if (column >= ARRAYSIZE(properties)) return E_INVALIDARG;
        *out = *properties[column];
        return S_OK;
    }
private:
    static std::wstring parsingName(std::wstring_view id) {
        std::wstring result = L"wc-";
        constexpr wchar_t hex[] = L"0123456789ABCDEF";
        for (auto ch : id) for (int shift = 12; shift >= 0; shift -= 4) result += hex[(ch >> shift) & 15];
        return result;
    }
};

class Factory final : public IClassFactory, public ComObject {
    GUID account;
public:
    explicit Factory(REFGUID account) : account(account) {}
    COM_REFCOUNT
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IClassFactory) return E_NOINTERFACE;
        *out = static_cast<IClassFactory*>(this);
        AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        return guarded([&] {
            auto folder = new Folder(account);
            auto hr = folder->QueryInterface(iid, out);
            folder->Release();
            return hr;
        });
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
        if (lock) ++moduleReferences;
        else --moduleReferences;
        return S_OK;
    }
};
}

extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    return guarded([&] {
        auto factory = new Factory(clsid);
        auto hr = factory->QueryInterface(iid, out);
        factory->Release();
        return hr;
    });
}

extern "C" HRESULT WINAPI DllCanUnloadNow() {
    return moduleReferences == 0 ? S_OK : S_FALSE;
}
