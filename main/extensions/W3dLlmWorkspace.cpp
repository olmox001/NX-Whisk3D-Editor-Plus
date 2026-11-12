#ifdef W3D_MCP

#include "extensions/W3dLlmWorkspace.h"
#include "io/w3dFilesystem.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct PlanPoint {
    std::string id;
    std::string text;
    std::string status; // open, complete, partial, missing
    std::vector<std::string> tasks;
};

struct Task {
    std::string id;
    std::string title;
    std::string point;
    std::string status; // active, suspended, closed
    std::string outcome; // open, complete, partial, missing
    std::string created;
    std::string updated;
    std::string closed;
    unsigned changes;
    unsigned commits;
    unsigned nextChange;
    unsigned nextCommit;
    Task() : changes(0), commits(0), nextChange(1), nextCommit(1) {}
};

struct Session {
    std::string id;
    std::string title;
    std::string objective;
    std::string status; // active, closed
    std::string result; // working, confirmed, partial, missing
    std::string created;
    std::string updated;
    std::string closed;
    std::string currentTask;
    std::vector<PlanPoint> plan;
    std::vector<Task> tasks;
    unsigned testsTotal;
    unsigned testsPassed;
    unsigned nextTask;
    unsigned nextTest;
    Session() : testsTotal(0), testsPassed(0), nextTask(1), nextTest(1) {}
};

static bool gReady = false;
static std::string gRoot;
static std::vector<Session> gSessions;
static std::string gCurrentSession;
static std::string gCurrentTask;
static unsigned gNextSession = 1;

static std::string Now() {
    const std::time_t t = std::time(NULL);
    std::tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char b[32];
    std::strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return b;
}

static std::string Encode(const std::string& s) {
    std::string out;
    char b[4];
    for (size_t i = 0; i < s.size(); i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c == '%' || c == '\t' || c == '\n' || c == '\r') {
            std::snprintf(b, sizeof(b), "%02X", (unsigned)c);
            out += '%';
            out += b;
        } else out += (char)c;
    }
    return out;
}

static int Hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static std::string Decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int a = Hex(s[i + 1]), b = Hex(s[i + 2]);
            if (a >= 0 && b >= 0) {
                out += (char)((a << 4) | b);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

static std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t a = 0;
    while (a <= s.size()) {
        size_t b = s.find(sep, a);
        if (b == std::string::npos) b = s.size();
        out.push_back(s.substr(a, b - a));
        if (b == s.size()) break;
        a = b + 1;
    }
    return out;
}

static std::string Join(const std::vector<std::string>& v, char sep) {
    std::string s;
    for (size_t i = 0; i < v.size(); i++) {
        if (i) s += sep;
        s += v[i];
    }
    return s;
}

static unsigned IdNumber(const std::string& id) {
    if (id.size() < 2) return 0;
    return (unsigned)std::strtoul(id.c_str() + 1, NULL, 10);
}

static std::string MakeId(char kind, unsigned n) {
    char b[24];
    std::snprintf(b, sizeof(b), "%c%06u", kind, n);
    return b;
}

static fs::path Root() { return fs::path(gRoot); }
static fs::path SessionDir(const std::string& sid) { return Root() / "sessions" / sid; }
static fs::path TaskDir(const std::string& sid, const std::string& tid) {
    return SessionDir(sid) / "tasks" / tid;
}

static bool Write(const fs::path& path, const std::string& data) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << data;
    return f.good();
}

static bool Append(const fs::path& path, const std::string& data) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::app);
    if (!f) return false;
    f << data;
    return f.good();
}

static std::vector<std::string> Lines(const fs::path& path) {
    std::vector<std::string> out;
    std::ifstream f(path, std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line[line.size() - 1] == '\r') line.resize(line.size() - 1);
        if (!line.empty()) out.push_back(line);
    }
    return out;
}

static std::string Field(const std::vector<std::string>& f, size_t i) {
    return i < f.size() ? Decode(f[i]) : std::string();
}

static Session* FindSession(const std::string& id) {
    for (size_t i = 0; i < gSessions.size(); i++)
        if (gSessions[i].id == id) return &gSessions[i];
    return NULL;
}

