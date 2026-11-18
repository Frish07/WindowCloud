#include "../src/Explorer/Explorer.hpp"
#include <cstdio>
#include <thread>
#include <stdexcept>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void check(HRESULT hr, const char* message) {
    if (FAILED(hr)) {
        std::printf("%s failed with %08lX\n", message, static_cast<unsigned long>(hr));
        throw std::runtime_error(message);
    }
    std::puts(message);
}
template<class T> struct Com {
    T* value{};
    ~Com() { if (value) value->Release(); }
    T* operator->() const { return value; }
    void** out() { return reinterpret_cast<void**>(&value); }
};
struct Handle {
    HANDLE value;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct ClipboardRestore {
    Com<IDataObject> previous;
    ClipboardRestore() { OleGetClipboard(&previous.value); }
    ~ClipboardRestore() { OleSetClipboard(previous.value); }
};
class LocalFiles final : public IDataObject, public ComObject {
    HGLOBAL paths;
public:
    explicit LocalFiles(HGLOBAL paths) : paths(paths) {}
    ~LocalFiles() override { GlobalFree(paths); }
    COM_REFCOUNT
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDataObject) return E_NOINTERFACE;
        *out = static_cast<IDataObject*>(this);
        AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
        return format && format->cfFormat == CF_HDROP && (format->tymed & TYMED_HGLOBAL) ? S_OK : DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
        if (FAILED(QueryGetData(format))) return DV_E_FORMATETC;
        *medium = {};
        medium->hGlobal = OleDuplicateData(paths, CF_HDROP, 0);
        medium->tymed = TYMED_HGLOBAL;
        return medium->hGlobal ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override {
        if (direction != DATADIR_GET) return E_NOTIMPL;
        FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        return SHCreateStdEnumFmtEtc(1, &format, out);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
};
template<class T> void send(HANDLE pipe, const T& value) {
    DWORD count{};
    require(WriteFile(pipe, &value, sizeof(value), &count, nullptr) && count == sizeof(value), "Write pipe value");
}
template<class T> T receive(HANDLE pipe) {
    T value;
    DWORD count{};
    require(ReadFile(pipe, &value, sizeof(value), &count, nullptr) && count == sizeof(value), "Read pipe value");
    return value;
}
void sendText(HANDLE pipe, const std::wstring& text) {
    auto bytes = static_cast<USHORT>(text.size() * 2);
    send(pipe, bytes);
    DWORD count{};
    require(WriteFile(pipe, text.data(), bytes, &count, nullptr) && count == bytes, "Write pipe text");
}
std::wstring receiveText(HANDLE pipe, DWORD bytes) {
    if (bytes == 0) return {};
    std::wstring value(bytes / 2, L'\0');
    DWORD count{};
    require(ReadFile(pipe, value.data(), bytes, &count, nullptr) && count == bytes, "Read pipe text");
    return value;
}
void sendItem(HANDLE pipe, BYTE flags, int64_t size, const wchar_t* id, const wchar_t* name) {
    send(pipe, flags);
    send(pipe, size);
    send(pipe, uint64_t{});
    sendText(pipe, id);
    sendText(pipe, name);
    sendText(pipe, name);
}

class MockCloud {
    std::thread listener;
    std::atomic<bool> stopping{};
    std::mutex uploadsLock;
    std::vector<std::wstring> uploads;
    std::wstring pipePath;
    std::wstring namespaceKey;
public:
    GUID account{};
    std::wstring key, pipeName;
    MockCloud(const fs::path& dll) {
        check(CoCreateGuid(&account), "Create account ID");
        wchar_t guid[39]{};
        StringFromGUID2(account, guid, 39);
        key = L"Software\\Classes\\CLSID\\" + std::wstring(guid);
        pipeName = L"WindowCloud-";
        for (auto ch : std::wstring(guid)) if (iswxdigit(ch)) pipeName += static_cast<wchar_t>(towlower(ch));
        pipePath = L"\\\\.\\pipe\\" + pipeName;
        HKEY registry{};
        require(RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &registry, nullptr) == 0,
            "Create temporary COM registration");
        RegSetValueExW(registry, L"WindowCloud.Account", 0, REG_SZ, reinterpret_cast<const BYTE*>(pipeName.c_str()),
            static_cast<DWORD>((pipeName.size() + 1) * 2));
        RegCloseKey(registry);
        RegCreateKeyExW(HKEY_CURRENT_USER, (key + L"\\ShellFolder").c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &registry, nullptr);
        DWORD attributes = SFGAO_FOLDER | SFGAO_HASSUBFOLDER | SFGAO_DROPTARGET;
        RegSetValueExW(registry, L"Attributes", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&attributes), sizeof(attributes));
        RegCloseKey(registry);
        namespaceKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\MyComputer\\NameSpace\\" + std::wstring(guid);
        RegCreateKeyExW(HKEY_CURRENT_USER, namespaceKey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &registry, nullptr);
        RegCloseKey(registry);
        RegCreateKeyExW(HKEY_CURRENT_USER, (key + L"\\InProcServer32").c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &registry, nullptr);
        auto path = dll.wstring();
        RegSetValueExW(registry, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(path.c_str()), static_cast<DWORD>((path.size() + 1) * 2));
        constexpr wchar_t apartment[] = L"Apartment";
        RegSetValueExW(registry, L"ThreadingModel", 0, REG_SZ, reinterpret_cast<const BYTE*>(apartment), sizeof(apartment));
        RegCloseKey(registry);
        listener = std::thread([this] {
            while (!stopping) {
                auto pipe = CreateNamedPipeW(pipePath.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, 16, 65536, 65536, 0, nullptr);
                if (pipe == INVALID_HANDLE_VALUE) return;
                if (!(ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) || stopping) {
                    CloseHandle(pipe);
                    continue;
                }
                std::thread([this, pipe] {
                    Handle connection{pipe};
                    try { serve(pipe); FlushFileBuffers(pipe); }
                    catch (...) { }
                }).detach();
            }
        });
    }
    ~MockCloud() {
        stopping = true;
        auto wake = CreateFileW(pipePath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (wake != INVALID_HANDLE_VALUE) CloseHandle(wake);
        listener.join();
        RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
        RegDeleteTreeW(HKEY_CURRENT_USER, namespaceKey.c_str());
    }
    void serve(HANDLE pipe) {
        auto command = receive<BYTE>(pipe);
        auto requested = receive<GUID>(pipe);
        require(requested == account, "Account routing");
        auto length = receive<DWORD>(pipe);
        auto offset = receive<uint64_t>(pipe);
        auto id = receiveText(pipe, length);
        std::printf("Cloud request %u\n", command);
        if (command == 1) {
            send(pipe, BYTE{});
            if (id.empty()) {
                send(pipe, DWORD{1});
                sendItem(pipe, 1, -1, L"folder", L"Cloud folder");
            } else if (id == L"folder") {
                send(pipe, DWORD{3});
                sendItem(pipe, 1, -1, L"empty", L"Empty folder");
                sendItem(pipe, 1, -1, L"nested", L"Nested");
                sendItem(pipe, 2, 10003, L"file", L"данные.bin");
            } else if (id == L"nested") {
                send(pipe, DWORD{2});
                sendItem(pipe, 2, 0, L"zero", L"a?.txt");
                sendItem(pipe, 2, 7, L"small", L"a*.txt");
            } else send(pipe, DWORD{});
        } else if (command == 2) {
            int64_t size = id == L"file" ? 10003 : id == L"small" ? 7 : 0;
            send(pipe, BYTE{});
            send(pipe, size);
            std::array<BYTE, 4096> bytes;
            for (uint64_t position = offset; position < static_cast<uint64_t>(size);) {
                auto count = static_cast<DWORD>(std::min<uint64_t>(bytes.size(), size - position));
                for (DWORD i = 0; i < count; ++i) bytes[i] = static_cast<BYTE>((position + i) % 251);
                send(pipe, count);
                DWORD stored{};
                require(WriteFile(pipe, bytes.data(), count, &stored, nullptr) && stored == count, "Write file chunk");
                position += count;
            }
            send(pipe, DWORD{});
        } else if (command == 5) {
            require(id == L"folder", "Correct upload destination");
            auto count = receive<DWORD>(pipe);
            std::lock_guard guard(uploadsLock);
            for (DWORD i = 0; i < count; ++i) {
                auto bytes = receive<USHORT>(pipe);
                uploads.push_back(receiveText(pipe, bytes));
            }
            auto bytes = receive<DWORD>(pipe);
            require(bytes >= 2, "Destination refresh identifier");
            std::vector<BYTE> location(bytes);
            DWORD received{};
            require(ReadFile(pipe, location.data(), bytes, &received, nullptr) && received == bytes, "Read destination identifier");
            send(pipe, BYTE{});
        } else send(pipe, BYTE{1});
    }
    void verifyUploads(const std::wstring& expected, size_t count = 1) {
        std::lock_guard guard(uploadsLock);
        require(uploads.size() == count && uploads.back() == expected, "Local folder queued for recursive upload");
    }
};

