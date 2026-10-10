// rakush — a shell binary that links the rakupp engine in, not the other way
// round. Pipes, { raku } stages and arguments, POSIX $(cmd), brace expansion,
// Raku lines, and `cd` as a host builtin. No job control, no redirects yet.
#include <rakupp/rakupp.h>

#include <editline/readline.h>
#include <glob.h>
#include <pwd.h>

#include <algorithm>
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

static bool run_line(const std::string& line, bool interactive);

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// Skip a quoted run starting at s[i] (a ' " or `); returns the index of the
// closing quote, or s.size() if there is none. Only "..." and `...` know `\`.
static size_t skip_quoted(const std::string& s, size_t i) {
    char q = s[i];
    size_t k = i + 1;
    while (k < s.size() && s[k] != q) k += (q != '\'' && s[k] == '\\') ? 2 : 1;
    return std::min(k, s.size());
}

// The index of the ( or { matching the one at s[i], quote- and escape-aware.
static size_t find_close(const std::string& s, size_t i) {
    char open = s[i], close = open == '(' ? ')' : '}';
    int depth = 0;
    for (size_t k = i; k < s.size(); k++) {
        char c = s[k];
        if (c == '\\') k++;
        else if (c == '\'' || c == '"' || c == '`') k = skip_quoted(s, k);
        else if (c == open) depth++;
        else if (c == close && --depth == 0) return k;
    }
    return std::string::npos;
}

// Split on `sep` at depth 0: not inside {}, (), quotes, or after a backslash.
static std::vector<std::string> splitTop(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t from = 0;
    int depth = 0;
    for (size_t k = 0; k < s.size(); k++) {
        char c = s[k];
        if (c == '\\') k++;
        else if (c == '\'' || c == '"' || c == '`') k = skip_quoted(s, k);
        else if (c == '{' || c == '(') depth++;
        else if (c == '}' || c == ')') depth--;
        else if (c == sep && depth == 0) { out.push_back(s.substr(from, k - from)); from = k + 1; }
    }
    out.push_back(s.substr(from));
    return out;
}

// A word as it is built: its bytes, and for each byte whether it came from
// unquoted literal text. Brace expansion, globbing and ~ act on those bytes
// only — a value from a variable, a command or a { } block is never re-read.
struct Word {
    std::string text;
    std::vector<bool> live;
    bool have = false;   // "" is a word; nothing at all is not
    void add(char c, bool l) { text += c; live.push_back(l); have = true; }
    void add(const std::string& v) { for (char c : v) add(c, false); }
    Word slice(size_t from, size_t to) const {
        Word w;
        w.text = text.substr(from, to - from);
        w.live.assign(live.begin() + from, live.begin() + to);
        w.have = true;
        return w;
    }
    void append(const Word& o) { text += o.text; live.insert(live.end(), o.live.begin(), o.live.end()); have = true; }
};

// $NAME (environment), $0, $$ and $?. Returns how many characters after the
// `$` it consumed, 0 if none. An unset variable adds nothing, not even "".
static size_t expand_var(const std::string& s, size_t i, Word& w) {
    if (i + 1 >= s.size()) return 0;
    char n = s[i + 1];
    if (n == '0') { w.add(argv0); return 1; }
    if (n == '$') { w.add(std::to_string(getpid())); return 1; }
    if (n == '?') { w.add(std::to_string(last_status)); return 1; }
    bool braced = n == '{';
    size_t j = i + 1 + braced;
    if (j >= s.size() || !(std::isalpha((unsigned char)s[j]) || s[j] == '_')) return 0;
    size_t from = j;
    while (j < s.size() && (std::isalnum((unsigned char)s[j]) || s[j] == '_')) j++;
    if (braced && (j >= s.size() || s[j] != '}')) return 0;
    if (const char* v = std::getenv(s.substr(from, j - from).c_str()); v && *v) w.add(v);
    return j - i - 1 + braced;
}