static Task* FindTask(Session* s, const std::string& id) {
    if (!s) return NULL;
    for (size_t i = 0; i < s->tasks.size(); i++)
        if (s->tasks[i].id == id) return &s->tasks[i];
    return NULL;
}

static PlanPoint* FindPoint(Session* s, const std::string& id) {
    if (!s) return NULL;
    for (size_t i = 0; i < s->plan.size(); i++)
        if (s->plan[i].id == id) return &s->plan[i];
    return NULL;
}

static Session* CurrentSession() { return FindSession(gCurrentSession); }
static Task* CurrentTask() { return FindTask(CurrentSession(), gCurrentTask); }

static unsigned ActiveSessionCount() {
    unsigned n = 0;
    for (size_t i = 0; i < gSessions.size(); i++)
        if (gSessions[i].status == "active") n++;
    return n;
}

static unsigned OpenTaskCount(const Session& s) {
    unsigned n = 0;
    for (size_t i = 0; i < s.tasks.size(); i++)
        if (s.tasks[i].status != "closed") n++;
    return n;
}

static void SaveCurrent() {
    std::ostringstream o;
    o << "current_session\t" << Encode(gCurrentSession) << "\n"
      << "current_task\t" << Encode(gCurrentTask) << "\n";
    Write(Root() / "current.tsv", o.str());
}

static void SaveTaskFile(const Session& s, const Task& t) {
    std::ostringstream o;
    o << "id\t" << Encode(t.id) << "\n"
      << "title\t" << Encode(t.title) << "\n"
      << "plan_point\t" << Encode(t.point) << "\n"
      << "status\t" << Encode(t.status) << "\n"
      << "outcome\t" << Encode(t.outcome) << "\n"
      << "created\t" << Encode(t.created) << "\n"
      << "updated\t" << Encode(t.updated) << "\n"
      << "closed\t" << Encode(t.closed) << "\n"
      << "changes\t" << t.changes << "\n"
      << "commits\t" << t.commits << "\n";
    Write(TaskDir(s.id, t.id) / "task.tsv", o.str());
}

static void SaveSession(Session& s) {
    std::error_code ec;
    fs::create_directories(SessionDir(s.id) / "tasks", ec);

    std::ostringstream meta;
    meta << "id\t" << Encode(s.id) << "\n"
         << "title\t" << Encode(s.title) << "\n"
         << "objective\t" << Encode(s.objective) << "\n"
         << "status\t" << Encode(s.status) << "\n"
         << "objective_result\t" << Encode(s.result) << "\n"
         << "created\t" << Encode(s.created) << "\n"
         << "updated\t" << Encode(s.updated) << "\n"
         << "closed\t" << Encode(s.closed) << "\n"
         << "tests_total\t" << s.testsTotal << "\n"
         << "tests_passed\t" << s.testsPassed << "\n";
    Write(SessionDir(s.id) / "session.tsv", meta.str());

    std::ostringstream plan;
    plan << "point_id\tstatus\ttext\ttasks\n";
    for (size_t i = 0; i < s.plan.size(); i++) {
        plan << Encode(s.plan[i].id) << '\t' << Encode(s.plan[i].status) << '\t'
             << Encode(s.plan[i].text) << '\t' << Encode(Join(s.plan[i].tasks, ',')) << "\n";
    }
    Write(SessionDir(s.id) / "plan.tsv", plan.str());

    std::ostringstream tasks;
    tasks << "task_id\tstatus\ttitle\tplan_point\tcreated\tupdated\tclosed\tchanges\tcommits\toutcome\n";
    for (size_t i = 0; i < s.tasks.size(); i++) {
        const Task& t = s.tasks[i];
        tasks << Encode(t.id) << '\t' << Encode(t.status) << '\t' << Encode(t.title) << '\t'
              << Encode(t.point) << '\t' << Encode(t.created) << '\t' << Encode(t.updated) << '\t'
              << Encode(t.closed) << '\t' << t.changes << '\t' << t.commits << '\t'
              << Encode(t.outcome) << "\n";
        SaveTaskFile(s, t);
    }
    Write(SessionDir(s.id) / "tasks.tsv", tasks.str());
}

