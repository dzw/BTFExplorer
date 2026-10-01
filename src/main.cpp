#define UNICODE
#define _UNICODE
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <propkey.h>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>
#include <filesystem>
#include <sstream>
#include <iomanip>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "OleAut32.lib")
#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Propsys.lib")

using std::wstring;

static constexpr int IDC_ADDRESS=1001, IDC_TREE=1002, IDC_LIST=1003;
static constexpr int IDC_BACK=1004, IDC_FORWARD=1005, IDC_UP=1006, IDC_REFRESH=1007;
static constexpr int IDC_FIRST=1010, IDC_PREV=1011, IDC_NEXT=1012, IDC_LAST=1013;
static constexpr int IDC_PAGE_SIZE=1014, IDC_STATUS=1015;

struct Entry {
    wstring path, name, type, modified;
    ULONGLONG size{};
    bool folder{};
};

static HWND gMain{}, gTree{}, gList{}, gAddress{}, gStatus{}, gPageSize{};
static HWND gBack{}, gForward{}, gUp{}, gRefresh{}, gFirst{}, gPrev{}, gNext{}, gLast{};
static HIMAGELIST gImages{};
static std::vector<Entry> gEntries;
static std::vector<wstring> gHistory;
static int gHistoryPos=-1, gPage=0, gPageSizeValue=100;
static bool gSortingAsc=true;
static int gSortColumn=0;
static wstring gCurrent;

static wstring FileTimeText(const FILETIME& ft) {
    SYSTEMTIME st{};
    FileTimeToSystemTime(&ft, &st);
    wchar_t b[64]; swprintf_s(b,L"%04d-%02d-%02d %02d:%02d",st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute);
    return b;
}

static wstring TypeText(const WIN32_FIND_DATAW& d) {
    SHFILEINFOW s{};
    SHGetFileInfoW(d.cFileName, FILE_ATTRIBUTE_NORMAL, &s, sizeof(s), SHGFI_TYPENAME);
    return s.szTypeName[0] ? s.szTypeName : (d.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY ? L"文件夹" : L"文件");
}

static ULONGLONG SizeOf(const WIN32_FIND_DATAW& d) {
    return (ULONGLONG(d.nFileSizeHigh)<<32) | d.nFileSizeLow;
}

static int PageCount() {
    return gEntries.empty() ? 1 : (int)((gEntries.size()+gPageSizeValue-1)/gPageSizeValue);
}

static void UpdateButtons() {
    bool canBack=gHistoryPos>0, canForward=gHistoryPos>=0 && gHistoryPos+1<(int)gHistory.size();
    EnableWindow(gBack,canBack); EnableWindow(gForward,canForward);
    EnableWindow(gUp,!gCurrent.empty());
    EnableWindow(gFirst,gPage>0); EnableWindow(gPrev,gPage>0);
    EnableWindow(gNext,gPage+1<PageCount()); EnableWindow(gLast,gPage+1<PageCount());
}

static void RefreshList() {
    int n=(int)gEntries.size();
    ListView_SetItemCountEx(gList, n ? min(gPageSizeValue, n-gPage*gPageSizeValue) : 0, LVSICF_NOINVALIDATEALL);
    ListView_RedrawItems(gList,0,max(0,min(gPageSizeValue,n-gPage*gPageSizeValue)-1));
    wchar_t s[256];
    swprintf_s(s,L"共 %zu 项    第 %d / %d 页",gEntries.size(),gPage+1,PageCount());
    SetWindowTextW(gStatus,s);
    UpdateButtons();
}

static void SortEntries() {
    std::sort(gEntries.begin(),gEntries.end(),[](const Entry&a,const Entry&b){
        if(a.folder!=b.folder) return a.folder>b.folder;
        if(gSortColumn==0) return gSortingAsc ? _wcsicmp(a.name.c_str(),b.name.c_str())<0 : _wcsicmp(a.name.c_str(),b.name.c_str())>0;
        if(gSortColumn==1) return gSortingAsc ? a.modified<b.modified : a.modified>b.modified;
        if(gSortColumn==2) return gSortingAsc ? _wcsicmp(a.type.c_str(),b.type.c_str())<0 : _wcsicmp(a.type.c_str(),b.type.c_str())>0;
        return gSortingAsc ? a.size<b.size : a.size>b.size;
    });
}

