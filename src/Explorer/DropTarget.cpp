#include "Explorer.hpp"

namespace {
class UploadTarget final : public IDropTarget, public ComObject {
    GUID account;
    std::wstring parent;
    Pidl location;
    bool accepted{};

    static bool supports(IDataObject* data) {
        FORMATETC files{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        return data && SUCCEEDED(data->QueryGetData(&files));
    }
public:
    UploadTarget(REFGUID account, std::wstring parent, Pidl location)
        : account(account), parent(std::move(parent)), location(std::move(location)) {}
    COM_REFCOUNT
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDropTarget) return E_NOINTERFACE;
        *out = static_cast<IDropTarget*>(this);
        AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        if (!effect) return E_POINTER;
        accepted = supports(data);
        *effect = accepted ? *effect & DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override {
        if (!effect) return E_POINTER;
        *effect = accepted ? *effect & DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override { accepted = false; return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        if (!effect) return E_POINTER;
        auto allowed = *effect;
        *effect = DROPEFFECT_NONE;
        accepted = false;
        if (!(allowed & DROPEFFECT_COPY) || !supports(data)) return DV_E_FORMATETC;
        return guarded([&] {
            FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            STGMEDIUM medium{};
            auto hr = data->GetData(&format, &medium);
            if (FAILED(hr)) return hr;
            struct Release { STGMEDIUM& medium; ~Release() { ReleaseStgMedium(&medium); } } release{medium};
            auto drop = static_cast<HDROP>(medium.hGlobal);
            auto count = DragQueryFileW(drop, UINT_MAX, nullptr, 0);
            if (!count || count > 10000) return E_INVALIDARG;
            Pipe pipe;
            if (!pipe.open(account, 5, parent, 0, false) || !pipe.write(count))
                return HRESULT_FROM_WIN32(ERROR_NETWORK_UNREACHABLE);
            for (UINT index = 0; index < count; ++index) {
                auto length = DragQueryFileW(drop, index, nullptr, 0);
                if (!length || length > 32760) return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
                std::wstring path(length + 1, L'\0');
                if (DragQueryFileW(drop, index, path.data(), length + 1) != length) return E_FAIL;
                auto bytes = static_cast<uint16_t>(length * sizeof(wchar_t));
                if (!pipe.write(bytes) || !pipe.write(path.data(), bytes)) return E_FAIL;
            }
            auto bytes = location ? ILGetSize(location.get()) : 0;
            if (!pipe.write(bytes) || (bytes && !pipe.write(location.get(), bytes))) return E_FAIL;
            BYTE status = 1;
            if (!pipe.read(status) || status != 0) {
                MessageBoxW(nullptr, L"Не удалось начать загрузку. Проверьте подключение и повторно войдите в аккаунт, чтобы разрешить запись.",
                    L"WindowCloud", MB_OK | MB_ICONERROR);
                return E_FAIL;
            }
            *effect = DROPEFFECT_COPY;
            return S_OK;
        });
    }
};
}

HRESULT makeDropTarget(REFGUID account, std::wstring parent, PCIDLIST_ABSOLUTE location, REFIID iid, void** out) {
    Pidl clone(location ? ILCloneFull(location) : nullptr);
    if (location && !clone) return E_OUTOFMEMORY;
    auto target = new UploadTarget(account, std::move(parent), std::move(clone));
    auto hr = target->QueryInterface(iid, out);
    target->Release();
    return hr;
}