static void SaveRegistry() {
    std::ostringstream o;
    o << "session_id\tstatus\tobjective_result\ttitle\tobjective\tcreated\tupdated\tclosed\n";
    for (size_t i = 0; i < gSessions.size(); i++) {
        const Session& s = gSessions[i];
        o << Encode(s.id) << '\t' << Encode(s.status) << '\t' << Encode(s.result) << '\t'
          << Encode(s.title) << '\t' << Encode(s.objective) << '\t' << Encode(s.created) << '\t'
          << Encode(s.updated) << '\t' << Encode(s.closed) << "\n";
    }
    Write(Root() / "registry.tsv", o.str());
    SaveCurrent();
}

static void Event(Session& s, const std::string& type, const std::string& detail) {
    Append(SessionDir(s.id) / "events.tsv",
           Encode(Now()) + "\t" + Encode(type) + "\t" + Encode(detail) + "\n");
}

static void LoadSession(Session& s) {
    const std::vector<std::string> meta = Lines(SessionDir(s.id) / "session.tsv");
    for (size_t i = 0; i < meta.size(); i++) {
        const std::vector<std::string> f = Split(meta[i], '\t');
        if (f.size() < 2) continue;
        const std::string k = f[0], v = Field(f, 1);
        if (k == "title") s.title = v;
        else if (k == "objective") s.objective = v;
        else if (k == "status") s.status = v;
        else if (k == "objective_result") s.result = v;
        else if (k == "created") s.created = v;
        else if (k == "updated") s.updated = v;
        else if (k == "closed") s.closed = v;
        else if (k == "current_task") s.currentTask = v;
        else if (k == "tests_total") s.testsTotal = (unsigned)std::strtoul(v.c_str(), NULL, 10);
        else if (k == "tests_passed") s.testsPassed = (unsigned)std::strtoul(v.c_str(), NULL, 10);
    }

    const std::vector<std::string> pl = Lines(SessionDir(s.id) / "plan.tsv");
    for (size_t i = 1; i < pl.size(); i++) {
        const std::vector<std::string> f = Split(pl[i], '\t');
        if (f.size() < 3) continue;
        PlanPoint p;
        p.id = Field(f, 0);
        p.status = Field(f, 1);
        p.text = Field(f, 2);
        if (f.size() > 3 && !Field(f, 3).empty()) p.tasks = Split(Field(f, 3), ',');
        s.plan.push_back(p);
    }

    const std::vector<std::string> ts = Lines(SessionDir(s.id) / "tasks.tsv");
    unsigned maxTask = 0;
    for (size_t i = 1; i < ts.size(); i++) {
        const std::vector<std::string> f = Split(ts[i], '\t');
        if (f.size() < 9) continue;
        Task t;
        t.id = Field(f, 0);
        t.status = Field(f, 1);
        t.title = Field(f, 2);
        t.point = Field(f, 3);
        t.created = Field(f, 4);
        t.updated = Field(f, 5);
        t.closed = Field(f, 6);
        t.changes = (unsigned)std::strtoul(Field(f, 7).c_str(), NULL, 10);
        t.commits = (unsigned)std::strtoul(Field(f, 8).c_str(), NULL, 10);
        if (f.size() > 9) t.outcome = Field(f, 9);
        else if (t.status != "closed") t.outcome = "open";
        else {
            PlanPoint* p = FindPoint(&s, t.point);
            t.outcome = p && p->status != "open" ? p->status : "partial";
        }
        t.nextChange = t.changes + 1;
        t.nextCommit = t.commits + 1;
        s.tasks.push_back(t);
        maxTask = std::max(maxTask, IdNumber(t.id));
    }
    s.nextTask = maxTask + 1;
    s.nextTest = s.testsTotal + 1;
}

