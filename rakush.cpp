// rakush — a shell binary that links the rakupp engine in, not the other way
// round. Pipes, $(raku) arguments, { raku } pipeline stages, Raku lines, and
// `cd` as a host builtin. No job control, no redirects yet.
#include <rakupp/rakupp.h>

#include <editline/readline.h>
#include <glob.h>

#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <set>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

static RkInterp rk;
static std::string argv0 = "rakush";
static int last_status = 0;

// $NAME (environment), $0, $$ and $? — expanded in place, never word-split.
// Returns how many characters after the `$` it consumed, 0 if none.
static size_t expand_var(const std::string& s, size_t i, std::string& cur) {
    if (i + 1 >= s.size()) return 0;
    char n = s[i + 1];
    if (n == '0') { cur += argv0; return 1; }
    if (n == '$') { cur += std::to_string(getpid()); return 1; }
    if (n == '?') { cur += std::to_string(last_status); return 1; }
    if (!(std::isalpha((unsigned char)n) || n == '_')) return 0;
    size_t j = i + 1;
    while (j < s.size() && (std::isalnum((unsigned char)s[j]) || s[j] == '_')) j++;
    if (const char* v = std::getenv(s.substr(i + 1, j - i - 1).c_str())) cur += v;
    return j - i - 1;
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// Split on `sep` at depth 0: not inside {}, () or quotes.
static std::vector<std::string> splitTop(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    char q = 0;
    for (char ch : s) {
        if (q) { if (ch == q) q = 0; }
        else if (ch == '\'' || ch == '"') q = ch;
        else if (ch == '{' || ch == '(') depth++;
        else if (ch == '}' || ch == ')') depth--;
        else if (ch == sep && depth == 0) { out.push_back(cur); cur.clear(); continue; }
        cur += ch;
    }
    out.push_back(cur);
    return out;
}

// Evaluate a Raku expression and flatten it into words — one argv entry per
// list element, so nothing is ever word-split.
static bool raku_words(const std::string& expr, std::vector<std::string>& out) {
    RkValue v;
    if (rk_eval(rk, ("(" + expr + ").map(*.Str).Array").c_str(), &v) != RK_OK) {
        std::cerr << "rakush: " << rk_last_error(rk) << "\n";
        return false;
    }
    RkCtx c = rk_ctx(rk);
    for (size_t i = 0, n = rk_elems(c, v); i < n; i++) {
        size_t len;
        const char* p = rk_str_get(c, rk_at_pos(c, v, i), &len);
        out.emplace_back(p, len);
    }
    return true;
}

// An unquoted word with glob characters or a leading ~ expands like sh does;
// one that matches nothing stays as it was.
static void push_word(const std::string& w, bool quoted, std::vector<std::string>& out) {
    if (quoted || (w.find_first_of("*?[") == std::string::npos && w[0] != '~')) {
        out.push_back(w);
        return;
    }
    glob_t g;
    if (glob(w.c_str(), GLOB_NOCHECK | GLOB_TILDE, nullptr, &g) == 0)
        for (size_t i = 0; i < g.gl_pathc; i++) out.emplace_back(g.gl_pathv[i]);
    else out.push_back(w);
    globfree(&g);
}

// Words of a command stage: whitespace-separated, quotes respected,
// $( raku ) expanded to as many words as the list has elements (never globbed).
static bool words(const std::string& s, std::vector<std::string>& out) {
    std::string cur;
    bool have = false, quoted = false;
    size_t used;
    for (size_t i = 0; i < s.size(); i++) {
        char ch = s[i];
        if (ch == '$' && i + 1 < s.size() && s[i + 1] == '(') {
            int depth = 1;
            size_t j = i + 2;
            for (; j < s.size() && depth; j++) depth += s[j] == '(' ? 1 : s[j] == ')' ? -1 : 0;
            if (!raku_words(s.substr(i + 2, j - i - 3), out)) return false;
            i = j - 1;
        }
        else if (size_t before = cur.size(); ch == '$' && (used = expand_var(s, i, cur)) > 0) {
            i += used;
            have = have || cur.size() > before;   // an unset variable leaves no word behind
        }
        else if (ch == '\'' || ch == '"') {
            size_t j = s.find(ch, i + 1);
            if (j == std::string::npos) j = s.size();
            for (size_t k = i + 1; k < j; k++) {
                size_t used = ch == '"' && s[k] == '$' ? expand_var(s, k, cur) : 0;
                if (used) k += used;
                else cur += s[k];
            }
            have = quoted = true;
            i = j;
        }
        else if (ch == ' ' || ch == '\t') {
            if (have) push_word(cur, quoted, out);
            cur.clear();
            have = quoted = false;
        }
        else { cur += ch; have = true; }
    }
    if (have) push_word(cur, quoted, out);
    return true;
}

// A { raku } stage runs in a forked copy of the session, as bash runs a
// function in a pipeline in a subshell: it sees every variable, changes none.
// Each input line is $_. A Bool filters, Nil drops, anything else replaces.
[[noreturn]] static void raku_stage(const std::string& body) {
    RkValue code;
    std::string src = "-> $_ { my \\r = do { " + body + " }; "
                      "r ~~ Bool ?? r !! r.defined ?? r.Str !! Nil }";
    if (rk_eval(rk, src.c_str(), &code) != RK_OK) {
        std::cerr << "rakush: " << rk_last_error(rk) << "\n";
        _exit(1);
    }
    RkCtx c = rk_ctx(rk);
    code = rk_root(c, code);
    // Read fd 0 itself: std::cin's buffer was forked along with the shell and
    // still holds whatever the shell had read ahead of its own input.
    std::string line, pending;
    char chunk[65536];
    auto next_line = [&]() -> bool {
        for (;;) {
            size_t nl = pending.find('\n');
            if (nl != std::string::npos) { line = pending.substr(0, nl); pending.erase(0, nl + 1); return true; }
            ssize_t n = read(0, chunk, sizeof chunk);
            if (n <= 0) { if (pending.empty()) return false; line.swap(pending); pending.clear(); return true; }
            pending.append(chunk, n);
        }
    };
    while (next_line()) {
        RkValue arg = rk_str(c, line.data(), line.size());
        RkValue r = rk_call_value(c, code, &arg, 1);
        if (rk_error(c)) { std::cerr << "rakush: " << rk_error(c) << "\n"; _exit(1); }
        switch (rk_type(c, r)) {
            case RK_BOOL: if (rk_truthy(c, r)) std::cout << line << "\n"; break;
            case RK_ANY:  break;
            default: {
                size_t len;
                const char* p = rk_str_get(c, r, &len);
                std::cout.write(p, len) << "\n";
            }
        }
        if (!std::cout) break;   // the reader went away
    }
    std::cout.flush();
    _exit(0);
}

static void run_pipeline(const std::string& line, bool interactive) {
    std::vector<std::string> stages = splitTop(line, '|');
    std::vector<pid_t> pids;
    int in = -1;
    // A stage that cannot start still leaves the ones before it running:
    // close their input and reap them before reporting the failure.
    auto abandon = [&] {
        if (in >= 0) close(in);
        for (pid_t p : pids) waitpid(p, nullptr, 0);
        last_status = 1;
    };
    std::cout.flush();
    for (size_t i = 0; i < stages.size(); i++) {
        std::string st = trim(stages[i]);
        bool last = i + 1 == stages.size();
        int fd[2] = {-1, -1};
        if (!last && pipe(fd) != 0) { perror("rakush: pipe"); abandon(); return; }
        std::vector<std::string> argv;
        bool is_raku = st.size() >= 2 && st.front() == '{' && st.back() == '}';
        if (!is_raku && (!words(st, argv) || argv.empty())) {
            if (!last) { close(fd[0]); close(fd[1]); }
            abandon();
            return;
        }
        pid_t pid = fork();
        if (pid == 0) {
            signal(SIGPIPE, SIG_DFL);
            signal(SIGINT, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            if (in >= 0) { dup2(in, 0); close(in); }
            if (!last) { dup2(fd[1], 1); close(fd[0]); close(fd[1]); }
            if (is_raku) raku_stage(st.substr(1, st.size() - 2));
            std::vector<char*> cargv;
            for (auto& a : argv) cargv.push_back(a.data());
            cargv.push_back(nullptr);
            execvp(cargv[0], cargv.data());
            std::cerr << "rakush: " << argv[0] << ": " << std::strerror(errno) << "\n";
            _exit(127);
        }
        pids.push_back(pid);
        if (in >= 0) close(in);
        if (!last) { close(fd[1]); in = fd[0]; }
    }
    int status = 0;
    for (pid_t p : pids) waitpid(p, &status, 0);
    last_status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (!interactive) return;   // a script reads $?; only a person needs the note
    if (WIFEXITED(status) && WEXITSTATUS(status)) std::cerr << "[exit " << WEXITSTATUS(status) << "]\n";
    else if (WIFSIGNALED(status) && WTERMSIG(status) != SIGPIPE) std::cerr << "[signal " << WTERMSIG(status) << "]\n";
}

// cd has to change the SHELL's own directory, so it is a host builtin — and
// Raku code can call it too, through rk_register.
static RkValue host_cd(RkCtx c, void*) {
    std::string dir;
    if (rk_argc(c)) { size_t n; const char* p = rk_str_get(c, rk_arg(c, 0), &n); dir.assign(p, n); }
    else dir = std::getenv("HOME");
    if (chdir(dir.c_str()) != 0) rk_die(c, std::strerror(errno));
    char buf[4096];
    return rk_str(c, buf, std::strlen(getcwd(buf, sizeof buf)));
}

int main(int, char** argv) {
    argv0 = argv[0];
    rk = rk_new(nullptr);
    if (!rk) { std::cerr << "rakush: no interpreter\n"; return 1; }
    rk_register(rk, "cd", host_cd, nullptr);
    // After a host chdir, Raku's $*CWD has to follow.
    rk_eval(rk, "sub sync-cwd(Str $d) { $*CWD = $d.IO }", nullptr);

    static const std::set<std::string> raku_first = {
        "my", "our", "say", "put", "note", "print", "for", "if", "unless", "while",
        "sub", "use", "constant", "class", "role", "enum", "dd", "given", "loop",
        "react", "await", "start", "multi", "grammar", "with", "without"};

    // Ctrl-C and Ctrl-\ belong to the running command, not to the shell.
    // (A Raku line runs inside the shell, so a runaway one cannot be stopped.)
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    bool tty = isatty(0);
    std::string hist = std::string(std::getenv("HOME")) + "/.rakush_history";
    if (tty) read_history(hist.c_str());

    std::string line;
    while (true) {
        char buf[4096];
        std::string here = getcwd(buf, sizeof buf), home = std::getenv("HOME");
        if (here.compare(0, home.size(), home) == 0) here = "~" + here.substr(home.size());
        std::string prompt = here + " rakush$ ";
        if (tty) {
            char* l = readline(prompt.c_str());
            if (!l) { std::cout << "\n"; break; }
            line = l;
            free(l);
            if (!trim(line).empty()) { add_history(line.c_str()); write_history(hist.c_str()); }
        }
        else if (!std::getline(std::cin, line)) break;   // no prompt when not interactive
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        std::string first = line.substr(0, line.find_first_of(" \t("));
        if (first == "exit") {
            std::vector<std::string> w;
            if (words(line, w) && w.size() > 1) last_status = std::atoi(w[1].c_str());
            break;
        }
        if (first == "cd") {
            std::vector<std::string> w;
            if (!words(line, w)) continue;
            RkCtx c = rk_ctx(rk);
            RkValue arg = w.size() > 1 ? rk_str(c, w[1].data(), w[1].size()) : nullptr;
            RkValue r = rk_call(c, "cd", &arg, arg ? 1 : 0);
            last_status = rk_error(c) ? 1 : 0;
            if (last_status) { std::cerr << "rakush: cd: " << rk_error(c) << "\n"; rk_clear_error(c); continue; }
            rk_call(c, "sync-cwd", &r, 1);
        }
        else if (line[0] == '=' || raku_first.count(first)) {
            // `=expr` shows the value, the way the REPL would; a keyword line runs as is.
            std::string src = line[0] == '=' ? "say do { " + line.substr(1) + " }" : line;
            last_status = rk_eval(rk, src.c_str(), nullptr) == RK_OK ? 0 : 1;
            if (last_status) std::cerr << "rakush: " << rk_last_error(rk) << "\n";
            std::cout.flush();
        }
        else run_pipeline(line, tty);
    }
    rk_free(rk);
    return last_status;
}
