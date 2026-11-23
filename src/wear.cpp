#include "wear.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace wear {
namespace {

std::wstring F(const wchar_t* fmt, double a, double b = 0, double c = 0) {
    wchar_t buf[256];
    swprintf(buf, 256, fmt, a, b, c);
    return buf;
}

// minute-weighted average of a range of days
void Average(const std::vector<const Day*>& d, double share[6], double& imb) {
    double w = 0;
    for (int i = 0; i < 6; ++i) share[i] = 0;
    imb = 0;
    for (auto* x : d) {
        w += x->minutes;
        for (int i = 0; i < 6; ++i) share[i] += x->share[i] * x->minutes;
        imb += x->imbalance * x->minutes;
    }
    if (w <= 0) return;
    for (int i = 0; i < 6; ++i) share[i] /= w;
    imb /= w;
}

} // namespace

bool Load(const std::wstring& path, std::vector<Day>& days) {
    days.clear();
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (line[0] < '0' || line[0] > '9') continue;   // header / comments
        Day d;
        char date[16] = {};
        int n = sscanf(line, "%15[^,],%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf", date, &d.minutes, &d.share[0], &d.share[1], &d.share[2],
                       &d.share[3], &d.share[4], &d.share[5], &d.imbalance);
        if (n != 9 || d.minutes < 0 || !std::isfinite(d.minutes) || !std::isfinite(d.imbalance)) continue;
        bool sane = true;
        for (double s : d.share) sane &= std::isfinite(s) && s >= 0 && s <= 1;
        if (!sane) continue;
        d.date = date;
        if (!days.empty() && days.back().date == d.date) days.back() = d;   // duplicate date: keep the last
        else days.push_back(d);
    }
    fclose(f);
    std::sort(days.begin(), days.end(), [](const Day& a, const Day& b) { return a.date < b.date; });
    return true;
}

bool Save(const std::wstring& path, const std::vector<Day>& days) {
    std::wstring tmp = path + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    fprintf(f, "date,loaded_minutes,p1_share,p2_share,p3_share,p4_share,p5_share,p6_share,imbalance\n");
    for (auto& d : days)
        fprintf(f, "%s,%.2f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.4f\n", d.date.c_str(), d.minutes, d.share[0], d.share[1], d.share[2],
                d.share[3], d.share[4], d.share[5], d.imbalance);
    bool ok = fflush(f) == 0;
    ok &= fclose(f) == 0;
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

Status Analyze(const std::vector<Day>& days) {
    Status st;
    std::vector<const Day*> q;
    for (auto& d : days) if (d.minutes >= kMinMinutes) q.push_back(&d);
    st.qualifyingDays = int(q.size());
    if (int(q.size()) < kBaseDays + 1) {
        st.state = LEARNING;
        st.text = F(L"Learning your connector: %.0f of %.0f days with gaming load so far", double(q.size()), double(kBaseDays + 1));
        return st;
    }
    std::vector<const Day*> base(q.begin(), q.begin() + kBaseDays);
    size_t nRecent = std::min<size_t>(kRecentDays, q.size() - kBaseDays);
    std::vector<const Day*> recent(q.end() - long(nRecent), q.end());
    Average(base, st.baseShare, st.baseImb);
    Average(recent, st.recentShare, st.recentImb);
    st.pin = 0;
    for (int i = 0; i < 6; ++i) {
        double d = (st.recentShare[i] - st.baseShare[i]) * 100;
        if (i == 0 || d > st.deltaPts) { st.deltaPts = d; st.pin = i; }
    }
    if (st.deltaPts >= kWarnPts) st.state = WARN;
    else if (st.deltaPts >= kWatchPts) st.state = WATCH;
    else st.state = OK;
    double pct = st.baseShare[st.pin] > 0 ? (st.recentShare[st.pin] / st.baseShare[st.pin] - 1) * 100 : 0;
    if (st.state == OK)
        st.text = F(L"Connector wear: OK - pin balance unchanged since you started (imbalance %.2f, was %.2f)", st.recentImb, st.baseImb);
    else
        st.text = F(L"Pin %.0f now carries %.1f %% of the current (was %.1f %%)", st.pin + 1.0, st.recentShare[st.pin] * 100, st.baseShare[st.pin] * 100) +
                  F(L" - %.0f %% more than before. ", pct) +
                  (st.state == WARN ? L"Check the 12V-2x6 connector: fully seated, not bent, no discoloration."
                                    : L"Keep an eye on it; reseat the connector if it keeps rising.");
    return st;
}

// ---------------- tracker ----------------
void Tracker::Open(const std::wstring& path) {
    path_ = path;
    Load(path_, days_);
    today_.clear();
    sec_ = imbSec_ = 0;
    for (double& s : shareSec_) s = 0;
    dirty_ = false;
    status_ = Analyze(days_);
}

void Tracker::LoadToday(const std::string& date) {
    today_ = date;
    sec_ = imbSec_ = 0;
    for (double& s : shareSec_) s = 0;
    if (!days_.empty() && days_.back().date == date) {   // continue today's row after a restart
        const Day& d = days_.back();
        sec_ = d.minutes * 60;
        for (int i = 0; i < 6; ++i) shareSec_[i] = d.share[i] * sec_;
        imbSec_ = d.imbalance * sec_;
    }
}

bool Tracker::Add(const double* amps, const std::string& date, double dtSec) {
    if (path_.empty() || dtSec <= 0 || dtSec > 30) return false;
    double total = 0;
    for (int i = 0; i < 6; ++i) {
        if (!std::isfinite(amps[i]) || amps[i] < 0) return false;
        total += amps[i];
    }
    if (total < kMinTotalA) return false;
    if (date != today_) {
        if (dirty_) Flush();
        LoadToday(date);
    }
    double hi = 0;
    for (int i = 0; i < 6; ++i) {
        double s = amps[i] / total;
        shareSec_[i] += s * dtSec;
        hi = std::max(hi, s);
    }
    imbSec_ += hi * 6 * dtSec;
    sec_ += dtSec;
    dirty_ = true;
    return true;
}

bool Tracker::Flush() {
    if (!dirty_ || today_.empty() || sec_ <= 0) return false;
    Day d;
    d.date = today_;
    d.minutes = sec_ / 60;
    for (int i = 0; i < 6; ++i) d.share[i] = shareSec_[i] / sec_;
    d.imbalance = imbSec_ / sec_;
    if (!days_.empty() && days_.back().date == today_) days_.back() = d;
    else days_.push_back(d);
    std::sort(days_.begin(), days_.end(), [](const Day& a, const Day& b) { return a.date < b.date; });
    if (Save(path_, days_)) dirty_ = false;
    State old = status_.state;
    status_ = Analyze(days_);
    return status_.state != old;
}

void Tracker::Reset() {
    if (path_.empty()) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t suffix[64];
    swprintf(suffix, 64, L".until-%04u%02u%02u-%02u%02u%02u.old", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    MoveFileExW(path_.c_str(), (path_ + suffix).c_str(), MOVEFILE_REPLACE_EXISTING);
    days_.clear();
    today_.clear();
    sec_ = imbSec_ = 0;
    for (double& s : shareSec_) s = 0;
    dirty_ = false;
    status_ = Analyze(days_);
}

} // namespace wear
