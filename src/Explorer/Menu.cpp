#include "Explorer.hpp"

namespace {
class Menu final : public IContextMenu, public ComObject {
    GUID account;
    std::vector<Item> items;
    Pidl folder;
    Pidl parent;
    std::vector<Pidl> children;
    bool canCopy;
public:
    Menu(REFGUID account, std::vector<Item> items, Pidl folder, Pidl parent, std::vector<Pidl> children)
        : account(account), items(std::move(items)), folder(std::move(folder)), parent(std::move(parent)),
        children(std::move(children)), canCopy(std::all_of(this->items.begin(), this->items.end(),
            [](const auto& item) { return item.copyable(); })) {}
    COM_REFCOUNT
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IContextMenu) return E_NOINTERFACE;
        *out = static_cast<IContextMenu*>(this);
        AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryContextMenu(HMENU menu, UINT position, UINT first, UINT last, UINT flags) override {
        if (flags & CMF_NOVERBS || first > last) return S_OK;
        const bool open = items.size() == 1;
        if (open) {
            if (!InsertMenuW(menu, position++, MF_BYPOSITION | MF_STRING, first,
                folder ? L"Открыть" : L"Открыть в браузере")) return HRESULT_FROM_WIN32(GetLastError());
            if (!(flags & CMF_NODEFAULT)) SetMenuDefaultItem(menu, first, FALSE);
        }
        if (canCopy && !(flags & CMF_DEFAULTONLY) && last - first >= 1) {
            if (!InsertMenuW(menu, position, MF_BYPOSITION | MF_STRING, first + 1, L"Копировать"))
                return HRESULT_FROM_WIN32(GetLastError());
            return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 2);
        }
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, open ? 1 : 0);
    }
    HRESULT STDMETHODCALLTYPE InvokeCommand(CMINVOKECOMMANDINFO* command) override {
        if (!command) return E_POINTER;
        UINT id = UINT_MAX;
        if (IS_INTRESOURCE(command->lpVerb)) id = LOWORD(command->lpVerb);
        else if (command->cbSize >= sizeof(CMINVOKECOMMANDINFOEX) && (command->fMask & CMIC_MASK_UNICODE)) {
            auto verb = reinterpret_cast<CMINVOKECOMMANDINFOEX*>(command)->lpVerbW;
            if (!IS_INTRESOURCE(verb)) {
                if (!_wcsicmp(verb, L"open")) id = 0;
                if (!_wcsicmp(verb, L"copy")) id = 1;
            }
        } else {
            if (!_stricmp(command->lpVerb, "open")) id = 0;
            if (!_stricmp(command->lpVerb, "copy")) id = 1;
        }
        if (id == 0 && items.size() == 1) {
            if (folder) {
                return SHOpenFolderAndSelectItems(folder.get(), 0, nullptr, 0);
            }
            Pipe pipe;
            return pipe.open(account, 3, items.front().id) ? S_OK : HRESULT_FROM_WIN32(ERROR_NETWORK_UNREACHABLE);
        }
        if (id == 1 && canCopy) {
            return guarded([&] {
                std::vector<PCUITEMID_CHILD> selection;
                for (const auto& child : children) selection.push_back(child.get());
                IDataObject* data = nullptr;
                auto hr = makeShellDataObject(account, items, parent.get(), static_cast<UINT>(selection.size()),
                    selection.data(), IID_IDataObject, reinterpret_cast<void**>(&data));
                if (SUCCEEDED(hr)) {
                    hr = OleSetClipboard(data);
                    data->Release();
                }
                return hr;
            });
        }
        return E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE GetCommandString(UINT_PTR id, UINT type, UINT*, LPSTR result, UINT capacity) override {
        if ((id == 0 && items.size() != 1) || (id == 1 && !canCopy) || id > 1) return E_INVALIDARG;
        if (type == GCS_VALIDATEA || type == GCS_VALIDATEW) return S_OK;
        if (!result || !capacity) return E_INVALIDARG;
        if (type == GCS_VERBW) return wcscpy_s(reinterpret_cast<LPWSTR>(result), capacity, id == 0 ? L"open" : L"copy") ? E_FAIL : S_OK;
        if (type == GCS_VERBA) return strcpy_s(result, capacity, id == 0 ? "open" : "copy") ? E_FAIL : S_OK;
        return E_NOTIMPL;
    }
};
}

HRESULT makeMenu(REFGUID account, std::vector<Item> items, PCIDLIST_ABSOLUTE parent,
    UINT count, PCUITEMID_CHILD_ARRAY children, REFIID iid, void** out) {
    Pidl folder;
    Pidl location(parent ? ILCloneFull(parent) : nullptr);
    if (parent && !location) return E_OUTOFMEMORY;
    std::vector<Pidl> selection;
    for (UINT i = 0; i < count; ++i) {
        Pidl clone(ILCloneFirst(children[i]));
        if (!clone) return E_OUTOFMEMORY;
        selection.push_back(std::move(clone));
    }
    if (count == 1 && items.front().folder()) {
        if (!parent) return E_UNEXPECTED;
        folder.reset(ILCombine(parent, children[0]));
        if (!folder) return E_OUTOFMEMORY;
    }
    auto menu = new Menu(account, std::move(items), std::move(folder), std::move(location), std::move(selection));
    auto hr = menu->QueryInterface(iid, out);
    menu->Release();
    return hr;
}