static void Load() {
    gSessions.clear();
    unsigned maxSession = 0;
    const std::vector<std::string> reg = Lines(Root() / "registry.tsv");
    for (size_t i = 1; i < reg.size(); i++) {
        const std::vector<std::string> f = Split(reg[i], '\t');
        if (f.empty()) continue;
        Session s;
        s.id = Field(f, 0);
        if (s.id.empty()) continue;
        LoadSession(s);
        gSessions.push_back(s);
        maxSession = std::max(maxSession, IdNumber(s.id));
    }
    gNextSession = maxSession + 1;

    const std::vector<std::string> cur = Lines(Root() / "current.tsv");
    for (size_t i = 0; i < cur.size(); i++) {
        const std::vector<std::string> f = Split(cur[i], '\t');
        if (f.size() < 2) continue;
        if (f[0] == "current_session") gCurrentSession = Field(f, 1);
        else if (f[0] == "current_task") gCurrentTask = Field(f, 1);
    }

    Session* cs = FindSession(gCurrentSession);
    if (!cs || cs->status != "active") {
        gCurrentSession.clear();
        gCurrentTask.clear();
        for (size_t i = gSessions.size(); i > 0; i--) {
            if (gSessions[i - 1].status == "active") {
                gCurrentSession = gSessions[i - 1].id;
                break;
            }
        }
    }
    cs = CurrentSession();
    Task* ct = FindTask(cs, gCurrentTask);
    if (!ct || ct->status != "active") {
        gCurrentTask.clear();
        if (cs) {
            Task* remembered = FindTask(cs, cs->currentTask);
            if (remembered && remembered->status == "active") gCurrentTask = remembered->id;
        }
    }
    if (cs) cs->currentTask = gCurrentTask;

    // Migra automaticamente los indices persistentes al esquema actual.
    // Historial, marcas de tiempo, pruebas y comentarios quedan intactos.
    for (size_t i = 0; i < gSessions.size(); i++) SaveSession(gSessions[i]);
    SaveCurrent();
}

static std::string TaskLine(const Session& s, const Task& t) {
    std::ostringstream o;
    o << t.id << " [" << t.status << "] " << t.title << " -> " << t.point
      << " outcome=" << (t.outcome.empty() ? "open" : t.outcome)
      << " changes=" << t.changes << " commits=" << t.commits;
    const PlanPoint* p = NULL;
    for (size_t i = 0; i < s.plan.size(); i++) {
        if (s.plan[i].id == t.point) {
            p = &s.plan[i];
            break;
        }
    }
    if (p) o << " point=" << p->status;
    return o.str();
}

static void RecomputePoint(Session& s, const std::string& pointId) {
    PlanPoint* p = FindPoint(&s, pointId);
    if (!p) return;

    bool any = false;
    bool open = false;
    unsigned complete = 0, partial = 0, missing = 0;
    for (size_t i = 0; i < s.tasks.size(); i++) {
        const Task& t = s.tasks[i];
        if (t.point != pointId) continue;
        any = true;
        if (t.status != "closed") { open = true; continue; }
        if (t.outcome == "complete") complete++;
        else if (t.outcome == "missing") missing++;
        else partial++;
    }

    if (!any || open) p->status = "open";
    else if (complete > 0 && partial == 0 && missing == 0) p->status = "complete";
    else if (missing > 0 && complete == 0 && partial == 0) p->status = "missing";
    else p->status = "partial";
}

static std::string DerivedSessionResult(const Session& s) {
    if (s.plan.empty()) return "missing";
    unsigned complete = 0, partial = 0, missing = 0;
    for (size_t i = 0; i < s.plan.size(); i++) {
        if (s.plan[i].status == "open") return "working";
        if (s.plan[i].status == "complete") complete++;
        else if (s.plan[i].status == "missing") missing++;
        else partial++;
    }
    if (complete == s.plan.size()) return "confirmed";
    if (missing == s.plan.size()) return "missing";
    return "partial";
}

static bool LastSessionTestPassed(const Session& s) {
    const std::vector<std::string> lines = Lines(SessionDir(s.id) / "tests.tsv");
    if (lines.empty()) return false;
    const std::vector<std::string> f = Split(lines.back(), '	');
    return f.size() >= 3 && f[2] == "pass";
}

} // namespace

void W3dLlmWorkspaceInit() {
    if (gReady) return;
    const char* overrideDir = std::getenv("W3D_LLM_WORKSPACE_DIR");
    if (overrideDir && *overrideDir) {
        gRoot = overrideDir;
    } else {
        std::string base = w3dFileSystem::GetUserDataDir();
        if (base.empty()) base = w3dFileSystem::GetHomeDir();
        if (base.empty()) return;
        gRoot = w3dFileSystem::JoinPath(base, "llm-workspace");
    }
    std::error_code ec;
    fs::create_directories(Root() / "sessions", ec);
    if (ec) return;
    Load();
    SaveRegistry();
    gReady = true;
}

