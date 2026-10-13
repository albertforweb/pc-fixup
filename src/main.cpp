#include <windows.h>
#include <commctrl.h>
#include <algorithm>
#include <iterator>
#include <shellapi.h>
#include <shlobj.h>
#include <taskschd.h>
#include <comdef.h>
#include <string>
#include <vector>
#include <sstream>
#include <cwchar>
#include <utility>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "taskschd.lib")

namespace {
constexpr wchar_t kClassName[] = L"WinStartReviewWindow";
constexpr int kListId = 1001;
constexpr int kTreeId = 1002;
constexpr int kToolbarId = 1003;
constexpr int kStatusId = 1004;
constexpr int kSplitterId = 1005;
constexpr int kRefreshId = 1101;
constexpr int kExitId = 1102;
constexpr int kInitialTreeWidth = 250;

enum class Category : LPARAM { All, Services, RegistryRun, RegistryRunOnce, Tasks, UserStartup, CommonStartup, RegistryGroup, StartupGroup };

struct Entry {
    std::wstring name;
    Category category;
    std::wstring kind;
    std::wstring location;
    std::wstring startup;
    std::wstring state;
    std::wstring assessment;
};

HWND g_list = nullptr;
HWND g_tree = nullptr;
HWND g_toolbar = nullptr;
HWND g_status = nullptr;
HWND g_splitter = nullptr;
int g_treeWidth = kInitialTreeWidth;
int g_selectedCategory = static_cast<int>(Category::All);
bool g_draggingSplitter = false;
std::wstring g_selectedCategoryName = L"All startup items";
HACCEL g_accelerators = nullptr;
std::vector<Entry> g_entries;

std::wstring ErrorText(DWORD error) {
    LPWSTR buffer = nullptr;
    const DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring result = size && buffer ? buffer : L"Windows error " + std::to_wstring(error);
    if (buffer) LocalFree(buffer);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' ')) result.pop_back();
    return result;
}

void Add(Entry entry) { g_entries.push_back(std::move(entry)); }

std::wstring RegistryString(HKEY key, const wchar_t* valueName) {
    DWORD type = 0, bytes = 0;
    if (RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t)) return L"";
    std::vector<wchar_t> value(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(key, valueName, nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &bytes) != ERROR_SUCCESS) return L"";
    return value.data();
}

void ScanRunKey(HKEY root, const wchar_t* rootName, REGSAM view, const wchar_t* viewName, const wchar_t* subkey) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ | view, &key) != ERROR_SUCCESS) return;
    DWORD count = 0, maxName = 0;
    if (RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &count, &maxName, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
        RegCloseKey(key); return;
    }
    std::vector<wchar_t> name(maxName + 2, L'\0');
    for (DWORD i = 0; i < count; ++i) {
        DWORD nameLen = static_cast<DWORD>(name.size());
        if (RegEnumValueW(key, i, name.data(), &nameLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) continue;
        const std::wstring command = RegistryString(key, name.data());
        const Category category = wcsstr(subkey, L"RunOnce") ? Category::RegistryRunOnce : Category::RegistryRun;
        Add({name.data(), category, L"Registry startup", std::wstring(rootName) + L"\\" + subkey + L" (" + viewName + L")",
             command, L"Enabled", L"Review — publisher and dependency evidence not assessed"});
    }
    RegCloseKey(key);
}

void ScanRegistry() {
    const wchar_t* paths[] = {
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce"
    };
    for (const auto* path : paths) {
        ScanRunKey(HKEY_CURRENT_USER, L"HKCU", KEY_WOW64_64KEY, L"64-bit view", path);
        ScanRunKey(HKEY_CURRENT_USER, L"HKCU", KEY_WOW64_32KEY, L"32-bit view", path);
        ScanRunKey(HKEY_LOCAL_MACHINE, L"HKLM", KEY_WOW64_64KEY, L"64-bit view", path);
        ScanRunKey(HKEY_LOCAL_MACHINE, L"HKLM", KEY_WOW64_32KEY, L"32-bit view", path);
    }
}