static void LoadDirectory(const wstring& dir, bool addHistory=true) {
    WIN32_FIND_DATAW d{};
    HANDLE h=FindFirstFileW((dir+L"\\*").c_str(),&d);
    if(h==INVALID_HANDLE_VALUE) return;
    gEntries.clear();
    do {
        if(wcscmp(d.cFileName,L".")==0 || wcscmp(d.cFileName,L"..")==0) continue;
        if(d.dwFileAttributes&FILE_ATTRIBUTE_HIDDEN) continue;
        Entry e;
        e.name=d.cFileName; e.path=dir+L"\\"+e.name;
        e.folder=(d.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
        e.size=e.folder?0:SizeOf(d); e.type=TypeText(d); e.modified=FileTimeText(d.ftLastWriteTime);
        gEntries.push_back(std::move(e));
    } while(FindNextFileW(h,&d));
    FindClose(h);
    gCurrent=dir; SetWindowTextW(gAddress,dir.c_str());
    SortEntries(); gPage=0;
    if(addHistory) {
        if(gHistoryPos+1<(int)gHistory.size()) gHistory.erase(gHistory.begin()+gHistoryPos+1,gHistory.end());
        gHistory.push_back(dir); gHistoryPos=(int)gHistory.size()-1;
    }
    RefreshList();
}

static void GoUp() {
    if(gCurrent.empty()) return;
    wchar_t p[MAX_PATH]; wcsncpy_s(p,gCurrent.c_str(),_TRUNCATE);
    PathRemoveFileSpecW(p); if(wcslen(p)) LoadDirectory(p);
}

static void NavigateHistory(int delta) {
    int p=gHistoryPos+delta;
    if(p<0 || p>=(int)gHistory.size()) return;
    gHistoryPos=p; LoadDirectory(gHistory[p],false);
}

static void OpenSelected() {
    int i=ListView_GetNextItem(gList,-1,LVNI_SELECTED); if(i<0) return;
    int idx=gPage*gPageSizeValue+i; if(idx<0 || idx>=(int)gEntries.size()) return;
    auto&e=gEntries[idx];
    if(e.folder) LoadDirectory(e.path); else ShellExecuteW(gMain,L"open",e.path.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
}

static void DeleteSelected() {
    int i=ListView_GetNextItem(gList,-1,LVNI_SELECTED); if(i<0) return;
    int idx=gPage*gPageSizeValue+i; if(idx<0 || idx>=(int)gEntries.size()) return;
    auto&e=gEntries[idx];
    if(MessageBoxW(gMain,(L"删除 "+e.name+L"？").c_str(),L"确认",MB_YESNO|MB_ICONQUESTION)!=IDYES) return;
    SHFILEOPSTRUCTW op{};
    wchar_t from[MAX_PATH]; wcsncpy_s(from,e.path.c_str(),_TRUNCATE);
    from[wcslen(from)+1]=0;
    op.hwnd=gMain; op.wFunc=FO_DELETE; op.pFrom=from; op.fFlags=FOF_ALLOWUNDO|FOF_NOCONFIRMATION|FOF_SILENT;
    if(SHFileOperationW(&op)==0) LoadDirectory(gCurrent,false);
}

static LRESULT CALLBACK ListProc(HWND,UINT,WPARAM,LPARAM);

static void InitList() {
    ListView_SetExtendedListViewStyle(gList,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER|LVS_EX_HEADERDRAGDROP);
    const wchar_t* cols[]={L"名称",L"修改日期",L"类型",L"大小"};
    int widths[]={330,150,150,110};
    for(int i=0;i<4;i++){ LVCOLUMNW c{LVCF_TEXT|LVCF_WIDTH}; c.cx=widths[i]; c.pszText=(LPWSTR)cols[i]; ListView_InsertColumn(gList,i,&c); }
    gImages=SHGetFileInfoW(L"C:\\",0,nullptr,0,SHGFI_SYSICONINDEX|SHGFI_SMALLICON);
    if(gImages) ListView_SetImageList(gList,gImages,LVSIL_SMALL);
}

static LRESULT CALLBACK WndProc(HWND h,UINT m,WPARAM w,LPARAM l) {
    switch(m) {
    case WM_CREATE: {
        gMain=h;
        gAddress=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,145,8,600,28,h,(HMENU)IDC_ADDRESS,nullptr,nullptr);
        gBack=CreateWindowW(L"BUTTON",L"←",WS_CHILD|WS_VISIBLE,8,8,38,28,h,(HMENU)IDC_BACK,nullptr,nullptr);
        gForward=CreateWindowW(L"BUTTON",L"→",WS_CHILD|WS_VISIBLE,48,8,38,28,h,(HMENU)IDC_FORWARD,nullptr,nullptr);
        gUp=CreateWindowW(L"BUTTON",L"↑",WS_CHILD|WS_VISIBLE,88,8,38,28,h,(HMENU)IDC_UP,nullptr,nullptr);
        gRefresh=CreateWindowW(L"BUTTON",L"⟳",WS_CHILD|WS_VISIBLE,750,8,38,28,h,(HMENU)IDC_REFRESH,nullptr,nullptr);
        gTree=CreateWindowExW(WS_EX_CLIENTEDGE,WC_TREEVIEWW,L"",WS_CHILD|WS_VISIBLE|TVS_HASLINES|TVS_LINESATROOT|TVS_HASBUTTONS,8,45,230,500,h,(HMENU)IDC_TREE,nullptr,nullptr);
        gList=CreateWindowExW(WS_EX_CLIENTEDGE,WC_LISTVIEWW,L"",WS_CHILD|WS_VISIBLE|LVS_REPORT|LVS_OWNERDATA|LVS_SHOWSELALWAYS,245,45,700,500,h,(HMENU)IDC_LIST,nullptr,nullptr);
        gStatus=CreateWindowW(L"STATIC",L"",WS_CHILD|WS_VISIBLE,8,550,450,25,h,(HMENU)IDC_STATUS,nullptr,nullptr);
        gFirst=CreateWindowW(L"BUTTON",L"⏮",WS_CHILD|WS_VISIBLE,520,548,42,28,h,(HMENU)IDC_FIRST,nullptr,nullptr);
        gPrev=CreateWindowW(L"BUTTON",L"◀",WS_CHILD|WS_VISIBLE,566,548,42,28,h,(HMENU)IDC_PREV,nullptr,nullptr);
        gNext=CreateWindowW(L"BUTTON",L"▶",WS_CHILD|WS_VISIBLE,612,548,42,28,h,(HMENU)IDC_NEXT,nullptr,nullptr);
        gLast=CreateWindowW(L"BUTTON",L"⏭",WS_CHILD|WS_VISIBLE,658,548,42,28,h,(HMENU)IDC_LAST,nullptr,nullptr);
        gPageSize=CreateWindowW(L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST,715,548,90,300,h,(HMENU)IDC_PAGE_SIZE,nullptr,nullptr);
        for(auto s:{L"50",L"100",L"200",L"500"}) SendMessageW(gPageSize,CB_ADDSTRING,0,(LPARAM)s);
        SendMessageW(gPageSize,CB_SETCURSEL,1,0);
        InitList();
        LoadDirectory(std::filesystem::current_path().wstring());
        break;
    }
    case WM_SIZE: {
        int W=LOWORD(l),H=HIWORD(l);
        MoveWindow(gAddress,145,8,max(100,W-200),28,TRUE); MoveWindow(gRefresh,W-48,8,40,28,TRUE);
        MoveWindow(gTree,8,45,230,max(100,H-85),TRUE); MoveWindow(gList,245,45,max(100,W-253),max(100,H-85),TRUE);
        MoveWindow(gStatus,8,H-35,450,28,TRUE);
        int x=W-440; MoveWindow(gFirst,x,H-37,42,28,TRUE); MoveWindow(gPrev,x+46,H-37,42,28,TRUE); MoveWindow(gNext,x+92,H-37,42,28,TRUE); MoveWindow(gLast,x+138,H-37,42,28,TRUE); MoveWindow(gPageSize,x+184,H-37,80,300,TRUE);
        break;
    }
    case WM_COMMAND:
        switch(LOWORD(w)) {
        case IDC_BACK: NavigateHistory(-1); break; case IDC_FORWARD: NavigateHistory(1); break; case IDC_UP: GoUp(); break;
        case IDC_REFRESH: LoadDirectory(gCurrent,false); break;
        case IDC_FIRST:gPage=0;RefreshList();break; case IDC_PREV:if(gPage>0)--gPage,RefreshList();break;
        case IDC_NEXT:if(gPage+1<PageCount())++gPage,RefreshList();break; case IDC_LAST:gPage=PageCount()-1;RefreshList();break;
        case IDC_PAGE_SIZE: if(HIWORD(w)==CBN_SELCHANGE){int a[]={50,100,200,500};gPageSizeValue=a[SendMessageW(gPageSize,CB_GETCURSEL,0,0)];gPage=0;RefreshList();}break;
        }
        break;
    case WM_NOTIFY: {
        auto*n=(LPNMHDR)l;
        if(n->idFrom==IDC_LIST) {
            if(n->code==LVN_GETDISPINFOW) {
                auto*di=(NMLVDISPINFOW*)l; int idx=gPage*gPageSizeValue+di->item.iItem;
                if(idx>=0&&idx<(int)gEntries.size()) {
                    auto&e=gEntries[idx];
                    if(di->item.mask&LVIF_TEXT){
                        static wchar_t b[128]; const wchar_t* t=L"";
                        if(di->item.iSubItem==0)t=e.name.c_str(); else if(di->item.iSubItem==1)t=e.modified.c_str(); else if(di->item.iSubItem==2)t=e.type.c_str();
                        else { swprintf_s(b,L"%llu KB",(unsigned long long)((e.size+1023)/1024)); t=b; }
                        wcsncpy_s(di->item.pszText,di->item.cchTextMax,t,_TRUNCATE);
                    }
                    if(di->item.mask&LVIF_IMAGE){ SHFILEINFOW s{};SHGetFileInfoW(e.path.c_str(),e.folder?FILE_ATTRIBUTE_DIRECTORY:FILE_ATTRIBUTE_NORMAL,&s,sizeof(s),SHGFI_SYSICONINDEX|SHGFI_SMALLICON);di->item.iImage=s.iIcon; }
                }
            } else if(n->code==NM_DBLCLK) OpenSelected();
            else if(n->code==LVN_COLUMNCLICK){auto*x=(NMLISTVIEW*)l; if(gSortColumn==x->iSubItem)gSortingAsc=!gSortingAsc;else{gSortColumn=x->iSubItem;gSortingAsc=true;}SortEntries();gPage=0;RefreshList();}
            else if(n->code==LVN_KEYDOWN){auto*k=(LPNMLVKEYDOWN)l;if(k->wVKey==VK_F2){int i=ListView_GetNextItem(gList,-1,LVNI_SELECTED);if(i>=0){int idx=gPage*gPageSizeValue+i; if(idx<(int)gEntries.size()){wstring np=gEntries[idx].path; wchar_t buf[MAX_PATH];wcsncpy_s(buf,gEntries[idx].name.c_str(),_TRUNCATE);if(SHFileOperationW(nullptr,FO_RENAME,buf,nullptr,FOF_NOCONFIRMATION,nullptr,nullptr)==0)LoadDirectory(gCurrent,false);}}} else if(k->wVKey==VK_DELETE)DeleteSelected();}
        }
        break;
    }
    case WM_DESTROY: PostQuitMessage(0); break;
    }
    return DefWindowProcW(h,m,w,l);
}

int WINAPI wWinMain(HINSTANCE hi,HINSTANCE, PWSTR,int cmd) {
    INITCOMMONCONTROLSEX ic{sizeof(ic),ICC_WIN95_CLASSES|ICC_LISTVIEW_CLASSES|ICC_TREEVIEW_CLASSES}; InitCommonControlsEx(&ic);
    OleInitialize(nullptr);
    WNDCLASSW wc{};wc.hInstance=hi;wc.lpfnWndProc=WndProc;wc.lpszClassName=L"PagedExplorerWnd";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);RegisterClassW(&wc);
    HWND h=CreateWindowW(wc.lpszClassName,L"Paged Explorer",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,1000,650,nullptr,nullptr,hi,nullptr);
    ShowWindow(h,cmd);UpdateWindow(h);
    MSG msg;while(GetMessageW(&msg,nullptr,0,0)){TranslateMessage(&msg);DispatchMessageW(&msg);}OleUninitialize();return 0;
}
