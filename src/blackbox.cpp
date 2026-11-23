#include "blackbox.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cstdio>

namespace bbox {

std::string CsvField(const std::wstring& w) {
    std::string s;
    if (!w.empty()) {
        int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
        s.assign(size_t(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    }
    if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
    std::string q = "\"";
    for (char c : s) {
        if (c == '"') q += "\"\"";
        else if (c == '\r' || c == '\n') q += ' ';
        else q += c;
    }
    return q + "\"";
}

void Recorder::Configure(const std::wstring& dir, double preSec, double postSec, double maxSec, int keepFiles) {
    Close();
    dir_ = dir;
    pre_ = std::max(1.0, preSec);
    post_ = std::max(1.0, postSec);
    max_ = std::max(pre_ + post_, maxSec);
    keep_ = std::max(1, keepFiles);
    ring_.clear();
}

void Recorder::WriteRow(const Sample& s) {
    FILE* f = static_cast<FILE*>(f_);
    if (!f) return;
    fprintf(f, "%s,%d", s.stamp.c_str(), s.ok ? 1 : 0);
    for (int i = 0; i < pins_; ++i) fprintf(f, ",%.3f", s.ok && i < s.pinCount ? s.volts[i] : 0.f);
    for (int i = 0; i < pins_; ++i) fprintf(f, ",%.3f", s.ok && i < s.pinCount ? s.amps[i] : 0.f);
    fprintf(f, ",%.1f,%.1f,%d,%.1f,%.1f,%.1f,%d,%s\n", s.connW, s.boardW, s.gpuUtil, s.coreC, s.hotspotC, s.vramC, s.maxState,
            CsvField(s.events).c_str());
    fflush(f);   // on disk right away: the moments before a crash / emergency shutdown are the ones that matter
}

void Recorder::Add(const Sample& s, bool trigger, const std::string& reason) {
    if (dir_.empty()) return;
    if (!f_ && trigger) {
        CreateDirectoryW(dir_.c_str(), nullptr);
        SYSTEMTIME t;
        GetLocalTime(&t);
        std::string why;
        for (char c : reason) if (isalnum(static_cast<unsigned char>(c)) || c == '-') why += c;
        if (why.size() > 40) why.resize(40);
        wchar_t name[128];
        swprintf(name, 128, L"\\blackbox_%04u%02u%02u_%02u%02u%02u%hs%hs.csv", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                 why.empty() ? "" : "_", why.c_str());
        path_ = dir_ + name;
        FILE* f = _wfopen(path_.c_str(), L"wb");
        if (f) {
            f_ = f;
            pins_ = s.pinCount > 6 ? kMaxPins : 6;
            fprintf(f, "time,pins_ok");
            for (int i = 1; i <= pins_; ++i) fprintf(f, ",p%d_V", i);
            for (int i = 1; i <= pins_; ++i) fprintf(f, ",p%d_A", i);
            fprintf(f, ",connector_W,board_W,gpu_util,core_C,hotspot_C,vram_C,pin_state,event\n");
            for (auto& r : ring_) WriteRow(r);   // the minute before
            start_ = s.t;
            end_ = s.t + post_;
        } else {
            path_.clear();
        }
    }
    if (f_) {
        if (trigger) end_ = std::min(std::max(end_, s.t + post_), start_ + max_);
        WriteRow(s);
        if (s.t >= end_) Close();
    }
    ring_.push_back(s);
    while (!ring_.empty() && ring_.front().t < s.t - pre_) ring_.pop_front();
}

void Recorder::Close() {
    if (!f_) return;
    fclose(static_cast<FILE*>(f_));
    f_ = nullptr;
    finished_ = path_;
    Prune();
}

std::wstring Recorder::TakeFinished() {
    std::wstring r;
    r.swap(finished_);
    return r;
}

void Recorder::Prune() {
    std::vector<std::wstring> files;   // names sort by date (blackbox_YYYYMMDD_HHMMSS...)
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir_ + L"\\blackbox_*.csv").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do files.push_back(fd.cFileName);
    while (FindNextFileW(h, &fd));
    FindClose(h);
    if (int(files.size()) <= keep_) return;
    std::sort(files.begin(), files.end());
    for (size_t i = 0; i + size_t(keep_) < files.size(); ++i) DeleteFileW((dir_ + L"\\" + files[i]).c_str());
}

} // namespace bbox