void ScanServices() {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (!manager) return;
    DWORD needed = 0, count = 0, resume = 0;
    EnumServicesStatusExW(manager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, nullptr, 0, &needed, &count, &resume, nullptr);
    if (GetLastError() != ERROR_MORE_DATA || needed == 0) { CloseServiceHandle(manager); return; }
    std::vector<BYTE> buffer(needed);
    resume = 0;
    if (!EnumServicesStatusExW(manager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, buffer.data(), needed, &needed, &count, &resume, nullptr)) {
        CloseServiceHandle(manager); return;
    }
    auto* services = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
    for (DWORD i = 0; i < count; ++i) {
        SC_HANDLE service = OpenServiceW(manager, services[i].lpServiceName, SERVICE_QUERY_CONFIG);
        if (!service) continue;
        DWORD configBytes = 0;
        QueryServiceConfigW(service, nullptr, 0, &configBytes);
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && configBytes) {
            std::vector<BYTE> configBuffer(configBytes);
            auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(configBuffer.data());
            if (QueryServiceConfigW(service, config, configBytes, &configBytes) &&
                (config->dwStartType == SERVICE_AUTO_START || config->dwStartType == SERVICE_BOOT_START || config->dwStartType == SERVICE_SYSTEM_START)) {
                std::wstring start = config->dwStartType == SERVICE_AUTO_START ? L"Automatic" :
                    config->dwStartType == SERVICE_BOOT_START ? L"Boot" : L"System";
                SERVICE_DELAYED_AUTO_START_INFO delayedInfo{};
                DWORD delayedBytes = 0;
                const bool delayed = QueryServiceConfig2W(service, SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
                    reinterpret_cast<BYTE*>(&delayedInfo), sizeof(delayedInfo), &delayedBytes) && delayedInfo.fDelayedAutostart;
                if (delayed) start += L" (delayed)";
                start += services[i].ServiceStatusProcess.dwCurrentState == SERVICE_RUNNING ? L" / Running" : L" / Not running";
                std::wstring command = config->lpBinaryPathName ? config->lpBinaryPathName : L"";
                if (config->lpDependencies && *config->lpDependencies) {
                    command += L"\nDependencies:";
                    for (const wchar_t* dependency = config->lpDependencies; *dependency; dependency += wcslen(dependency) + 1) {
                        command += L" ";
                        command += dependency;
                        command += L";";
                    }
                }
                Add({services[i].lpDisplayName, Category::Services, L"Windows service", services[i].lpServiceName,
                     command, start,
                     L"Review — Windows/service dependencies require validation"});
            }
        }
        CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
}

std::wstring BstrText(BSTR text) {
    if (!text) return L"";
    std::wstring result(text, SysStringLen(text));
    SysFreeString(text);
    return result;
}

void ScanTaskFolder(ITaskFolder* folder, const std::wstring& parent) {
    IRegisteredTaskCollection* tasks = nullptr;
    if (SUCCEEDED(folder->GetTasks(TASK_ENUM_HIDDEN, &tasks)) && tasks) {
        LONG count = 0;
        tasks->get_Count(&count);
        for (LONG i = 1; i <= count; ++i) {
            IRegisteredTask* task = nullptr;
            if (FAILED(tasks->get_Item(_variant_t(i), &task)) || !task) continue;
            BSTR taskName = nullptr, path = nullptr;
            task->get_Name(&taskName);
            task->get_Path(&path);
            VARIANT_BOOL enabled = VARIANT_FALSE;
            task->get_Enabled(&enabled);
            bool isStartup = false;
            ITaskDefinition* definition = nullptr;
            if (SUCCEEDED(task->get_Definition(&definition)) && definition) {
                ITriggerCollection* triggers = nullptr;
                if (SUCCEEDED(definition->get_Triggers(&triggers)) && triggers) {
                    LONG triggerCount = 0;
                    triggers->get_Count(&triggerCount);
                    for (LONG j = 1; j <= triggerCount; ++j) {
                        ITrigger* trigger = nullptr;
                        if (SUCCEEDED(triggers->get_Item(j, &trigger)) && trigger) {
                            TASK_TRIGGER_TYPE2 type{};
                            if (SUCCEEDED(trigger->get_Type(&type)) && (type == TASK_TRIGGER_BOOT || type == TASK_TRIGGER_LOGON)) isStartup = true;
                            trigger->Release();
                        }
                    }
                    triggers->Release();
                }
                definition->Release();
            }
            if (isStartup) {
                const std::wstring taskPath = BstrText(path);
                Add({BstrText(taskName), Category::Tasks, L"Scheduled task", taskPath.empty() ? parent : taskPath,
                     L"Boot/logon trigger", enabled == VARIANT_TRUE ? L"Enabled" : L"Disabled",
                     L"Review — task purpose and trigger conditions require validation"});
            } else {
                if (taskName) SysFreeString(taskName);
                if (path) SysFreeString(path);
            }
            task->Release();
        }
        tasks->Release();
    }
    ITaskFolderCollection* folders = nullptr;
    if (SUCCEEDED(folder->GetFolders(0, &folders)) && folders) {
        LONG count = 0;
        folders->get_Count(&count);
        for (LONG i = 1; i <= count; ++i) {
            ITaskFolder* child = nullptr;
            if (SUCCEEDED(folders->get_Item(_variant_t(i), &child)) && child) {
                BSTR childName = nullptr;
                child->get_Name(&childName);
                ScanTaskFolder(child, parent + L"\\" + BstrText(childName));
                child->Release();
            }
        }
        folders->Release();
    }
}

