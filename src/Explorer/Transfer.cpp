#include "Explorer.hpp"

namespace {
class DownloadStream final : public IStream, public ComObject {
    GUID account;
    Item item;
    Pipe pipe;
    SRWLOCK lock = SRWLOCK_INIT;
    uint64_t position{};
    int64_t length;
    uint32_t remaining{};
    bool connected{};
    bool eof{};
    IUnknown* marshaler{};

    struct Exclusive {
        SRWLOCK& lock;
        explicit Exclusive(SRWLOCK& lock) : lock(lock) { AcquireSRWLockExclusive(&lock); }
        ~Exclusive() { ReleaseSRWLockExclusive(&lock); }
    };
    void disconnect() {
        pipe.close();
        connected = false;
        remaining = 0;
    }
    HRESULT ensureConnected() {
        if (connected) return S_OK;
        int64_t actual = -1;
        if (!pipe.open(account, 2, item.id, position) || !pipe.read(actual)) {
            disconnect();
            return STG_E_READFAULT;
        }
        if (actual >= 0) {
            if (length >= 0 && actual != length) { disconnect(); return STG_E_READFAULT; }
            length = actual;
        }
        connected = true;
        return S_OK;
    }
public:
    DownloadStream(REFGUID account, Item item) : account(account), item(std::move(item)), length(this->item.size) {
        CoCreateFreeThreadedMarshaler(static_cast<IStream*>(this), &marshaler);
    }
    ~DownloadStream() override { if (marshaler) marshaler->Release(); }
    COM_REFCOUNT
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == IID_IMarshal && marshaler) return marshaler->QueryInterface(iid, out);
        if (iid != IID_IUnknown && iid != IID_IStream && iid != IID_ISequentialStream) return E_NOINTERFACE;
        *out = static_cast<IStream*>(this);
        AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Read(void* buffer, ULONG bytes, ULONG* read) override {
        if (read) *read = 0;
        if (!bytes) return S_OK;
        if (!buffer) return STG_E_INVALIDPOINTER;
        Exclusive guard(lock);
        if (eof || (!connected && length >= 0 && position >= static_cast<uint64_t>(length))) return S_FALSE;
        auto hr = ensureConnected();
        if (FAILED(hr)) return hr;
        auto output = static_cast<BYTE*>(buffer);
        ULONG total = 0;
        while (total < bytes) {
            if (!remaining) {
                if (!pipe.read(remaining) || remaining > 65536) { hr = STG_E_READFAULT; break; }
                if (!remaining) {
                    eof = true;
                    disconnect();
                    if (length < 0) length = static_cast<int64_t>(position);
                    hr = position == static_cast<uint64_t>(length) ? S_FALSE : STG_E_READFAULT;
                    break;
                }
                if (length >= 0 && position + remaining > static_cast<uint64_t>(length)) { hr = STG_E_READFAULT; break; }
            }
            auto count = std::min<ULONG>(bytes - total, remaining);
            if (!pipe.read(output + total, count)) { hr = STG_E_READFAULT; break; }
            remaining -= count;
            position += count;
            total += count;
        }
        if (FAILED(hr)) disconnect();
        if (read) *read = total;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* result) override {
        Exclusive guard(lock);
        uint64_t base{};
        switch (origin) {
        case STREAM_SEEK_SET: break;
        case STREAM_SEEK_CUR: base = position; break;
        case STREAM_SEEK_END:
            if (length < 0 && FAILED(ensureConnected())) return STG_E_READFAULT;
            if (length < 0) return STG_E_INVALIDFUNCTION;
            base = static_cast<uint64_t>(length);
            break;
        default: return STG_E_INVALIDFUNCTION;
        }
        auto delta = move.QuadPart;
        auto magnitude = delta >= 0 ? static_cast<uint64_t>(delta) : static_cast<uint64_t>(-(delta + 1)) + 1;
        if (delta < 0 ? magnitude > base : magnitude > static_cast<uint64_t>(INT64_MAX) - base)
            return STG_E_INVALIDFUNCTION;
        auto next = delta < 0 ? base - magnitude : base + magnitude;
        if (next != position) {
            disconnect();
            eof = false;
            position = next;
        }
        if (result) result->QuadPart = position;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream* target, ULARGE_INTEGER bytes, ULARGE_INTEGER* read, ULARGE_INTEGER* written) override {
        if (read) read->QuadPart = 0;
        if (written) written->QuadPart = 0;
        if (!target) return STG_E_INVALIDPOINTER;
        std::array<BYTE, 32768> buffer;
        while (bytes.QuadPart) {
            ULONG count = 0, stored = 0;
            HRESULT hr = Read(buffer.data(), static_cast<ULONG>(std::min<uint64_t>(buffer.size(), bytes.QuadPart)), &count);
            if (read) read->QuadPart += count;
            if (FAILED(hr)) return hr;
            if (!count) return S_OK;
            hr = target->Write(buffer.data(), count, &stored);
            if (written) written->QuadPart += stored;
            if (FAILED(hr)) return hr;
            if (stored != count) return STG_E_WRITEFAULT;
            bytes.QuadPart -= count;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD flags) override {
        if (!stat) return STG_E_INVALIDPOINTER;
        *stat = {};
        Exclusive guard(lock);
        stat->type = STGTY_STREAM;
        stat->grfMode = STGM_READ;
        if (length < 0) {
            auto hr = ensureConnected();
            if (FAILED(hr)) return hr;
        }
        if (length >= 0) stat->cbSize.QuadPart = static_cast<uint64_t>(length);
        stat->mtime.dwLowDateTime = static_cast<DWORD>(item.modified);
        stat->mtime.dwHighDateTime = static_cast<DWORD>(item.modified >> 32);
        if (!(flags & STATFLAG_NONAME)) return SHStrDupW(item.copyName.c_str(), &stat->pwcsName);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        return guarded([&] {
            Exclusive guard(lock);
            auto clone = new DownloadStream(account, item);
            clone->position = position;
            clone->length = length;
            *out = clone;
            return S_OK;
        });
    }
};

std::wstring safeName(std::wstring name) {
    for (auto& ch : name)
        if (ch < L' ' || std::wstring_view(L"<>:\"/\\|?*").find(ch) != std::wstring_view::npos) ch = L'_';
    while (!name.empty() && (name.back() == L'.' || name.back() == L' ')) name.pop_back();
    if (name.empty()) return L"file";
    if (name.size() > 235) {
        auto dot = name.find_last_of(L'.');
        auto extension = dot != std::wstring::npos && name.size() - dot < 32 ? name.substr(dot) : L"";
        name.resize(235 - extension.size());
        if (name.back() >= 0xD800 && name.back() <= 0xDBFF) name.pop_back();
        name += extension;
    }
    auto stem = name.substr(0, name.find(L'.'));
    while (!stem.empty() && stem.back() == L' ') stem.pop_back();
    constexpr const wchar_t* reserved[] = {L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"};
    auto same = [&](const wchar_t* other) { return _wcsicmp(stem.c_str(), other) == 0; };
    if (std::any_of(std::begin(reserved), std::end(reserved), same) ||
        (stem.size() == 4 && (!_wcsnicmp(stem.c_str(), L"COM", 3) || !_wcsnicmp(stem.c_str(), L"LPT", 3)) &&
         std::wstring_view(L"0123456789\u00b9\u00b2\u00b3").find(stem[3]) != std::wstring_view::npos)) name.insert(0, 1, L'_');
    return name;
}

class ClipboardFiles final : public IDataObject, public IDataObjectAsyncCapability, public ComObject {
    GUID account;
    std::vector<Item> items;
    std::atomic<BOOL> asynchronous{TRUE};
    std::atomic<BOOL> running{FALSE};
    IUnknown* marshaler{};
    const CLIPFORMAT descriptors = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW));
    const CLIPFORMAT contents = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILECONTENTS));
    const CLIPFORMAT effect = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
