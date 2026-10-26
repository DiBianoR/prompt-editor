// Prompt Editor: navigate and edit long system prompts by topic.
// Usage: PromptEditor.exe [document [index]]
//   The index defaults to <document stem>.index.json next to the document.

#include "MainWindow.h"

#include <shellapi.h>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' "  \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' " \
                        "language='*'\"")

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    MainWindow window;
    if (!window.create(instance, show)) return 1;

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 2) window.open(argv[1], argc >= 3 ? std::filesystem::path(argv[2]) : std::filesystem::path());
    LocalFree(argv);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (window.findDialog() && IsDialogMessageW(window.findDialog(), &msg)) continue;
        if (TranslateAcceleratorW(window.hwnd(), window.accelerators(), &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