bool W3dLlmWorkspaceReady() {
    return gReady;
}

std::string W3dLlmWorkspaceContext(bool verbose) {
    if (!gReady) return "LLM WORKSPACE: not initialized\n";

    std::ostringstream o;
    Session* s = CurrentSession();
    Task* t = CurrentTask();
    o << "LLM WORKSPACE\n";

    if (!s) {
        o << "current_session=(none) -- open or switch a session before mutating the editor\n";
    } else {
        o << "current_session=" << s->id << " " << s->title
          << " | objective=" << s->objective
          << " | result=" << s->result
          << " | tests=" << s->testsPassed << "/" << s->testsTotal << "\n";
        if (t) o << "current_task=" << TaskLine(*s, *t) << "\n";
        else o << "current_task=(none)\n";

        o << "session_tasks:";
        bool any = false;
        for (size_t i = 0; i < s->tasks.size(); i++) {
            if (s->tasks[i].status == "closed" && !verbose) continue;
            o << "\n  " << TaskLine(*s, s->tasks[i]);
            any = true;
        }
        if (!any) o << " (none)";

        o << "\nplan:";
        for (size_t i = 0; i < s->plan.size(); i++)
            o << "\n  " << s->plan[i].id << " [" << s->plan[i].status << "] " << s->plan[i].text;
        if (s->plan.empty()) o << " (required before tasks)";
        o << "\n";
    }

    o << "other_active_sessions:";
    bool other = false;
    for (size_t i = 0; i < gSessions.size(); i++) {
        const Session& x = gSessions[i];
        if (x.status != "active" || x.id == gCurrentSession) continue;
        o << "\n  " << x.id << " " << x.title
          << " | objective=" << x.objective
          << " | current_task=" << (x.currentTask.empty() ? "(none)" : x.currentTask)
          << " | open_tasks=" << OpenTaskCount(x)
          << " | tests=" << x.testsPassed << "/" << x.testsTotal;
        other = true;
    }
    if (!other) o << " (none)";

    o << "\nsuspended_tasks:";
    bool suspended = false;
    for (size_t i = 0; i < gSessions.size(); i++) {
        const Session& x = gSessions[i];
        if (x.status != "active") continue;
        for (size_t k = 0; k < x.tasks.size(); k++) {
            if (x.tasks[k].status != "suspended") continue;
            o << "\n  " << x.id << "/" << x.tasks[k].id << " " << x.tasks[k].title
              << " -> " << x.tasks[k].point
              << " updated=" << x.tasks[k].updated;
            suspended = true;
        }
    }
    if (!suspended) o << " (none)";

    o << "\nlimits: active_sessions=" << ActiveSessionCount() << "/3";
    if (s) o << " open_tasks_current_session=" << OpenTaskCount(*s) << "/8";
    o << "\n";
    if (verbose) o << "registry=" << (Root() / "registry.tsv").string() << "\n";
    return o.str();
}

W3dLlmWorkspaceResult W3dLlmSessionOpen(const std::string& title,
                                         const std::string& objective) {
    if (!gReady) return W3dLlmWorkspaceResult(false, "workspace not initialized");
    if (title.empty() || objective.empty())
        return W3dLlmWorkspaceResult(false, "title and objective are required");
    if (ActiveSessionCount() >= 3)
        return W3dLlmWorkspaceResult(false, "maximum 3 active sessions; close one first");

    Session s;
    s.id = MakeId('S', gNextSession++);
    s.title = title;
    s.objective = objective;
    s.status = "active";
    s.result = "working";
    s.created = s.updated = Now();
    gSessions.push_back(s);

    gCurrentSession = s.id;
    gCurrentTask.clear();
    SaveSession(gSessions.back());
    SaveRegistry();
    Event(gSessions.back(), "session_open", objective);

    return W3dLlmWorkspaceResult(
        true, "opened session " + s.id + "; create its objective plan before opening tasks");
}

