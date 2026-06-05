#include "PeerSettingsDialog.h"

#include "NetworkManager.h"
#include "PeerConfig.h"
#include "ThreadUtil.h"
#include "UiTheme.h"
#include "resource.h"

#include <commctrl.h>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace clipsync {

namespace {

constexpr wchar_t kDialogClass[] = L"LANClipSyncPeerSettings";

constexpr int IDC_LOCAL_ADDR = 101;
constexpr int IDC_PEER_LIST = 102;
constexpr int IDC_PEER_INPUT = 104;
constexpr int IDC_BTN_COPY = 109;
constexpr int IDC_BTN_ADD = 105;
constexpr int IDC_BTN_REMOVE = 106;
constexpr int IDC_BTN_REFRESH = 110;
constexpr int IDC_BTN_SAVE = 107;
constexpr int IDC_BTN_CANCEL = 108;
constexpr int IDC_STATUS_BAR = 111;
constexpr int IDC_SEP1 = 112;

constexpr int ID_HDR_LOCAL = 1000;
constexpr int ID_CAP_LOCAL = 1001;
constexpr int ID_HDR_PEERS = 1002;

constexpr UINT WM_PROBE_UPDATE = WM_APP + 20;

NetworkManager* g_network = nullptr;
HWND g_list = nullptr;
HWND g_statusBar = nullptr;
HWND g_dialog = nullptr;
std::vector<PeerEndpoint> g_editingPeers;
std::vector<PeerStatus> g_displayStatus;
bool g_probeRunning = false;

HMENU ctrlId(int id) { return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)); }