void ScanTasks() {
    ITaskService* service = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService, reinterpret_cast<void**>(&service));
    if (FAILED(hr) || !service) return;
    VARIANT empty;
    VariantInit(&empty);
    if (SUCCEEDED(service->Connect(empty, empty, empty, empty))) {
        ITaskFolder* root = nullptr;
        if (SUCCEEDED(service->GetFolder(_bstr_t(L"\\"), &root)) && root) {
            ScanTaskFolder(root, L"\\");
            root->Release();
        }
    }
    service->Release();
}

void ScanStartupFolder(REFKNOWNFOLDERID folderId, const wchar_t* label, Category category) {
    PWSTR folderPath = nullptr;
    if (FAILED(SHGetKnownFolderPath(folderId, KF_FLAG_DEFAULT, nullptr, &folderPath)) || !folderPath) return;
    const std::wstring path(folderPath);
    CoTaskMemFree(folderPath);
    const std::wstring pattern = path + L"\\*";
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0 || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        Add({data.cFileName, category, L"Startup folder", label, path + L"\\" + data.cFileName,
             L"Enabled", L"Review — shortcut target and user intent not assessed"});
    } while (FindNextFileW(find, &data));
    FindClose(find);
}

void ScanAll() {
    g_entries.clear();
    ScanRegistry();
    ScanServices();
    ScanTasks();
    ScanStartupFolder(FOLDERID_Startup, L"Current user Startup", Category::UserStartup);
    ScanStartupFolder(FOLDERID_CommonStartup, L"All users Startup", Category::CommonStartup);
}

void SetCell(int row, int column, const std::wstring& value) {
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = row;
    item.iSubItem = column;
    item.pszText = const_cast<wchar_t*>(value.c_str());
    ListView_SetItem(g_list, &item);
}

size_t Count(Category category) {
    size_t count = 0;
    for (const auto& entry : g_entries) if (entry.category == category) ++count;
    return count;
}

bool IsRegistryCategory(Category category) {
    return category == Category::RegistryRun || category == Category::RegistryRunOnce;
}

bool IsStartupFolderCategory(Category category) {
    return category == Category::UserStartup || category == Category::CommonStartup;
}

bool MatchesSelection(const Entry& entry) {
    const auto category = static_cast<Category>(g_selectedCategory);
    if (category == Category::All) return true;
    if (category == Category::RegistryGroup) return IsRegistryCategory(entry.category);
    if (category == Category::StartupGroup) return IsStartupFolderCategory(entry.category);
    return entry.category == category;
}

std::wstring CountedLabel(const wchar_t* label, size_t count) {
    return std::wstring(label) + L" (" + std::to_wstring(count) + L")";
}

HTREEITEM InsertTreeNode(HTREEITEM parent, const std::wstring& label, int category) {
    TVINSERTSTRUCTW insert{};
    insert.hParent = parent;
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask = TVIF_TEXT | TVIF_PARAM;
    insert.item.pszText = const_cast<wchar_t*>(label.c_str());
    insert.item.lParam = category;
    return TreeView_InsertItem(g_tree, &insert);
}