W3dLlmWorkspaceResult W3dLlmSessionAddPlan(const std::vector<std::string>& points) {
    Session* s = CurrentSession();
    if (!s || s->status != "active")
        return W3dLlmWorkspaceResult(false, "no active current session");
    if (points.empty())
        return W3dLlmWorkspaceResult(false, "at least one plan point is required");

    unsigned next = 1;
    for (size_t i = 0; i < s->plan.size(); i++)
        next = std::max(next, IdNumber(s->plan[i].id) + 1);

    unsigned added = 0;
    for (size_t i = 0; i < points.size(); i++) {
        if (points[i].empty()) continue;
        bool duplicate = false;
        for (size_t k = 0; k < s->plan.size(); k++)
            if (s->plan[k].text == points[i]) duplicate = true;
        if (duplicate) continue;

        PlanPoint p;
        p.id = MakeId('P', next++);
        p.text = points[i];
        p.status = "open";
        s->plan.push_back(p);
        added++;
    }

    if (!added)
        return W3dLlmWorkspaceResult(false, "no new plan points were added");

    s->updated = Now();
    SaveSession(*s);
    SaveRegistry();
    Event(*s, "plan_add", std::to_string(added));
    return W3dLlmWorkspaceResult(
        true, "plan updated; added " + std::to_string(added) + " point(s)");
}

W3dLlmWorkspaceResult W3dLlmSessionSwitch(const std::string& sessionId) {
    Session* s = FindSession(sessionId);
    if (!s || s->status != "active")
        return W3dLlmWorkspaceResult(false, "active session not found: " + sessionId);

    gCurrentSession = s->id;
    gCurrentTask.clear();
    Task* remembered = FindTask(s, s->currentTask);
    if (remembered && remembered->status == "active") gCurrentTask = remembered->id;
    if (gCurrentTask.empty()) {
        for (size_t i = 0; i < s->tasks.size(); i++) {
            if (s->tasks[i].status == "active") {
                gCurrentTask = s->tasks[i].id;
                break;
            }
        }
    }
    s->currentTask = gCurrentTask;
    s->updated = Now();
    SaveRegistry();
    Event(*s, "session_switch", s->id);
    return W3dLlmWorkspaceResult(true, "current session: " + s->id);
}

W3dLlmWorkspaceResult W3dLlmSessionClose(const std::string& result,
                                          const std::string& comment) {
    Session* s = CurrentSession();
    if (!s || s->status != "active")
        return W3dLlmWorkspaceResult(false, "no active current session");
    if (!result.empty() && result != "confirmed" && result != "partial" && result != "missing")
        return W3dLlmWorkspaceResult(false, "result must be confirmed, partial, missing, or omitted for automatic derivation");
    if (comment.empty())
        return W3dLlmWorkspaceResult(false, "session close comment is required");
    if (s->plan.empty())
        return W3dLlmWorkspaceResult(false, "session has no objective plan");

    for (size_t i = 0; i < s->tasks.size(); i++) {
        if (s->tasks[i].status != "closed")
            return W3dLlmWorkspaceResult(
                false, "all session tasks must be closed first: " + s->tasks[i].id);
    }
    for (size_t i = 0; i < s->plan.size(); i++) {
        if (s->plan[i].status == "open")
            return W3dLlmWorkspaceResult(
                false, "plan point still open: " + s->plan[i].id);
    }
    if (s->testsTotal == 0)
        return W3dLlmWorkspaceResult(
            false, "at least one session_test is required");

    const std::string derived = DerivedSessionResult(*s);
    if (derived == "working")
        return W3dLlmWorkspaceResult(false, "objective still has open plan points");
    if (!result.empty() && result != derived)
        return W3dLlmWorkspaceResult(
            false, "objective result is derived automatically as " + derived +
                   "; supplied result was " + result);
    if (derived == "confirmed" && !LastSessionTestPassed(*s))
        return W3dLlmWorkspaceResult(
            false, "confirmed objective requires the latest session_test to pass");

    const std::string closedId = s->id;
    s->result = derived;
    s->status = "closed";
    s->closed = s->updated = Now();
    Event(*s, "session_close", s->result + ": " + comment);
    SaveSession(*s);

    gCurrentSession.clear();
    gCurrentTask.clear();
    for (size_t i = gSessions.size(); i > 0; i--) {
        if (gSessions[i - 1].status == "active") {
            gCurrentSession = gSessions[i - 1].id;
            break;
        }
    }
    SaveRegistry();
    return W3dLlmWorkspaceResult(
        true, "closed session " + closedId + " with objective result=" + s->result);
}