void setFont(HWND hwnd, HFONT font) {
    if (hwnd && font) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

void updateSummaryText() {
    if (!g_statusBar) {
        return;
    }
    size_t online = 0;
    for (const auto& row : g_displayStatus) {
        if (row.state == PeerLinkState::Online) {
            ++online;
        }
    }
    std::wstring text;
    if (g_editingPeers.empty()) {
        text = L"尚未添加对端";
    } else if (online == g_editingPeers.size()) {
        text = L"全部已连接（" + std::to_wstring(online) + L"/" +
               std::to_wstring(g_editingPeers.size()) + L"）";
    } else {
        text = std::to_wstring(online) + L"/" + std::to_wstring(g_editingPeers.size()) + L" 对端在线";
    }
    SetWindowTextW(g_statusBar, text.c_str());
}

void fillListView() {
    if (!g_list) {
        return;
    }
    ListView_DeleteAllItems(g_list);

    for (size_t i = 0; i < g_editingPeers.size(); ++i) {
        const auto& peer = g_editingPeers[i];
        const std::wstring addr = formatPeerAddress(peer);
        std::wstring stateText = L"—";
        if (i < g_displayStatus.size()) {
            stateText = peerLinkStateText(g_displayStatus[i].state);
        }

        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<wchar_t*>(stateText.c_str());
        ListView_InsertItem(g_list, &item);
        ListView_SetItemText(g_list, static_cast<int>(i), 1, const_cast<wchar_t*>(addr.c_str()));
    }
    updateSummaryText();
}

void startProbe(HWND hwnd);

struct ProbeDialogCtx {
    HWND hwnd;
    NetworkManager* net;
    std::vector<PeerEndpoint> peers;
};

DWORD WINAPI probeDialogThreadProc(LPVOID param) {
    auto* ctx = static_cast<ProbeDialogCtx*>(param);
    const auto result = ctx->net->probePeers(ctx->peers, 1000);
    if (IsWindow(ctx->hwnd)) {
        auto* heap = new std::vector<PeerStatus>(result);
        PostMessageW(ctx->hwnd, WM_PROBE_UPDATE, 0, reinterpret_cast<LPARAM>(heap));
    }
    delete ctx;
    return 0;
}

void persistEditingPeers() {
    if (g_network) {
        g_network->setPeers(g_editingPeers);
    }
}

bool endpointIdentityEqual(const PeerEndpoint& a, const PeerEndpoint& b) {
    if (a.tcpPort != b.tcpPort) {
        return false;
    }
    return _stricmp(a.keyAddress().c_str(), b.keyAddress().c_str()) == 0;
}

void removePeerAt(HWND hwnd, int sel, bool confirm) {
    if (sel < 0 || sel >= static_cast<int>(g_editingPeers.size())) {
        return;
    }
    if (confirm) {
        const std::wstring addr = formatPeerAddress(g_editingPeers[static_cast<size_t>(sel)]);
        const std::wstring msg = L"删除对端 " + addr + L" ？";
        if (MessageBoxW(hwnd, msg.c_str(), L"确认", MB_YESNO | MB_ICONQUESTION) != IDYES) {
            return;
        }
    }
    g_editingPeers.erase(g_editingPeers.begin() + sel);
    if (sel < static_cast<int>(g_displayStatus.size())) {
        g_displayStatus.erase(g_displayStatus.begin() + sel);
    }
    fillListView();
    persistEditingPeers();
}

bool addPeerFromInput(HWND hwnd) {
    wchar_t buf[256]{};
    GetDlgItemTextW(hwnd, IDC_PEER_INPUT, buf, 255);
    PeerEndpoint ep;
    if (!parsePeerAddress(buf, ep)) {
        MessageBoxW(hwnd,
                    L"格式：计算机名或 IP[:端口]\n"
                    L"支持分号粘贴：10.0.0.1:12123;计算机名:12123\n"
                    L"IPv6 请用 [fe80::1%33]:12123\n"
                    L"省略端口时将依次尝试 12123、12345、12306",
                    L"格式错误", MB_OK | MB_ICONWARNING);
        return false;
    }
    for (const auto& existing : g_editingPeers) {
        if (endpointIdentityEqual(existing, ep)) {
            MessageBoxW(hwnd, L"该对端已在列表中", L"提示", MB_OK | MB_ICONINFORMATION);
            return false;
        }
    }
    if (ep.tcpPort == 0) {
        if (!g_network || !g_network->discoverPeerPort(ep)) {
            MessageBoxW(hwnd,
                        L"在约定端口 12123、12345、12306 上均未连通。\n"
                        L"请确认对端已启动，或手动指定端口。",
                        L"未找到对端", MB_OK | MB_ICONWARNING);
            return false;
        }
    }
    g_editingPeers.push_back(ep);
    g_displayStatus.push_back(PeerStatus{ep, PeerLinkState::Unknown});
    SetDlgItemTextW(hwnd, IDC_PEER_INPUT, L"");
    fillListView();
    persistEditingPeers();
    startProbe(hwnd);
    return true;
}

void handleListDoubleClick(HWND hwnd, int index) {
    if (index < 0 || index >= static_cast<int>(g_editingPeers.size())) {
        return;
    }
    PeerLinkState state = PeerLinkState::Unknown;
    if (index < static_cast<int>(g_displayStatus.size())) {
        state = g_displayStatus[static_cast<size_t>(index)].state;
    }
    if (state == PeerLinkState::Online) {
        removePeerAt(hwnd, index, true);
        return;
    }
    if (!g_network) {
        return;
    }
    PeerEndpoint& peer = g_editingPeers[static_cast<size_t>(index)];
    if (g_network->discoverPeerPort(peer)) {
        persistEditingPeers();
        fillListView();
        startProbe(hwnd);
        const std::wstring msg =
            L"已检测到端口 " + std::to_wstring(peer.tcpPort) + L"，正在刷新状态";
        SetWindowTextW(g_statusBar, msg.c_str());
    } else {
        MessageBoxW(hwnd,
                    L"在约定端口 12123、12345、12306 上未连通。\n"
                    L"请检查对端程序是否已运行、防火墙是否放行。",
                    L"未连通", MB_OK | MB_ICONWARNING);
    }
}

void startProbe(HWND hwnd) {
    if (!g_network || g_probeRunning) {
        return;
    }
    g_probeRunning = true;
    for (size_t i = 0; i < g_editingPeers.size(); ++i) {
        if (i >= g_displayStatus.size()) {
            g_displayStatus.resize(g_editingPeers.size());
        }
        g_displayStatus[i].endpoint = g_editingPeers[i];
        g_displayStatus[i].state = PeerLinkState::Checking;
    }
    fillListView();

    auto* ctx = new ProbeDialogCtx{hwnd, g_network, g_editingPeers};
    HANDLE th = createSmallStackThread(probeDialogThreadProc, ctx, 192 * 1024);
    if (!th) {
        g_probeRunning = false;
        delete ctx;
    } else {
        CloseHandle(th);
    }
}

// 根据客户区自适应布局：顶栏固定，列表伸缩，底栏锚定
void layoutControls(HWND hwnd) {
    RECT cr{};
    GetClientRect(hwnd, &cr);
    int clientW = cr.right;
    int clientH = cr.bottom;
    if (clientW < UiTheme::scale(hwnd, 320)) {
        clientW = UiTheme::scale(hwnd, 468);
    }
    if (clientH < UiTheme::scale(hwnd, 200)) {
        clientH = UiTheme::scale(hwnd, 420);
    }

    const int m = UiTheme::scale(hwnd, 24);
    const int w = clientW - m * 2;
    const int gap = UiTheme::scale(hwnd, 8);
    const int sectionGap = UiTheme::scale(hwnd, 20);
    const int rowH = UiTheme::scale(hwnd, 32);
    const int captionH = UiTheme::scale(hwnd, 36);
    const int sectionH = UiTheme::scale(hwnd, 22);
    const int statusH = UiTheme::scale(hwnd, 20);
    const int btnW = UiTheme::scale(hwnd, 96);
    const int btnH = UiTheme::scale(hwnd, 32);
    const int smallBtnW = UiTheme::scale(hwnd, 88);
    const int sepH = UiTheme::scale(hwnd, 1);
    const int footerH = btnH + m;

    auto place = [](HWND ctrl, int x, int y, int cw, int ch) {
        if (ctrl) {
            SetWindowPos(ctrl, nullptr, x, y, cw, ch, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    };

    HWND hdrLocal = GetDlgItem(hwnd, ID_HDR_LOCAL);
    HWND capLocal = GetDlgItem(hwnd, ID_CAP_LOCAL);
    HWND editLocal = GetDlgItem(hwnd, IDC_LOCAL_ADDR);
    HWND btnCopy = GetDlgItem(hwnd, IDC_BTN_COPY);
    HWND sep1 = GetDlgItem(hwnd, IDC_SEP1);
    HWND hdrPeers = GetDlgItem(hwnd, ID_HDR_PEERS);
    HWND list = GetDlgItem(hwnd, IDC_PEER_LIST);
    HWND statusBar = GetDlgItem(hwnd, IDC_STATUS_BAR);
    HWND editPeer = GetDlgItem(hwnd, IDC_PEER_INPUT);
    HWND btnAdd = GetDlgItem(hwnd, IDC_BTN_ADD);
    HWND btnRemove = GetDlgItem(hwnd, IDC_BTN_REMOVE);
    HWND btnRefresh = GetDlgItem(hwnd, IDC_BTN_REFRESH);
    HWND btnSave = GetDlgItem(hwnd, IDC_BTN_SAVE);
    HWND btnCancel = GetDlgItem(hwnd, IDC_BTN_CANCEL);

    int y = m;

    place(hdrLocal, m, y, w, sectionH);
    y += sectionH + gap;
    place(capLocal, m, y, w, captionH);
    y += captionH + gap;

    const int copyW = UiTheme::scale(hwnd, 80);
    place(editLocal, m, y, w - copyW - gap, rowH);
    place(btnCopy, m + w - copyW, y, copyW, rowH);
    y += rowH + sectionGap;

    place(sep1, m, y, w, sepH);
    y += sepH + sectionGap;

    place(hdrPeers, m, y, w, sectionH);
    y += sectionH + gap;

    const int footerTop = clientH - footerH;
    const int inputRowTop = footerTop - gap - rowH;
    const int toolRowTop = inputRowTop - gap - btnH;
    const int statusTop = toolRowTop - gap - statusH;
    const int listTop = y;
    const int listH = statusTop - gap - listTop;
    const int minListH = UiTheme::scale(hwnd, 72);

    g_list = list;
    place(list, m, listTop, w, listH > minListH ? listH : minListH);
    place(statusBar, m, statusTop, w, statusH);
    g_statusBar = statusBar;

    const int addW = UiTheme::scale(hwnd, 80);
    place(editPeer, m, inputRowTop, w - addW - gap, rowH);
    place(btnAdd, m + w - addW, inputRowTop, addW, rowH);

    place(btnRemove, m, toolRowTop, smallBtnW, btnH);
    place(btnRefresh, m + smallBtnW + gap, toolRowTop, smallBtnW, btnH);

    place(btnSave, m + w - btnW * 2 - gap, footerTop + (footerH - btnH) / 2, btnW, btnH);
    place(btnCancel, m + w - btnW, footerTop + (footerH - btnH) / 2, btnW, btnH);

    if (list && listH > minListH) {
        ListView_SetColumnWidth(list, 1, w - UiTheme::scale(hwnd, 88));
    }
}

int minClientHeight(HWND hwnd) {
    const int m = UiTheme::scale(hwnd, 24);
    const int gap = UiTheme::scale(hwnd, 8);
    const int sectionGap = UiTheme::scale(hwnd, 20);
    const int rowH = UiTheme::scale(hwnd, 32);
    const int captionH = UiTheme::scale(hwnd, 36);
    const int sectionH = UiTheme::scale(hwnd, 22);
    const int statusH = UiTheme::scale(hwnd, 20);
    const int btnH = UiTheme::scale(hwnd, 32);
    const int sepH = UiTheme::scale(hwnd, 1);
    const int listMin = UiTheme::scale(hwnd, 100);
    const int footerH = btnH + m;

    return m + sectionH + gap + captionH + gap + rowH + sectionGap + sepH + sectionGap + sectionH +
           gap + listMin + gap + statusH + gap + rowH + gap + btnH + gap + footerH + m;
}

LRESULT onCustomDrawList(NMHDR* nmhdr) {
    auto* nmcd = reinterpret_cast<NMLVCUSTOMDRAW*>(nmhdr);
    switch (nmcd->nmcd.dwDrawStage) {
        case CDDS_PREPAINT:
            return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT:
            return CDRF_NOTIFYSUBITEMDRAW;
        case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
            if (nmcd->iSubItem == 0) {
                const int idx = static_cast<int>(nmcd->nmcd.dwItemSpec);
                if (idx >= 0 && idx < static_cast<int>(g_displayStatus.size())) {
                    switch (g_displayStatus[static_cast<size_t>(idx)].state) {
                        case PeerLinkState::Online:
                            nmcd->clrText = UiTheme::colorOnline();
                            break;
                        case PeerLinkState::Offline:
                            nmcd->clrText = UiTheme::colorOffline();
                            break;
                        case PeerLinkState::Checking:
                            nmcd->clrText = UiTheme::colorChecking();
                            break;
                        default:
                            nmcd->clrText = UiTheme::colorTextSecondary();
                            break;
                    }
                }
            }
            return CDRF_DODEFAULT;
        default:
            break;
    }
    return CDRF_DODEFAULT;
}

LRESULT CALLBACK dialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_dialog = hwnd;
            const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            const HINSTANCE inst = static_cast<HINSTANCE>(cs->hInstance);

            HWND hdrLocal = CreateWindowExW(0, L"STATIC", L"本机", WS_CHILD | WS_VISIBLE, 0, 0, 0,
                                            0, hwnd, ctrlId(ID_HDR_LOCAL), inst, nullptr);
            HWND capLocal = CreateWindowExW(
                0, L"STATIC",
                L"将下方地址发给对方；可添加计算机名或 IP（域环境推荐计算机名，IP 变化仍可同步）。",
                WS_CHILD | WS_VISIBLE,
                0, 0, 0, 0, hwnd, ctrlId(ID_CAP_LOCAL), inst, nullptr);
            HWND editLocal = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_READONLY | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd,
                ctrlId(IDC_LOCAL_ADDR), inst, nullptr);
            HWND btnCopy = CreateWindowExW(0, L"BUTTON", L"复制",
                                           WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0,
                                           hwnd, ctrlId(IDC_BTN_COPY), inst, nullptr);
            CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ, 0, 0, 0, 0,
                            hwnd, ctrlId(IDC_SEP1), inst, nullptr);

            HWND hdrPeers = CreateWindowExW(0, L"STATIC", L"对端", WS_CHILD | WS_VISIBLE, 0, 0, 0,
                                            0, hwnd, ctrlId(ID_HDR_PEERS), inst, nullptr);
            g_list = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                     WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL |
                                         LVS_SHOWSELALWAYS,
                                     0, 0, 0, 0, hwnd, ctrlId(IDC_PEER_LIST), inst, nullptr);
            ListView_SetExtendedListViewStyle(
                g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);

            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            col.pszText = const_cast<wchar_t*>(L"状态");
            col.cx = UiTheme::scale(hwnd, 80);
            ListView_InsertColumn(g_list, 0, &col);
            col.pszText = const_cast<wchar_t*>(L"地址");
            col.cx = UiTheme::scale(hwnd, 300);
            ListView_InsertColumn(g_list, 1, &col);

            g_statusBar = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                                          hwnd, ctrlId(IDC_STATUS_BAR), inst, nullptr);

            HWND editPeer = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0,
                                            hwnd, ctrlId(IDC_PEER_INPUT), inst, nullptr);
            SendMessageW(editPeer, EM_SETCUEBANNER, FALSE,
                         reinterpret_cast<LPARAM>(L"计算机名或 IP[:端口]，可分号粘贴整行"));

            CreateWindowExW(0, L"BUTTON", L"添加", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0,
                            0, hwnd, ctrlId(IDC_BTN_ADD), inst, nullptr);
            CreateWindowExW(0, L"BUTTON", L"删除", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0,
                            0, hwnd, ctrlId(IDC_BTN_REMOVE), inst, nullptr);
            CreateWindowExW(0, L"BUTTON", L"刷新状态", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0,
                            0, 0, 0, hwnd, ctrlId(IDC_BTN_REFRESH), inst, nullptr);
            CreateWindowExW(0, L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 0, 0,
                            0, 0, hwnd, ctrlId(IDC_BTN_SAVE), inst, nullptr);
            CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0,
                            0, hwnd, ctrlId(IDC_BTN_CANCEL), inst, nullptr);

            setFont(hdrLocal, UiTheme::fontSection());
            setFont(hdrPeers, UiTheme::fontSection());
            setFont(capLocal, UiTheme::fontCaption());
            setFont(g_statusBar, UiTheme::fontCaption());

            for (HWND child = GetWindow(hwnd, GW_CHILD); child;
                 child = GetWindow(child, GW_HWNDNEXT)) {
                const int id = GetDlgCtrlID(child);
                if (id != ID_HDR_LOCAL && id != ID_HDR_PEERS && id != ID_CAP_LOCAL &&
                    id != IDC_STATUS_BAR) {
                    setFont(child, UiTheme::fontBody());
                }
            }

            if (g_network) {
                SetDlgItemTextW(
                    hwnd, IDC_LOCAL_ADDR,
                    formatLocalListenAddresses(g_network->localIPv4(), g_network->localIPv6(),
                                               localComputerNameUtf8(), g_network->listenPort())
                        .c_str());
            }

            g_displayStatus.clear();
            for (const auto& p : g_editingPeers) {
                g_displayStatus.push_back(PeerStatus{p, PeerLinkState::Unknown});
            }

            const int cw = UiTheme::scale(hwnd, 468);
            const int ch = minClientHeight(hwnd);
            UiTheme::resizeWindowToClient(hwnd, cw, ch);
            layoutControls(hwnd);
            fillListView();
            startProbe(hwnd);
            return 0;
        }

        case WM_PROBE_UPDATE: {
            g_probeRunning = false;
            auto* data = reinterpret_cast<std::vector<PeerStatus>*>(lParam);
            if (data) {
                g_displayStatus = *data;
                delete data;
            }
            fillListView();
            return 0;
        }

        case WM_KEYDOWN:
            if (wParam == VK_RETURN && GetDlgCtrlID(GetFocus()) == IDC_PEER_INPUT) {
                addPeerFromInput(hwnd);
                return 0;
            }
            return 0;

        case WM_NOTIFY: {
            const auto* hdr = reinterpret_cast<NMHDR*>(lParam);
            if (hdr->idFrom != IDC_PEER_LIST) {
                return 0;
            }
            if (hdr->code == NM_CUSTOMDRAW) {
                return onCustomDrawList(const_cast<NMHDR*>(hdr));
            }
            if (hdr->code == NM_DBLCLK) {
                const auto* item = reinterpret_cast<const NMITEMACTIVATE*>(lParam);
                if (item->iItem < 0) {
                    addPeerFromInput(hwnd);
                } else {
                    handleListDoubleClick(hwnd, item->iItem);
                }
                return 0;
            }
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            const HWND ctrl = reinterpret_cast<HWND>(lParam);
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(dc, TRANSPARENT);
            const int id = GetDlgCtrlID(ctrl);
            if (id == ID_CAP_LOCAL || id == IDC_STATUS_BAR) {
                SetTextColor(dc, UiTheme::colorTextSecondary());
            } else {
                SetTextColor(dc, UiTheme::colorText());
            }
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }

        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = UiTheme::scale(hwnd, 400);
            info->ptMinTrackSize.y = minClientHeight(hwnd);
            return 0;
        }

        case WM_SIZE:
            layoutControls(hwnd);
            return 0;

        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            if (id == IDC_BTN_COPY) {
                if (OpenClipboard(hwnd)) {
                    EmptyClipboard();
                    wchar_t buf[256]{};
                    GetDlgItemTextW(hwnd, IDC_LOCAL_ADDR, buf, 255);
                    const size_t bytes = (wcslen(buf) + 1) * sizeof(wchar_t);
                    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
                    if (mem) {
                        void* locked = GlobalLock(mem);
                        memcpy(locked, buf, bytes);
                        GlobalUnlock(mem);
                        SetClipboardData(CF_UNICODETEXT, mem);
                    }
                    CloseClipboard();
                }
                return 0;
            }
            if (id == IDC_BTN_ADD) {
                addPeerFromInput(hwnd);
                return 0;
            }
            if (id == IDC_BTN_REMOVE) {
                const int sel = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
                if (sel < 0) {
                    MessageBoxW(hwnd, L"请先选中要删除的对端，或双击已连接行删除", L"提示",
                                MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                removePeerAt(hwnd, sel, true);
                return 0;
            }
            if (id == IDC_BTN_REFRESH) {
                startProbe(hwnd);
                return 0;
            }
            if (id == IDC_BTN_SAVE) {
                persistEditingPeers();
                DestroyWindow(hwnd);
                return 0;
            }
            if (id == IDC_BTN_CANCEL) {
                DestroyWindow(hwnd);
                return 0;
            }
            return 0;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            g_list = nullptr;
            g_statusBar = nullptr;
            g_dialog = nullptr;
            g_probeRunning = false;
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

void PeerSettingsDialog::show(HWND owner, NetworkManager* network) {
    UiTheme::initialize();

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = dialogProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = kDialogClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    g_network = network;
    g_editingPeers = network ? network->getPeers() : std::vector<PeerEndpoint>{};

    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    const int x = ownerRect.left + UiTheme::scale(owner, 48);
    const int y = ownerRect.top + UiTheme::scale(owner, 48);

    const DWORD style = WS_CAPTION | WS_SYSMENU | WS_VISIBLE | WS_THICKFRAME;
    const DWORD exStyle = WS_EX_DLGMODALFRAME;

    HWND dlg = CreateWindowExW(exStyle, kDialogClass, L"对端与连接", style, x, y,
                               UiTheme::scale(owner, 500), UiTheme::scale(owner, 200), owner,
                               nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!dlg) {
        return;
    }

    const HICON appIcon = static_cast<HICON>(LoadImageW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 0, 0,
        LR_DEFAULTSIZE | LR_SHARED));
    if (appIcon) {
        SendMessageW(dlg, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(appIcon));
        SendMessageW(dlg, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(appIcon));
    }

    UiTheme::enableWindowRoundCorner(dlg);
    EnableWindow(owner, FALSE);

    MSG msg{};
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    g_network = nullptr;
}

}  // namespace clipsync