public:
    ClipboardFiles(REFGUID account, std::vector<Item> items) : account(account), items(std::move(items)) {
        for (auto it = this->items.begin(); it != this->items.end(); ++it) {
            auto name = safeName(it->copyName);
            const auto dot = name.find_last_of(L'.');
            const auto suffix = dot == 0 || dot == std::wstring::npos ? L"" : name.substr(dot);
            const auto stem = name.substr(0, name.size() - suffix.size());
            for (unsigned index = 2; std::any_of(this->items.begin(), it, [&](const Item& previous) {
                return !_wcsicmp(previous.copyName.c_str(), name.c_str());
            }); ++index) name = stem + L" (" + std::to_wstring(index) + L")" + suffix;
            it->copyName = std::move(name);
        }
        CoCreateFreeThreadedMarshaler(static_cast<IDataObject*>(this), &marshaler);
    }
    ~ClipboardFiles() override { if (marshaler) marshaler->Release(); }
    COM_REFCOUNT
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == IID_IMarshal && marshaler) return marshaler->QueryInterface(iid, out);
        if (iid == IID_IUnknown || iid == IID_IDataObject) *out = static_cast<IDataObject*>(this);
        else if (iid == IID_IDataObjectAsyncCapability) *out = static_cast<IDataObjectAsyncCapability*>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
        if (!medium) return E_POINTER;
        *medium = {};
        auto hr = QueryGetData(format);
        if (FAILED(hr)) return hr;
        if (format->cfFormat == contents) {
            const auto index = format->lindex < 0 ? 0 : format->lindex;
            hr = makeStream(account, items[index], IID_IStream, reinterpret_cast<void**>(&medium->pstm));
            if (SUCCEEDED(hr)) medium->tymed = TYMED_ISTREAM;
            return hr;
        }
        const bool files = format->cfFormat == descriptors;
        SIZE_T bytes = files ? offsetof(FILEGROUPDESCRIPTORW, fgd) + items.size() * sizeof(FILEDESCRIPTORW) : sizeof(DWORD);
        auto handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
        if (!handle) return E_OUTOFMEMORY;
        auto data = GlobalLock(handle);
        if (!data) { GlobalFree(handle); return E_OUTOFMEMORY; }
        if (files) {
            auto group = static_cast<FILEGROUPDESCRIPTORW*>(data);
            group->cItems = static_cast<UINT>(items.size());
            auto entries = reinterpret_cast<FILEDESCRIPTORW*>(static_cast<BYTE*>(data) + offsetof(FILEGROUPDESCRIPTORW, fgd));
            for (const auto& item : items) {
                auto& entry = *entries++;
                entry.dwFlags = FD_ATTRIBUTES | FD_UNICODE | FD_PROGRESSUI;
                entry.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
                if (item.size >= 0) {
                    entry.dwFlags |= FD_FILESIZE;
                    entry.nFileSizeLow = static_cast<DWORD>(item.size);
                    entry.nFileSizeHigh = static_cast<DWORD>(item.size >> 32);
                }
                if (item.modified) {
                    entry.dwFlags |= FD_WRITESTIME;
                    entry.ftLastWriteTime = {static_cast<DWORD>(item.modified), static_cast<DWORD>(item.modified >> 32)};
                }
                wcscpy_s(entry.cFileName, item.copyName.c_str());
            }
        } else *static_cast<DWORD*>(data) = DROPEFFECT_COPY;
        GlobalUnlock(handle);
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = handle;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
        if (!format) return E_POINTER;
        if (format->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;
        if (format->cfFormat == contents) {
            if (!(format->tymed & TYMED_ISTREAM)) return DV_E_TYMED;
            return format->lindex == -1 || (format->lindex >= 0 && static_cast<size_t>(format->lindex) < items.size())
                ? S_OK : DV_E_LINDEX;
        }
        if (format->cfFormat != descriptors && format->cfFormat != effect) return DV_E_FORMATETC;
        return format->tymed & TYMED_HGLOBAL ? S_OK : DV_E_TYMED;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
        if (!out) return E_POINTER;
        out->ptd = nullptr;
        return DATA_S_SAMEFORMATETC;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (direction != DATADIR_GET) return E_NOTIMPL;
        FORMATETC formats[] = {
            {descriptors, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
            {contents, nullptr, DVASPECT_CONTENT, -1, TYMED_ISTREAM},
            {effect, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}
        };
        return SHCreateStdEnumFmtEtc(ARRAYSIZE(formats), formats, out);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE SetAsyncMode(BOOL value) override { asynchronous = value; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetAsyncMode(BOOL* value) override { if (!value) return E_POINTER; *value = asynchronous; return S_OK; }
    HRESULT STDMETHODCALLTYPE StartOperation(IBindCtx*) override { running = TRUE; return S_OK; }
    HRESULT STDMETHODCALLTYPE InOperation(BOOL* value) override { if (!value) return E_POINTER; *value = running; return S_OK; }
    HRESULT STDMETHODCALLTYPE EndOperation(HRESULT, IBindCtx*, DWORD) override { running = FALSE; return S_OK; }
};
}

HRESULT makeDataObject(REFGUID account, std::vector<Item> items, REFIID iid, void** out) {
    if (items.empty() || std::any_of(items.begin(), items.end(), [](const auto& item) { return !item.copyable(); }))
        return E_NOINTERFACE;
    return guarded([&] {
        auto object = new ClipboardFiles(account, std::move(items));
        auto hr = object->QueryInterface(iid, out);
        object->Release();
        return hr;
    });
}

HRESULT makeShellDataObject(REFGUID account, std::vector<Item> items, PCIDLIST_ABSOLUTE parent,
    UINT count, PCUITEMID_CHILD_ARRAY children, REFIID iid, void** out) {
    if (!parent) return makeDataObject(account, std::move(items), iid, out);
    IDataObject* inner = nullptr;
    auto hr = makeDataObject(account, std::move(items), IID_IDataObject, reinterpret_cast<void**>(&inner));
    if (FAILED(hr)) return hr;
    hr = SHCreateDataObject(parent, count, children, inner, iid, out);
    inner->Release();
    return hr;
}

HRESULT makeStream(REFGUID account, const Item& item, REFIID iid, void** out) {
    if (!item.copyable()) return E_NOINTERFACE;
    return guarded([&] {
        auto stream = new DownloadStream(account, item);
        auto hr = stream->QueryInterface(iid, out);
        stream->Release();
        return hr;
    });
}