void PopulateTree() {
    TreeView_DeleteAllItems(g_tree);
    const HTREEITEM all = InsertTreeNode(TVI_ROOT, CountedLabel(L"All startup items", g_entries.size()), static_cast<int>(Category::All));
    InsertTreeNode(TVI_ROOT, CountedLabel(L"Services", Count(Category::Services)), static_cast<int>(Category::Services));
    const HTREEITEM registry = InsertTreeNode(TVI_ROOT, CountedLabel(L"Registry", Count(Category::RegistryRun) + Count(Category::RegistryRunOnce)), static_cast<int>(Category::RegistryGroup));
    InsertTreeNode(registry, CountedLabel(L"Run", Count(Category::RegistryRun)), static_cast<int>(Category::RegistryRun));
    InsertTreeNode(registry, CountedLabel(L"RunOnce", Count(Category::RegistryRunOnce)), static_cast<int>(Category::RegistryRunOnce));
    InsertTreeNode(TVI_ROOT, CountedLabel(L"Scheduled tasks", Count(Category::Tasks)), static_cast<int>(Category::Tasks));
    const HTREEITEM startup = InsertTreeNode(TVI_ROOT, CountedLabel(L"Startup folders", Count(Category::UserStartup) + Count(Category::CommonStartup)), static_cast<int>(Category::StartupGroup));
    InsertTreeNode(startup, CountedLabel(L"Current user", Count(Category::UserStartup)), static_cast<int>(Category::UserStartup));
    InsertTreeNode(startup, CountedLabel(L"All users", Count(Category::CommonStartup)), static_cast<int>(Category::CommonStartup));
    TreeView_Expand(g_tree, registry, TVE_EXPAND);
    TreeView_Expand(g_tree, startup, TVE_EXPAND);
    TreeView_SelectItem(g_tree, all);
}

void UpdateStatus() {
    if (!g_status) return;
    size_t visible = 0;
    for (const auto& entry : g_entries) if (MatchesSelection(entry)) ++visible;
    const std::wstring parts[] = {
        g_selectedCategoryName,
        std::to_wstring(visible) + L" items shown",
        std::to_wstring(g_entries.size()) + L" total"
    };
    for (int i = 0; i < 3; ++i) SendMessageW(g_status, SB_SETTEXTW, i, reinterpret_cast<LPARAM>(parts[i].c_str()));
}

void PopulateList() {
    if (!g_list) return;
    ListView_DeleteAllItems(g_list);
    int row = 0;
    for (const auto& entry : g_entries) {
        if (!MatchesSelection(entry)) continue;
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        item.pszText = const_cast<wchar_t*>(entry.name.c_str());
        ListView_InsertItem(g_list, &item);
        SetCell(row, 1, entry.kind);
        SetCell(row, 2, entry.state);
        SetCell(row, 3, entry.location);
        SetCell(row, 4, entry.startup);
        SetCell(row, 5, entry.assessment);
        ++row;
    }
    UpdateStatus();
}

void AddColumn(HWND list, int index, const wchar_t* title, int width) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = const_cast<wchar_t*>(title);
    column.cx = width;
    column.iSubItem = index;
    ListView_InsertColumn(list, index, &column);
}

void Refresh(HWND hwnd) {
    SetWindowTextW(hwnd, L"WinStartReview — scanning...");
    ScanAll();
    PopulateTree();
    PopulateList();
    SetWindowTextW(hwnd, (L"WinStartReview — " + std::to_wstring(g_entries.size()) + L" startup entries; review before changing").c_str());
}

void LayoutChildren(HWND hwnd) {
    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = client.right;
    const int height = client.bottom;
    const int toolbarHeight = 30;
    const int statusHeight = 24;
    if (g_toolbar) MoveWindow(g_toolbar, 0, 0, width, toolbarHeight, TRUE);
    if (g_status) MoveWindow(g_status, 0, height - statusHeight, width, statusHeight, TRUE);
    const int contentTop = toolbarHeight;
    const int contentHeight = std::max(0, height - toolbarHeight - statusHeight);
    const int splitX = std::min(std::max(g_treeWidth, 150), std::max(150, width - 250));
    g_treeWidth = splitX;
    if (g_tree) MoveWindow(g_tree, 0, contentTop, splitX, contentHeight, TRUE);
    if (g_splitter) MoveWindow(g_splitter, splitX, contentTop, 5, contentHeight, TRUE);
    if (g_list) MoveWindow(g_list, splitX + 5, contentTop, std::max(0, width - splitX - 5), contentHeight, TRUE);
    if (g_status) {
        int parts[] = {width / 2, width * 3 / 4, -1};
        SendMessageW(g_status, SB_SETPARTS, 3, reinterpret_cast<LPARAM>(parts));
    }
}