W3dLlmWorkspaceResult W3dLlmTaskOpen(const std::string& title,
                                      const std::string& planPointId) {
    Session* s = CurrentSession();
    if (!s || s->status != "active")
        return W3dLlmWorkspaceResult(false, "no active current session");
    if (s->plan.empty())
        return W3dLlmWorkspaceResult(false, "create the session objective plan first");
    if (title.empty())
        return W3dLlmWorkspaceResult(false, "task title is required");

    PlanPoint* p = FindPoint(s, planPointId);
    if (!p)
        return W3dLlmWorkspaceResult(false, "unknown plan point: " + planPointId);
    if (OpenTaskCount(*s) >= 8)
        return W3dLlmWorkspaceResult(false, "maximum 8 active/suspended tasks per session");

    Task t;
    t.id = MakeId('T', s->nextTask++);
    t.title = title;
    t.point = p->id;
    t.status = "active";
    t.outcome = "open";
    t.created = t.updated = Now();
    s->tasks.push_back(t);
    p->tasks.push_back(t.id);
    p->status = "open";

    gCurrentTask = t.id;
    s->currentTask = t.id;
    s->updated = Now();
    SaveSession(*s);
    SaveRegistry();
    Event(*s, "task_open", t.id + ": " + title + " -> " + p->id);
    return W3dLlmWorkspaceResult(true, "opened task " + t.id + " for " + p->id);
}

W3dLlmWorkspaceResult W3dLlmTaskSwitch(const std::string& taskId) {
    Session* s = CurrentSession();
    Task* t = FindTask(s, taskId);
    if (!t || t->status != "active")
        return W3dLlmWorkspaceResult(false, "active task not found: " + taskId);

    gCurrentTask = t->id;
    s->currentTask = t->id;
    t->updated = s->updated = Now();
    SaveSession(*s);
    SaveRegistry();
    Event(*s, "task_switch", t->id);
    return W3dLlmWorkspaceResult(true, "current task: " + t->id);
}

W3dLlmWorkspaceResult W3dLlmTaskSuspend(const std::string& comment) {
    Session* s = CurrentSession();
    Task* t = CurrentTask();
    if (!s || !t || t->status != "active")
        return W3dLlmWorkspaceResult(false, "no active current task");
    if (comment.empty())
        return W3dLlmWorkspaceResult(false, "suspend comment is required");

    const std::string taskId = t->id;
    t->status = "suspended";
    t->updated = s->updated = Now();
    Event(*s, "task_suspend", t->id + ": " + comment);
    gCurrentTask.clear();
    s->currentTask.clear();
    SaveSession(*s);
    SaveRegistry();
    return W3dLlmWorkspaceResult(true, "suspended task " + taskId);
}

W3dLlmWorkspaceResult W3dLlmTaskResume(const std::string& taskId) {
    Session* s = CurrentSession();
    Task* t = FindTask(s, taskId);
    if (!t || t->status != "suspended")
        return W3dLlmWorkspaceResult(false, "suspended task not found: " + taskId);

    t->status = "active";
    t->updated = s->updated = Now();
    gCurrentTask = t->id;
    s->currentTask = t->id;
    SaveSession(*s);
    SaveRegistry();
    Event(*s, "task_resume", t->id);
    return W3dLlmWorkspaceResult(true, "resumed task " + t->id);
}

W3dLlmWorkspaceResult W3dLlmTaskCommit(const std::string& summary,
                                        const std::string& comment) {
    Session* s = CurrentSession();
    Task* t = CurrentTask();
    if (!s || !t || t->status != "active")
        return W3dLlmWorkspaceResult(false, "no active current task");
    if (summary.empty() || comment.empty())
        return W3dLlmWorkspaceResult(
            false, "commit summary and comment are both required");

    const unsigned seq = t->nextCommit++;
    std::ostringstream row;
    row << seq << '\t' << Encode(Now()) << '\t' << t->changes << '\t'
        << Encode(summary) << '\t' << Encode(comment) << "\n";
    if (!Append(TaskDir(s->id, t->id) / "commits.tsv", row.str()))
        return W3dLlmWorkspaceResult(false, "cannot persist task commit");

    t->commits++;
    t->updated = s->updated = Now();
    Event(*s, "task_commit",
          t->id + " #" + std::to_string(seq) + ": " + summary);
    SaveSession(*s);
    SaveRegistry();

    return W3dLlmWorkspaceResult(
        true, "task commit " + t->id + "#" + std::to_string(seq) +
                  " recorded at change index " + std::to_string(t->changes));
}

