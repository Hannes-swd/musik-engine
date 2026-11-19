#include "FileDialog.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <vector>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

namespace {
std::wstring widen(const char* s, size_t len) {
    if (len == 0) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s, int(len), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, int(len), w.data(), n);
    return w;
}

std::wstring widen(const std::string& s) { return widen(s.data(), s.size()); }

// Filter enthält \0-Trenner, daher bis zum doppelten \0 umwandeln
std::wstring widenFilter(const char* f) {
    size_t len = 0;
    while (f[len] || f[len + 1]) ++len;
    return widen(f, len + 2);
}

std::string narrow(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string run(bool save, void* hwnd, const char* filter, const char* defExt, const std::string& defName, const char* title) {
    std::vector<wchar_t> buf(4096, L'\0');
    std::wstring name = widen(defName);
    if (!name.empty() && name.size() < buf.size()) std::copy(name.begin(), name.end(), buf.begin());
    std::wstring wFilter = widenFilter(filter);
    std::wstring wExt = defExt ? widen(defExt) : std::wstring();
    std::wstring wTitle = widen(title);

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd ? glfwGetWin32Window(static_cast<GLFWwindow*>(hwnd)) : nullptr;
    ofn.lpstrFilter = wFilter.c_str();
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = DWORD(buf.size());
    ofn.lpstrTitle = wTitle.c_str();
    ofn.lpstrDefExt = wExt.empty() ? nullptr : wExt.c_str();
    ofn.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    return ok ? narrow(buf.data()) : std::string();
}
}

std::string openFileDialog(void* hwnd, const char* filter, const char* title) {
    return run(false, hwnd, filter, nullptr, "", title);
}

std::string executableDir() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    if (n == 0) return ".";
    std::wstring w(buf, n);
    auto pos = w.find_last_of(L"\\/");
    if (pos != std::wstring::npos) w.resize(pos);
    return narrow(w.c_str());
}

std::string saveFileDialog(void* hwnd, const char* filter, const char* defaultExt, const std::string& defaultName,
                           const char* title) {
    return run(true, hwnd, filter, defaultExt, defaultName, title);
}

#else
std::string executableDir() { return "."; }
std::string openFileDialog(void*, const char*, const char*) { return {}; }
std::string saveFileDialog(void*, const char*, const char*, const std::string& name, const char*) { return name; }
#endif