LRESULT CALLBACK SplitterProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    if (message == WM_LBUTTONDOWN) {
        g_draggingSplitter = true;
        SetCapture(GetParent(hwnd));
        return 0;
    }
    if (message == WM_SETCURSOR) {
        SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
        return TRUE;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

HMENU CreateMainMenu() {
    HMENU menu = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, kRefreshId, L"Refresh\tF5");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, kExitId, L"Exit");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"File");
    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, kRefreshId, L"Refresh\tF5");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"View");
    return menu;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        g_toolbar = CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr, WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS | CCS_TOP | CCS_NODIVIDER,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kToolbarId)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(g_toolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
        SendMessageW(g_toolbar, TB_LOADIMAGES, IDB_STD_SMALL_COLOR, reinterpret_cast<LPARAM>(HINST_COMMCTRL));
        const LRESULT refreshText = SendMessageW(g_toolbar, TB_ADDSTRINGW, 0, reinterpret_cast<LPARAM>(L"Refresh"));
        TBBUTTON refreshButton{};
        refreshButton.iBitmap = I_IMAGENONE;
        refreshButton.idCommand = kRefreshId;
        refreshButton.fsState = TBSTATE_ENABLED;
        refreshButton.fsStyle = TBSTYLE_BUTTON | BTNS_AUTOSIZE | BTNS_SHOWTEXT;
        refreshButton.iString = static_cast<INT_PTR>(refreshText);
        SendMessageW(g_toolbar, TB_ADDBUTTONS, 1, reinterpret_cast<LPARAM>(&refreshButton));

        g_status = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr, WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusId)), GetModuleHandleW(nullptr), nullptr);
        g_tree = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, nullptr,
            WS_CHILD | WS_VISIBLE | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTreeId)), GetModuleHandleW(nullptr), nullptr);
        g_splitter = CreateWindowExW(0, L"STATIC", nullptr, WS_CHILD | WS_VISIBLE | SS_NOTIFY,
            0, 0, 5, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSplitterId)), GetModuleHandleW(nullptr), nullptr);
        SetWindowSubclass(g_splitter, SplitterProc, 1, 0);
        g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kListId)), GetModuleHandleW(nullptr), nullptr);
        ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
        AddColumn(g_list, 0, L"Name", 210);
        AddColumn(g_list, 1, L"Source", 130);
        AddColumn(g_list, 2, L"State", 100);
        AddColumn(g_list, 3, L"Location", 260);
        AddColumn(g_list, 4, L"Startup command / trigger", 300);
        AddColumn(g_list, 5, L"Assessment", 330);
        LayoutChildren(hwnd);
        Refresh(hwnd);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == kRefreshId) Refresh(hwnd);
        else if (LOWORD(wParam) == kExitId) DestroyWindow(hwnd);
        return 0;
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (header->hwndFrom == g_tree && header->code == TVN_SELCHANGEDW) {
            const auto* change = reinterpret_cast<NMTREEVIEWW*>(lParam);
            g_selectedCategory = static_cast<int>(change->itemNew.lParam);
            wchar_t label[256]{};
            TVITEMW selected{};
            selected.mask = TVIF_TEXT;
            selected.hItem = change->itemNew.hItem;
            selected.pszText = label;
            selected.cchTextMax = static_cast<int>(std::size(label));
            if (TreeView_GetItem(g_tree, &selected)) g_selectedCategoryName = label;
            PopulateList();
            return 0;
        }
        return 0;
    }
    case WM_SIZE:
        LayoutChildren(hwnd);
        return 0;
    case WM_MOUSEMOVE:
        if (g_draggingSplitter) {
            g_treeWidth = static_cast<int>(static_cast<short>(LOWORD(lParam)));
            LayoutChildren(hwnd);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_draggingSplitter) {
            g_draggingSplitter = false;
            ReleaseCapture();
        }
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_F5) Refresh(hwnd);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comResult)) return 1;
    CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT,
        RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance;
    wc.lpfnWndProc = WindowProc;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassExW(&wc)) { CoUninitialize(); return 1; }
    HWND hwnd = CreateWindowExW(0, kClassName, L"WinStartReview", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1160, 760, nullptr, CreateMainMenu(), instance, nullptr);
    if (!hwnd) { CoUninitialize(); return 1; }
    ACCEL refreshAccel{FVIRTKEY, VK_F5, static_cast<WORD>(kRefreshId)};
    g_accelerators = CreateAcceleratorTableW(&refreshAccel, 1);
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (g_accelerators && TranslateAcceleratorW(hwnd, g_accelerators, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_accelerators) DestroyAcceleratorTable(g_accelerators);
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