W3dLlmWorkspaceResult W3dLlmTaskClose(const std::string& outcome,
                                       const std::string& comment) {
    Session* s = CurrentSession();
    Task* t = CurrentTask();
    if (!s || !t || t->status != "active")
        return W3dLlmWorkspaceResult(false, "no active current task");
    if (outcome != "complete" && outcome != "partial" && outcome != "missing")
        return W3dLlmWorkspaceResult(
            false, "outcome must be complete, partial, or missing");
    if (t->commits == 0)
        return W3dLlmWorkspaceResult(
            false, "task cannot close before an internal task_commit");
    if (comment.empty())
        return W3dLlmWorkspaceResult(false, "task close comment is required");

    PlanPoint* p = FindPoint(s, t->point);
    if (!p)
        return W3dLlmWorkspaceResult(false, "task plan point no longer exists");

    const std::string taskId = t->id;
    const std::string pointId = p->id;
    t->outcome = outcome;
    t->status = "closed";
    t->closed = t->updated = s->updated = Now();
    RecomputePoint(*s, pointId);
    Event(*s, "task_close", t->id + " -> " + outcome + ": " + comment);
    gCurrentTask.clear();
    s->currentTask.clear();
    SaveSession(*s);
    SaveRegistry();

    return W3dLlmWorkspaceResult(
        true, "closed task " + taskId + "; " + pointId + "=" + p->status +
                  " (task outcome=" + outcome + ")");
}

bool W3dLlmWorkspaceCanMutate(std::string& reason) {
    if (!gReady) {
        reason = "workspace not initialized";
        return false;
    }
    Session* s = CurrentSession();
    Task* t = CurrentTask();
    if (!s) {
        reason = "open/switch an LLM session first";
        return false;
    }
    if (!t || t->status != "active") {
        reason = "open/resume/switch an active task before changing the editor";
        return false;
    }
    return true;
}

void W3dLlmWorkspaceRecordChange(const std::string& tool,
                                  const std::string& detail) {
    Session* s = CurrentSession();
    Task* t = CurrentTask();
    if (!s || !t || t->status != "active") return;

    const unsigned seq = t->nextChange++;
    std::ostringstream row;
    row << seq << '\t' << Encode(Now()) << '\t'
        << Encode(tool) << '\t' << Encode(detail) << "\n";
    if (!Append(TaskDir(s->id, t->id) / "changes.tsv", row.str())) return;

    t->changes++;
    t->updated = s->updated = Now();
    SaveSession(*s);
    SaveRegistry();
}

W3dLlmWorkspaceResult W3dLlmWorkspaceRecordTest(const std::string& commands,
                                                 const std::string& expected,
                                                 const std::string& output,
                                                 bool passed) {
    Session* s = CurrentSession();
    if (!s || s->status != "active")
        return W3dLlmWorkspaceResult(false, "no active current session");

    const unsigned seq = s->nextTest++;
    std::ostringstream row;
    row << seq << '\t' << Encode(Now()) << '\t'
        << (passed ? "pass" : "fail") << '\t'
        << Encode(commands) << '\t' << Encode(expected) << '\t'
        << Encode(output) << "\n";
    if (!Append(SessionDir(s->id) / "tests.tsv", row.str()))
        return W3dLlmWorkspaceResult(false, "cannot persist session test");

    s->testsTotal++;
    if (passed) s->testsPassed++;
    s->updated = Now();
    Event(*s, "session_test",
          std::string(passed ? "pass: " : "fail: ") + commands);
    SaveSession(*s);
    SaveRegistry();

    return W3dLlmWorkspaceResult(
        true, "session test #" + std::to_string(seq) +
                  (passed ? " passed" : " failed"));
}

#endif
