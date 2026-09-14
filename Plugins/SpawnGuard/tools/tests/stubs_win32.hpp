// Заглушки Win32 и логгера AsaApi для тестов логики SpawnGuard на Linux/WSL.
//
// Нужны только для того, чтобы вырезанный из Main.cpp блок кэша/имён хуков
// компилировался без Windows SDK. Имена, поведение и разделители путей
// повторяют настоящие Win32-функции (включая '\\' в путях).
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <dirent.h>
#include <sys/stat.h>
#include <cmath>

using DWORD = unsigned long;
using BOOL = int;
using HANDLE = void*;
constexpr DWORD MAX_PATH = 260;
constexpr unsigned long FILE_ATTRIBUTE_DIRECTORY = 0x10;
constexpr unsigned long FILE_ATTRIBUTE_NORMAL = 0x80;
constexpr DWORD GENERIC_READ = 0x80000000u;
constexpr DWORD FILE_SHARE_READ = 1;
constexpr DWORD FILE_SHARE_WRITE = 2;
constexpr DWORD OPEN_EXISTING = 3;
#define INVALID_HANDLE_VALUE ((HANDLE)(long long)-1)
constexpr int CP_UTF8 = 65001;
constexpr int GetFileExInfoStandard = 0;
#define FALSE 0

struct FILETIME { DWORD dwLowDateTime, dwHighDateTime; };
union ULARGE_INTEGER { unsigned long long QuadPart; struct { DWORD LowPart; DWORD HighPart; }; };
union LARGE_INTEGER { long long QuadPart; struct { DWORD LowPart; DWORD HighPart; }; };
struct WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes; FILETIME ftLastWriteTime; DWORD nFileSizeHigh, nFileSizeLow;
};
struct WIN32_FIND_DATAW { DWORD dwFileAttributes; wchar_t cFileName[MAX_PATH]; };

std::string g_test_root;
std::string narrow(const wchar_t* w) { std::string o; for (; w && *w; ++w) o.push_back((char)*w); return o; }
std::string unix_path(const std::string& s) { std::string o = s; for (size_t i = 0; i < o.size(); ++i) if (o[i] == '\\') o[i] = '/'; return o; }
std::wstring widen(const std::string& s) { std::wstring o; for (char c : s) o.push_back((wchar_t)(unsigned char)c); return o; }

int MultiByteToWideChar(int, DWORD, const char* src, int srcLen, wchar_t* dst, int dstLen) {
    if (!dst || dstLen <= 0) return srcLen;
    for (int i = 0; i < srcLen; ++i) dst[i] = (wchar_t)(unsigned char)src[i];
    return srcLen;
}
int WideCharToMultiByte(int, DWORD, const wchar_t* src, int srcLen, char* dst, int dstLen, const char*, BOOL*) {
    if (!dst || dstLen <= 0) return srcLen;
    for (int i = 0; i < srcLen; ++i) dst[i] = (char)src[i];
    return srcLen;
}
DWORD GetModuleFileNameW(void*, wchar_t* buffer, DWORD size) {
    std::string exe = g_test_root;
    if (exe.empty() || exe[exe.size() - 1] != '/') exe += '/';
    exe += "ArkAscendedServer.exe";                 // как на реальном сервере: ...\Win64\ArkAscendedServer.exe
    for (size_t i = 0; i < exe.size(); ++i) if (exe[i] == '/') exe[i] = '\\';
    std::wstring p = widen(exe);
    if (p.size() + 1 > size) return size;
    std::memcpy(buffer, p.c_str(), (p.size() + 1) * sizeof(wchar_t));
    return (DWORD)p.size();
}
BOOL GetFileAttributesExW(const wchar_t* path, int, WIN32_FILE_ATTRIBUTE_DATA* data) {
    struct stat st{};
    if (stat(unix_path(narrow(path)).c_str(), &st) != 0) return 0;
    data->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : 0;
    data->nFileSizeLow = (DWORD)st.st_size;
    data->nFileSizeHigh = 0;
    data->ftLastWriteTime.dwLowDateTime = (DWORD)st.st_mtime;
    data->ftLastWriteTime.dwHighDateTime = 0;
    return 1;
}
struct StubDir { DIR* dir; std::string path; };
std::map<HANDLE, StubDir> g_dirs;
int g_handle = 1;
HANDLE FindFirstFileW(const wchar_t* mask, WIN32_FIND_DATAW* data) {
    std::string m = unix_path(narrow(mask));
    std::string dir = m.substr(0, m.find_last_of('/'));
    DIR* d = opendir(dir.c_str());
    if (!d) return INVALID_HANDLE_VALUE;
    HANDLE h = (HANDLE)(long long)(++g_handle);
    g_dirs[h] = StubDir{ d, dir };
    struct dirent* e = readdir(d);
    if (!e) { closedir(d); g_dirs.erase(h); return INVALID_HANDLE_VALUE; }
    std::string name = e->d_name;
    std::wstring w = widen(name);
    std::memcpy(data->cFileName, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
    std::string full = dir + "/" + name;
    struct stat st{};
    data->dwFileAttributes = (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) ? FILE_ATTRIBUTE_DIRECTORY : 0;
    return h;
}
BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW* data) {
    auto it = g_dirs.find(h);
    if (it == g_dirs.end()) return 0;
    struct dirent* e = readdir(it->second.dir);
    if (!e) return 0;
    std::string name = e->d_name;
    std::wstring w = widen(name);
    std::memcpy(data->cFileName, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
    std::string full = it->second.path + "/" + name;
    struct stat st{};
    data->dwFileAttributes = (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) ? FILE_ATTRIBUTE_DIRECTORY : 0;
    return 1;
}
BOOL FindClose(HANDLE h) { auto it = g_dirs.find(h); if (it != g_dirs.end()) { closedir(it->second.dir); g_dirs.erase(it); } return 1; }
struct StubFile { std::string data; size_t offset = 0; };
std::map<HANDLE, StubFile> g_files;
HANDLE CreateFileW(const wchar_t* path, DWORD, DWORD, void*, DWORD, DWORD, void*) {
    std::ifstream in(unix_path(narrow(path)).c_str(), std::ios::binary);
    if (!in.is_open()) return INVALID_HANDLE_VALUE;
    StubFile f;
    f.data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    HANDLE h = (HANDLE)(long long)(++g_handle);
    g_files[h] = f;
    return h;
}
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER* size) {
    auto it = g_files.find(h);
    if (it == g_files.end()) return 0;
    size->QuadPart = (long long)it->second.data.size();
    return 1;
}
BOOL ReadFile(HANDLE h, void* buffer, DWORD toRead, DWORD* read, void*) {
    auto it = g_files.find(h);
    if (it == g_files.end()) return 0;
    const size_t avail = it->second.data.size() - it->second.offset;
    const size_t n = (toRead < avail) ? toRead : avail;
    std::memcpy(buffer, it->second.data.data() + it->second.offset, n);
    it->second.offset += n;
    *read = (DWORD)n;
    return 1;
}
BOOL CloseHandle(HANDLE h) { g_files.erase(h); return 1; }


namespace Log
{
    struct StubLogger
    {
        template <typename... A> void info(const char*, A&&...) const {}
        template <typename... A> void warn(const char*, A&&...) const {}
        template <typename... A> void error(const char*, A&&...) const {}
        void flush() const {}
    };
    inline StubLogger* GetLog() { static StubLogger logger; return &logger; }
}