void pasteInto(const fs::path& destination) {
    Com<IShellItem> item;
    check(SHCreateItemFromParsingName(destination.c_str(), nullptr, IID_IShellItem, item.out()), "Destination Shell item");
    Com<IShellFolder> folder;
    check(item->BindToHandler(nullptr, BHID_SFObject, IID_IShellFolder, folder.out()), "Bind destination folder");
    Com<IContextMenu> menu;
    check(folder->CreateViewObject(nullptr, IID_IContextMenu, menu.out()), "Destination context menu");
    CMINVOKECOMMANDINFO command{sizeof(command)};
    command.lpVerb = "paste";
    command.nShow = SW_SHOWNORMAL;
    check(menu->InvokeCommand(&command), "Invoke native Paste");
}

int wmain(int argc, wchar_t** args) {
    try {
        setvbuf(stdout, nullptr, _IONBF, 0);
        require(argc == 3, "Expected DLL and temporary directory paths");
        check(OleInitialize(nullptr), "Initialize OLE");
        ClipboardRestore restoreClipboard;
        Com<IDataObject> systemData;
        auto availability = SHCreateDataObject(nullptr, 0, nullptr, nullptr, IID_IDataObject, systemData.out());
        bool desktop = availability != REGDB_E_CLASSNOTREG;
        if (desktop) check(availability, "System Shell data object");
        else std::puts("System Shell data-object class is unregistered. Desktop Paste checks are unavailable on this host.");
        auto temporary = fs::path(args[2]);
        fs::create_directory(temporary / "output");
        MockCloud mock(fs::absolute(args[1]));
        Com<IShellFolder> account;
        check(CoCreateInstance(mock.account, nullptr, CLSCTX_INPROC_SERVER, IID_IShellFolder, account.out()), "Activate Explorer extension");
        Com<IPersistFolder> persist;
        check(account->QueryInterface(IID_IPersistFolder, persist.out()), "Persistent folder");
        wchar_t guid[39]{};
        StringFromGUID2(mock.account, guid, 39);
        auto name = L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\::" + std::wstring(guid);
        PIDLIST_ABSOLUTE rootRaw{};
        check(SHParseDisplayName(name.c_str(), nullptr, &rootRaw, 0, nullptr), "Parse registered account");
        Pidl root(rootRaw);
        check(persist->Initialize(root.get()), "Initialize account location");
        Com<IEnumIDList> enumeration;
        check(account->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &enumeration.value), "Enumerate account");
        ITEMIDLIST* raw{};
        require(enumeration->Next(1, &raw, nullptr) == S_OK, "Cloud folder enumerated");
        Pidl cloudFolder(raw);
        const ITEMIDLIST* selection[] = {cloudFolder.get()};
        Com<IDataObject> clipboard;
        check(account->GetUIObjectOf(nullptr, 1, selection, IID_IDataObject, nullptr, clipboard.out()), "Get cloud folder data object");
        if (desktop) {
            Com<IDataObject> registeredSource;
            auto sourceStatus = SHCreateDataObject(root.get(), 1, selection, clipboard.value, IID_IDataObject, registeredSource.out());
            if (sourceStatus == REGDB_E_CLASSNOTREG) {
                desktop = false;
                std::puts("Registered Shell source class is unavailable. Verifying descriptors, file streams and upload target.");
            } else check(sourceStatus, "System Shell source object");
        }
        FORMATETC descriptor{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW)),
            nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM medium{};
        check(clipboard->GetData(&descriptor, &medium), "Get recursive file descriptors");
        auto group = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(medium.hGlobal));
        require(group && group->cItems == 6, "Folder descriptors include all descendants");
        require(group->fgd[0].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY, "Directory attribute");
        std::vector<LONG> fileIndexes;
        std::vector<uint64_t> sizes;
        for (UINT index = 0; index < group->cItems; ++index) {
            if (group->fgd[index].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            fileIndexes.push_back(static_cast<LONG>(index));
            sizes.push_back((static_cast<uint64_t>(group->fgd[index].nFileSizeHigh) << 32) | group->fgd[index].nFileSizeLow);
        }
        GlobalUnlock(medium.hGlobal);
        ReleaseStgMedium(&medium);
        for (size_t i = 0; i < fileIndexes.size(); ++i) {
            FORMATETC contents{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILECONTENTS)),
                nullptr, DVASPECT_CONTENT, fileIndexes[i], TYMED_ISTREAM};
            check(clipboard->GetData(&contents, &medium), "Read indexed file contents");
            ULONG count{};
            uint64_t position{};
            std::array<BYTE, 4096> buffer;
            HRESULT result;
            do {
                result = medium.pstm->Read(buffer.data(), static_cast<ULONG>(buffer.size()), &count);
                check(result, "Read file stream");
                for (ULONG j = 0; j < count; ++j) require(buffer[j] == (position + j) % 251, "Stream bytes");
                position += count;
            } while (result == S_OK);
            ReleaseStgMedium(&medium);
            require(position == sizes[i], "Stream size");
        }
        std::puts("Recursive descriptors and indexed file bytes verified.");
        if (desktop) {
        check(OleSetClipboard(clipboard.value), "Set cloud folder clipboard");
        pasteInto(temporary / "output");
        auto output = temporary / "output" / "Cloud folder";
        for (int i = 0; i < 200 && !fs::exists(output / "Nested" / "a_ (2).txt"); ++i) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 25, QS_ALLINPUT);
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        }
        require(fs::is_directory(output / "Empty folder"), "Empty cloud folder copied");
        require(fs::file_size(output / "Nested" / "a_.txt") == 0, "Empty file copied");
        require(fs::file_size(output / "Nested" / "a_ (2).txt") == 7, "Filename collision resolved");
        std::ifstream input(output / L"данные.bin", std::ios::binary);
        require(input.good(), "Nested file copied");
        for (int i = 0; i < 10003; ++i) require(input.get() == i % 251, "Downloaded file bytes");
        require(input.get() == EOF, "Downloaded file size");
        check(OleSetClipboard(nullptr), "Clear clipboard");
        std::puts("Native Paste copied nested files, empty folders, Unicode names and filename collisions.");
        }
        Com<IShellFolder> destination;
        check(account->BindToObject(cloudFolder.get(), nullptr, IID_IShellFolder, destination.out()), "Bind cloud destination");
        Com<IDropTarget> target;
        check(destination->CreateViewObject(nullptr, IID_IDropTarget, target.out()), "Get cloud upload target");
        auto path = (temporary / "output").wstring();
        auto bytes = sizeof(DROPFILES) + (path.size() + 2) * sizeof(wchar_t);
        medium = {};
        medium.tymed = TYMED_HGLOBAL;
        medium.hGlobal = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
        auto files = static_cast<DROPFILES*>(GlobalLock(medium.hGlobal));
        files->pFiles = sizeof(DROPFILES);
        files->fWide = TRUE;
        std::memcpy(reinterpret_cast<BYTE*>(files) + sizeof(DROPFILES), path.c_str(), (path.size() + 1) * 2);
        GlobalUnlock(medium.hGlobal);
        Com<IDataObject> local;
        local.value = new LocalFiles(medium.hGlobal);
        DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
        check(target->DragEnter(local.value, 0, {}, &effect), "Accept local folder drag");
        require(effect == DROPEFFECT_COPY, "Only copy offered");
        check(target->Drop(local.value, 0, {}, &effect), "Queue folder upload");
        require(effect == DROPEFFECT_COPY, "Folder upload accepted");
        mock.verifyUploads(path);
        std::puts("Cloud drop target forwards a local folder to the background upload operation.");
        if (desktop) {
        check(OleSetClipboard(local.value), "Set local clipboard for cloud Paste");
        Com<IContextMenu> background;
        check(destination->CreateViewObject(nullptr, IID_IContextMenu, background.out()), "Cloud background menu");
        CMINVOKECOMMANDINFO paste{sizeof(paste)};
        paste.lpVerb = "paste";
        check(background->InvokeCommand(&paste), "Invoke Paste into cloud folder");
        mock.verifyUploads(path, 2);
        check(OleSetClipboard(nullptr), "Clear local clipboard");
        std::puts("Native Paste into the cloud folder queues the upload.");
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
