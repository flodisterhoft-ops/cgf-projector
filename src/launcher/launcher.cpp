/***************************************************************************
//
//    CGF Projector launcher
//    Copyright (C) 2026  CGF Vernon
//
//    This program is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation version 3 of the License.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
***************************************************************************/

// CGFProjector.exe in the install folder (docs/song-push.md, "App updates").
// Each app version lives in its own folder, app-<version>; the database and
// settings stay in the install folder. This starts the current version:
//
//   CGFProjector.exe [args]          the app, with --data-dir <install folder>
//   CGFProjector.exe --companion     the companion (at Windows sign-in)
//   ... --after <pid>                first wait for that process to end
//
// A new version is on trial until it has run a full minute (it then writes
// cgf-healthy.txt). If it stops with an error in its first 20 seconds, or
// never runs a full minute in three starts, the previous version comes back
// and the new one is never installed again. Plain Win32, linked statically:
// it must start even when the Qt libraries of a version are broken.

#include <windows.h>
#include <shellapi.h>

#include <cstdlib>
#include <map>
#include <string>
#include <vector>

using std::string;
using std::wstring;

namespace {

wstring root;

wstring widen(const string &s)
{
    if(s.empty())
        return wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
    return w;
}

string narrow(const wstring &w)
{
    if(w.empty())
        return string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

wstring join(const wstring &a, const wstring &b)
{
    return a + L"\\" + b;
}

bool isFile(const wstring &p)
{
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

string trim(const string &s)
{
    const size_t b = s.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
    if(b == string::npos)
        return string();
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

string readFile(const wstring &path)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if(h == INVALID_HANDLE_VALUE)
        return string();
    string out;
    char buf[4096];
    DWORD got = 0;
    while(out.size() < 65536 && ReadFile(h, buf, sizeof buf, &got, nullptr) && got > 0)
        out.append(buf, got);
    CloseHandle(h);
    return out;
}

bool writeFileAtomic(const wstring &path, const string &text)
{
    const wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if(h == INVALID_HANDLE_VALUE)
        return false;
    DWORD wrote = 0;
    const bool ok = WriteFile(h, text.data(), DWORD(text.size()), &wrote, nullptr) && wrote == text.size();
    FlushFileBuffers(h);
    CloseHandle(h);
    return ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

void logLine(const string &text)
{
    const wstring path = join(root, L"cgf-launcher.log");
    WIN32_FILE_ATTRIBUTE_DATA info;
    if(GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) && info.nFileSizeLow > 256 * 1024)
        MoveFileExW(path.c_str(), (path + L".1").c_str(), MOVEFILE_REPLACE_EXISTING);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[32];
    wsprintfA(stamp, "%04d-%02d-%02d %02d:%02d:%02d  ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    const string line = stamp + text + "\r\n";
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if(h == INVALID_HANDLE_VALUE)
        return;
    DWORD wrote = 0;
    WriteFile(h, line.data(), DWORD(line.size()), &wrote, nullptr);
    CloseHandle(h);
}

// cgf-versions.txt: key=value lines (written by the companion and here).
typedef std::map<string, string> State;

State parseState(const string &text)
{
    State s;
    size_t pos = 0;
    while(pos < text.size())
    {
        size_t end = text.find('\n', pos);
        if(end == string::npos)
            end = text.size();
        const string line = trim(text.substr(pos, end - pos));
        const size_t eq = line.find('=');
        if(eq != string::npos)
            s[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
        pos = end + 1;
    }
    return s;
}

string formatState(const State &s)
{
    static const char *order[] = {"current", "previous", "seq", "trial", "starts", "bad"};
    string out;
    for(const char *k : order)
    {
        auto it = s.find(k);
        out += string(k) + "=" + (it == s.end() ? string() : it->second) + "\n";
    }
    for(const auto &kv : s)
    {
        bool known = false;
        for(const char *k : order)
            known = known || kv.first == k;
        if(!known)
            out += kv.first + "=" + kv.second + "\n";
    }
    return out;
}

wstring appExe(const string &folder, const wchar_t *name)
{
    return join(join(root, widen(folder)), name);
}

// The newest app-* folder that has a program, for an install whose
// cgf-versions.txt is missing or names a folder that is gone.
string newestAppFolder()
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(join(root, L"app-*").c_str(), &fd);
    if(h == INVALID_HANDLE_VALUE)
        return string();
    string best;
    FILETIME bestTime = {0, 0};
    do
    {
        const wstring name = fd.cFileName;
        if(!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || name.find(L".partial") != wstring::npos)
            continue;
        if(!isFile(join(join(root, name), L"CGFProjector.exe")))
            continue;
        if(best.empty() || CompareFileTime(&fd.ftLastWriteTime, &bestTime) > 0)
        {
            best = narrow(name);
            bestTime = fd.ftLastWriteTime;
        }
    } while(FindNextFileW(h, &fd));
    FindClose(h);
    return best;
}

// Quoting that CommandLineToArgvW (and Qt) read back exactly.
wstring quoteArg(const wstring &arg)
{
    if(!arg.empty() && arg.find_first_of(L" \t\"") == wstring::npos)
        return arg;
    wstring out = L"\"";
    for(size_t i = 0; ; ++i)
    {
        size_t slashes = 0;
        while(i < arg.size() && arg[i] == L'\\')
        {
            ++slashes;
            ++i;
        }
        if(i == arg.size())
        {
            out.append(slashes * 2, L'\\'); // before the closing quote
            break;
        }
        if(arg[i] == L'"')
            out.append(slashes * 2 + 1, L'\\');
        else
            out.append(slashes, L'\\');
        out += arg[i];
    }
    return out + L"\"";
}

bool start(const wstring &exe, const std::vector<wstring> &args, PROCESS_INFORMATION *info)
{
    wstring cmd = quoteArg(exe);
    for(const wstring &a : args)
        cmd += L" " + quoteArg(a);
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    if(!CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, 0, nullptr, root.c_str(), &si, &pi))
    {
        logLine("cannot start " + narrow(exe) + " (error " + std::to_string(GetLastError()) + ")");
        return false;
    }
    if(info)
        *info = pi;
    else
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return true;
}

bool canRollBack(const State &s)
{
    auto it = s.find("previous");
    return it != s.end() && !it->second.empty() && isFile(appExe(it->second, L"CGFProjector.exe"));
}

void rollBack(State &s, const string &why)
{
    const string failed = s["current"];
    logLine(failed + " " + why + "; going back to " + s["previous"]);
    s["bad"] = s["bad"].empty() ? failed : s["bad"] + "," + failed;
    s["current"] = s["previous"];
    s["previous"] = "";
    s["trial"] = "0";
    s["starts"] = "0";
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    {
        std::vector<wchar_t> buf(32768);
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
        const wstring self(buf.data(), n);
        const size_t slash = self.find_last_of(L"\\/");
        root = slash == wstring::npos ? L"." : self.substr(0, slash);
    }

    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool companion = false;
    DWORD afterPid = 0;
    std::vector<wstring> passOn;
    for(int i = 1; i < argc; ++i)
    {
        const wstring a = argv[i];
        if(a == L"--companion")
            companion = true;
        else if(a == L"--after" && i + 1 < argc)
            afterPid = DWORD(wcstoul(argv[++i], nullptr, 10));
        else
            passOn.push_back(a);
    }
    LocalFree(argv);

    // A restart for an update: wait until the old copy has closed.
    if(afterPid)
        if(HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, afterPid))
        {
            WaitForSingleObject(h, 60000);
            CloseHandle(h);
        }

    const wstring stateFile = join(root, L"cgf-versions.txt");
    State state = parseState(readFile(stateFile));
    if(state["current"].empty() || !isFile(appExe(state["current"], L"CGFProjector.exe")))
    {
        const string found = newestAppFolder();
        if(found.empty())
        {
            MessageBoxW(nullptr, L"CGF Projector cannot find its program files. Please install it again.",
                        L"CGF Projector", MB_ICONERROR | MB_OK);
            return 1;
        }
        logLine("the current version folder is missing; starting " + found);
        state["current"] = found;
        state["trial"] = "0";
        writeFileAtomic(stateFile, formatState(state));
    }

    if(companion)
    {
        start(appExe(state["current"], L"cgf-companion.exe"),
              {L"--config", join(root, L"cgf-cloud.json"), L"--data-dir", root, L"--stay"}, nullptr);
        return 0;
    }

    bool trial = state["trial"] == "1";
    if(trial && trim(readFile(join(root, L"cgf-healthy.txt"))) == state["current"])
    {
        logLine(state["current"] + " ran a full minute; keeping it");
        trial = false;
        state["trial"] = "0";
        state["starts"] = "0";
        writeFileAtomic(stateFile, formatState(state));
    }
    if(trial)
    {
        const int starts = std::atoi(state["starts"].c_str()) + 1;
        state["starts"] = std::to_string(starts);
        if(starts > 3 && canRollBack(state))
        {
            rollBack(state, "never ran a full minute in 3 starts");
            trial = false;
        }
        writeFileAtomic(stateFile, formatState(state));
    }

    std::vector<wstring> args = {L"--data-dir", root};
    args.insert(args.end(), passOn.begin(), passOn.end());
    PROCESS_INFORMATION pi = {};
    if(!start(appExe(state["current"], L"CGFProjector.exe"), args, &pi))
    {
        if(trial && canRollBack(state))
        {
            rollBack(state, "could not be started");
            writeFileAtomic(stateFile, formatState(state));
            return start(appExe(state["current"], L"CGFProjector.exe"), args, nullptr) ? 0 : 1;
        }
        MessageBoxW(nullptr, L"CGF Projector could not be started.", L"CGF Projector", MB_ICONERROR | MB_OK);
        return 1;
    }
    if(trial)
    {
        // Stay a little while: a version that fails at once is replaced
        // before the operator has to try again.
        DWORD code = 0;
        if(WaitForSingleObject(pi.hProcess, 20000) == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &code)
                && code != 0 && canRollBack(state))
        {
            rollBack(state, "stopped with code " + std::to_string(code) + " in its first 20 seconds");
            writeFileAtomic(stateFile, formatState(state));
            start(appExe(state["current"], L"CGFProjector.exe"), args, nullptr);
        }
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