// Evaluate a { } block once and flatten what it returns into words — one
// argument per element, so nothing is ever word-split.
static bool raku_words(const std::string& body, std::vector<std::string>& out) {
    RkValue v;
    std::string src = "(do { " + body + " }).flat.grep(*.defined).map(*.Str).Array";
    if (rk_eval(rk, src.c_str(), &v) != RK_OK) {
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

// $( cmd ) and `cmd`: run the line in a forked copy of the shell and collect
// its stdout, trailing newlines dropped.
static bool capture(const std::string& cmd, std::string& text) {
    int fd[2];
    if (pipe(fd) != 0) { perror("rakush: pipe"); return false; }
    std::cout.flush();
    pid_t pid = fork();
    if (pid == 0) {
        close(fd[0]);
        dup2(fd[1], 1);
        close(fd[1]);
        run_line(cmd, false);
        std::cout.flush();
        _exit(last_status);
    }
    close(fd[1]);
    char buf[65536];
    ssize_t n;
    while ((n = read(fd[0], buf, sizeof buf)) > 0) text.append(buf, n);
    close(fd[0]);
    waitpid(pid, nullptr, 0);
    while (!text.empty() && text.back() == '\n') text.pop_back();
    return true;
}

// {a..e}, {1..10}, {10..1..3}, {01..10}: the sequence form of brace expansion.
static bool brace_sequence(const std::string& body, std::vector<std::string>& items) {
    size_t d1 = body.find("..");
    if (d1 == std::string::npos) return false;
    size_t d2 = body.find("..", d1 + 2);
    std::string a = body.substr(0, d1), b = body.substr(d1 + 2, d2 == std::string::npos ? std::string::npos : d2 - d1 - 2);
    std::string st = d2 == std::string::npos ? "1" : body.substr(d2 + 2);
    auto is_int = [](const std::string& x) {
        size_t k = x[0] == '-' || x[0] == '+';
        return k < x.size() && x.find_first_not_of("0123456789", k) == std::string::npos;
    };
    if (a.empty() || b.empty() || !is_int(st)) return false;
    long step = std::labs(std::atol(st.c_str()));
    if (step == 0) step = 1;
    if (is_int(a) && is_int(b)) {
        long x = std::atol(a.c_str()), y = std::atol(b.c_str());
        bool pad = (a.size() > 1 && a[a[0] == '-'] == '0') || (b.size() > 1 && b[b[0] == '-'] == '0');
        size_t width = pad ? std::max(a.size(), b.size()) : 0;
        for (long v = x; x <= y ? v <= y : v >= y; v += x <= y ? step : -step) {
            std::string t = std::to_string(std::labs(v));
            while (t.size() + (v < 0) < width) t = "0" + t;
            items.push_back((v < 0 ? "-" : "") + t);
        }
        return true;
    }
    if (a.size() == 1 && b.size() == 1 && std::isalpha((unsigned char)a[0]) && std::isalpha((unsigned char)b[0])) {
        for (int v = a[0]; a[0] <= b[0] ? v <= b[0] : v >= b[0]; v += a[0] <= b[0] ? step : -step)
            items.push_back(std::string(1, (char)v));
        return true;
    }
    return false;
}

// Brace expansion as bash and zsh do it: x{,.bak} {a,b}/c {1..5}. Braces with
// no comma and no sequence inside stay literal: {} HEAD@{1}.
static void brace_expand(const Word& w, std::vector<Word>& res) {
    for (size_t i = 0; i < w.text.size(); i++) {
        if (w.text[i] != '{' || !w.live[i]) continue;
        int depth = 0;
        size_t close = std::string::npos;
        std::vector<size_t> commas;
        for (size_t k = i; k < w.text.size(); k++) {
            if (!w.live[k]) continue;
            char c = w.text[k];
            if (c == '{') depth++;
            else if (c == '}' && --depth == 0) { close = k; break; }
            else if (c == ',' && depth == 1) commas.push_back(k);
        }
        if (close == std::string::npos) break;
        std::vector<Word> alts;
        std::vector<std::string> seq;
        if (!commas.empty()) {
            size_t from = i + 1;
            commas.push_back(close);
            for (size_t c : commas) { alts.push_back(w.slice(from, c)); from = c + 1; }
        }
        else if (brace_sequence(w.text.substr(i + 1, close - i - 1), seq)) {
            for (auto& t : seq) { Word a; a.add(t); alts.push_back(a); }
        }
        else continue;
        Word pre = w.slice(0, i), post = w.slice(close + 1, w.text.size());
        for (auto& a : alts) {
            Word n = pre;
            n.append(a);
            n.append(post);
            brace_expand(n, res);
        }
        return;
    }
    res.push_back(w);
}

// The braces of a lone { … } word hold Raku unless they look like brace
// expansion: no blank inside, and a top-level comma or a sequence.
static bool brace_shaped(const std::string& body) {
    if (body.find_first_of(" \t") != std::string::npos) return false;
    std::vector<std::string> seq;
    if (brace_sequence(body, seq)) return true;
    int depth = 0;
    for (char c : body) {
        if (c == '{') depth++;
        else if (c == '}') depth--;
        else if (c == ',' && depth == 0) return true;
    }
    return false;
}

// A finished word: braces, then ~, then globs, on the live bytes only. A glob
// that matches nothing stays as typed, as in bash.
static void expand_word(const Word& w, std::vector<std::string>& out) {
    std::vector<Word> forms;
    brace_expand(w, forms);
    for (Word f : forms) {
        if (!f.text.empty() && f.text[0] == '~' && f.live[0]) {
            size_t end = f.text.find('/');
            if (end == std::string::npos) end = f.text.size();
            std::string user = f.text.substr(1, end - 1), home;
            if (user.empty()) { if (const char* h = std::getenv("HOME")) home = h; }
            else if (struct passwd* pw = getpwnam(user.c_str())) home = pw->pw_dir;
            if (!home.empty()) {
                Word h;
                h.add(home);
                h.append(f.slice(end, f.text.size()));
                f = h;
            }
        }
        bool magic = false;
        std::string pattern;
        for (size_t k = 0; k < f.text.size(); k++) {
            char c = f.text[k];
            bool special = c == '*' || c == '?' || c == '[' || c == ']' || c == '\\';
            if (special && f.live[k] && c != ']' && c != '\\') magic = true;
            if (special && !f.live[k]) pattern += '\\';
            pattern += c;
        }
        glob_t g;
        if (magic && glob(pattern.c_str(), 0, nullptr, &g) == 0) {
            for (size_t k = 0; k < g.gl_pathc; k++) out.emplace_back(g.gl_pathv[k]);
            globfree(&g);
        }
        else {
            if (magic) globfree(&g);
            out.push_back(f.text);
        }
    }
}

// The words of a command: blanks separate them; '…' is literal; "…" expands
// $VAR and $(…) and knows \; a backslash outside quotes takes the next byte
// as is. $( cmd ) and `cmd` give one word per output line; a lone { … } is a
// Raku block, run once, one word per element. Nothing is ever split on blanks.
static bool words(const std::string& s, std::vector<std::string>& out) {
    Word w;
    auto flush = [&] { if (w.have) expand_word(w, out); w = Word(); };
    auto lines_into = [&](const std::string& text) {
        size_t from = 0;
        bool first = true;
        while (from <= text.size()) {
            size_t nl = text.find('\n', from);
            if (nl == std::string::npos) nl = text.size();
            std::string line = text.substr(from, nl - from);
            if (!line.empty()) {
                if (!first) flush();
                w.add(line);
                first = false;
            }
            from = nl + 1;
        }
    };
    for (size_t i = 0; i < s.size(); i++) {
        char ch = s[i];
        if (ch == ' ' || ch == '\t') { flush(); continue; }
        if (ch == '\\') { if (i + 1 < s.size()) w.add(s[++i], false); continue; }
        if (ch == '{' && !w.have) {
            size_t e = find_close(s, i);
            if (e != std::string::npos && (e + 1 == s.size() || s[e + 1] == ' ' || s[e + 1] == '\t')) {
                std::string body = s.substr(i + 1, e - i - 1);
                if (!trim(body).empty() && !brace_shaped(body)) {
                    if (!raku_words(body, out)) return false;
                    i = e;
                    continue;
                }
            }
        }
        if (ch == '$' && i + 1 < s.size() && s[i + 1] == '(') {
            size_t e = find_close(s, i + 1);
            if (e == std::string::npos) { std::cerr << "rakush: unmatched $(\n"; return false; }
            std::string text;
            if (!capture(s.substr(i + 2, e - i - 2), text)) return false;
            lines_into(text);
            i = e;
            continue;
        }
        if (ch == '`') {
            size_t e = skip_quoted(s, i);
            std::string text;
            if (!capture(s.substr(i + 1, e - i - 1), text)) return false;
            lines_into(text);
            i = e;
            continue;
        }
        if (ch == '$') {
            if (size_t used = expand_var(s, i, w)) { i += used; continue; }
        }
        if (ch == '\'') {
            size_t e = skip_quoted(s, i);
            w.have = true;
            for (size_t k = i + 1; k < e; k++) w.add(s[k], false);
            i = e;
            continue;
        }
        if (ch == '"') {
            size_t e = skip_quoted(s, i);
            w.have = true;
            for (size_t k = i + 1; k < e; k++) {
                char c = s[k];
                if (c == '\\' && k + 1 < e && std::strchr("$`\"\\", s[k + 1])) { w.add(s[++k], false); continue; }
                if (c == '$' && k + 1 < e && s[k + 1] == '(') {
                    size_t ce = find_close(s, k + 1);
                    std::string text;
                    if (ce == std::string::npos || ce > e || !capture(s.substr(k + 2, ce - k - 2), text)) return false;
                    w.add(text);
                    k = ce;
                    continue;
                }
                if (c == '$') {
                    if (size_t used = expand_var(s, k, w)) { k += used; continue; }
                }
                w.add(c, false);
            }
            i = e;
            continue;
        }
        w.add(ch, true);
    }
    flush();
    return true;
}

// A { raku } stage runs in a forked copy of the session, as bash runs a
// function in a pipeline in a subshell: it sees every variable, changes none.
// Each input line is $_. What the block returns follows rak's rules: exactly
// True keeps the line; False, Nil and Empty drop it; a Slip gives one line per
// element; anything else replaces the line with its .Str. A returned Regex is
// matched against $_, as `if /x/` would. A line that throws is dropped, and
// only the first failure is reported in full.
[[noreturn]] static void raku_stage(const std::string& body) {
    RkValue code;
    std::string src = "-> $_ { my \\r = do { " + body + " }; "
                      "r ~~ Regex ?? so $_ ~~ r "
                      "!! r =:= True ?? True "
                      "!! r =:= False || r =:= Empty || r =:= Nil ?? False "
                      "!! r ~~ Slip ?? r.map(*.Str).Array "
                      "!! r.Str }";
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
    auto emit = [&](RkValue v) {
        size_t len;
        const char* p = rk_str_get(c, v, &len);
        std::cout.write(p, len) << "\n";
    };
    size_t lineno = 0, failed = 0;
    while (next_line()) {
        lineno++;
        RkValue arg = rk_str(c, line.data(), line.size());
        RkValue r = rk_call_value(c, code, &arg, 1);
        if (const char* err = rk_error(c)) {
            if (failed++ == 0) {
                std::string msg = err;
                while (!msg.empty() && msg.back() == '\n') msg.pop_back();
                std::cerr << "rakush: { } failed on input line " << lineno << ": " << msg << "\n";
            }
            rk_clear_error(c);
            continue;
        }
        switch (rk_type(c, r)) {
            case RK_BOOL:  if (rk_truthy(c, r)) std::cout << line << "\n"; break;
            case RK_ARRAY: for (size_t i = 0, n = rk_elems(c, r); i < n; i++) emit(rk_at_pos(c, r, i)); break;
            case RK_STR:   emit(r); break;
            default:       break;
        }
        if (!std::cout) break;   // the reader went away
    }
    if (failed > 1) std::cerr << "rakush: { } failed on " << failed - 1 << " more input lines\n";
    std::cout.flush();
    _exit(failed ? 1 : 0);
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

// One line: a builtin, a Raku line, or a pipeline. Returns false on `exit`.
static bool run_line(const std::string& raw, bool interactive) {
    static const std::set<std::string> raku_first = {
        "my", "our", "say", "put", "note", "print", "for", "if", "unless", "while",
        "sub", "use", "constant", "class", "role", "enum", "dd", "given", "loop",
        "react", "await", "start", "multi", "grammar", "with", "without"};
    std::string line = trim(raw);
    if (line.empty() || line[0] == '#') return true;
    std::string first = line.substr(0, line.find_first_of(" \t("));
    if (first == "exit") {
        std::vector<std::string> w;
        if (words(line, w) && w.size() > 1) last_status = std::atoi(w[1].c_str());
        return false;
    }
    if (first == "cd") {
        std::vector<std::string> w;
        if (!words(line, w)) { last_status = 1; return true; }
        RkCtx c = rk_ctx(rk);
        RkValue arg = w.size() > 1 ? rk_str(c, w[1].data(), w[1].size()) : nullptr;
        RkValue r = rk_call(c, "cd", &arg, arg ? 1 : 0);
        last_status = rk_error(c) ? 1 : 0;
        if (last_status) { std::cerr << "rakush: cd: " << rk_error(c) << "\n"; rk_clear_error(c); return true; }
        rk_call(c, "sync-cwd", &r, 1);
    }
    else if (line[0] == '=' || raku_first.count(first)) {
        // `=expr` shows the value, the way the REPL would; a keyword line runs as is.
        std::string src = line[0] == '=' ? "say do { " + line.substr(1) + " }" : line;
        last_status = rk_eval(rk, src.c_str(), nullptr) == RK_OK ? 0 : 1;
        if (last_status) std::cerr << "rakush: " << rk_last_error(rk) << "\n";
        std::cout.flush();
    }
    else run_pipeline(line, interactive);
    return true;
}

int main(int argc, char** argv) {
    argv0 = argv[0];
    rk = rk_new(nullptr);
    if (!rk) { std::cerr << "rakush: no interpreter\n"; return 1; }
    rk_register(rk, "cd", host_cd, nullptr);
    // After a host chdir, Raku's $*CWD has to follow.
    rk_eval(rk, "sub sync-cwd(Str $d) { $*CWD = $d.IO }", nullptr);

    // rakush -c 'line': what ssh, vim's :! and other programs ask of $SHELL.
    if (argc > 2 && std::strcmp(argv[1], "-c") == 0) {
        run_line(argv[2], false);
        std::cout.flush();
        rk_free(rk);
        return last_status;
    }

    // Ctrl-C and Ctrl-\ belong to the running command, not to the shell.
    // (A Raku line runs inside the shell, so a runaway one cannot be stopped.)
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    bool tty = isatty(0);
    std::string hist = std::string(std::getenv("HOME")) + "/.rakush_history";
    if (tty) read_history(hist.c_str());

    std::string line;
    while (true) {
        if (tty) {
            char buf[4096];
            std::string here = getcwd(buf, sizeof buf), home = std::getenv("HOME");
            if (here.compare(0, home.size(), home) == 0) here = "~" + here.substr(home.size());
            std::string prompt = here + " rakush$ ";
            char* l = readline(prompt.c_str());
            if (!l) { std::cout << "\n"; break; }
            line = l;
            free(l);
            if (!trim(line).empty()) { add_history(line.c_str()); write_history(hist.c_str()); }
        }
        else if (!std::getline(std::cin, line)) break;   // no prompt when not interactive
        if (!run_line(line, tty)) break;
    }
    rk_free(rk);
    return last_status;
}
