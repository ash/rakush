# rakush

An experimental Unix shell with Raku built in. The name is Raku + sh.

rakush is a separate program that links the [rakupp](https://github.com/ash/rakupp)
engine in as a library. rakupp itself knows nothing about rakush.

**Status:** an experiment of a few hundred lines. It is fun to play with,
but it is not a shell for daily use yet (see [What is missing](#what-is-missing)).

## A session

```text
~ rakush$ =1/3 + 1/6
0.5
~ rakush$ printf 'apple\nbanana\ncherry\n' | { .contains('an') }
banana
~ rakush$ yes | { $_ ~ '!' } | head -2
y!
y!
~ rakush$ my @w = 'a b', 'c'
~ rakush$ printf '[%s]\n' { @w }
[a b]
[c]
```

## How a line is read

- A line that starts with a Raku keyword (`my`, `say`, `for`, `if`, `sub`,
  `react`, …) is Raku. All Raku lines run in one session, so variables live
  from line to line.
- `=expr` is Raku too, and prints the value.
- Anything else is a command line:
  - `a | b | c` is a pipeline.
  - `{ … }` is a Raku block, and where it stands decides how often it runs,
    as in Raku itself (`.map` calls a block per element; a string calls it
    once):
    - as a pipeline stage, once per input line, with the line in `$_`. What
      it returns follows the rules of lizmat's
      [rak](https://github.com/lizmat/App-Rak): exactly `True` keeps the
      line; `False`, `Nil` and `Empty` drop it (so `{ .uc unless .starts-with('#') }`
      skips comments); a Slip gives one line per element
      (`{ .words.Slip }`); any other value replaces the line, so `0` and `""`
      are printed, not taken as false. A returned regex is matched against
      `$_`, so `{ /foo/ }` is a grep. A line whose block throws is dropped;
      the first error is reported, and the stage exits with 1.
    - as a word of its own, once. Each element of what it returns becomes one
      argument.
  - `$( cmd )` and `` `cmd` `` are POSIX command substitution. Each output
    line becomes one argument; inside double quotes, the whole output is one.
  - Brace expansion works as in bash and zsh: `x{,.bak}`, `{a,b}/c`,
    `{1..5}`, `{01..10}`, `{a..e}`. Braces with no comma or sequence inside
    stay literal, so `find … -exec cmd {} \;` and `HEAD@{1}` work. A lone
    `{ … }` is Raku unless it looks like brace expansion (no blank inside,
    and a top-level comma or a sequence).
  - `$NAME`, `${NAME}`, `$0`, `$$` and `$?` expand. Unquoted `*`, `?`, `[…]`
    glob, and a leading `~` is your home directory.
  - `'…'` is literal; `"…"` expands variables and `$( … )`, and `\` escapes
    `$`, `` ` ``, `"` and `\` inside it; outside quotes `\` escapes any
    character.
  - Nothing is ever split on whitespace: `$VAR` holding `x  y` is one argument
    whether it is quoted or not. A value is never re-read either: a `*` that
    came from a variable, a command or a block stays a `*`.
- `cd` and `exit` are built in.

Arrow keys and history work; history is kept in `~/.rakush_history`. Ctrl-C
stops the running command, not the shell.

## Building

You need a rakupp checkout built with cmake (rakush links its static
archives) and libedit (`libedit-dev` on Debian and Ubuntu; part of macOS).

```bash
make RAKUPP=~/raku++
make test
```

On an arm64 iMac the binary is 20 MB and starts in under 20 ms.

## How it works

rakupp ships its runtime as static archives with a C API,
[`rakupp.h`](https://github.com/ash/rakupp/blob/main/include/rakupp/rakupp.h).
The `rakupp` command is one program built on them; rakush is another.

| rakush feature | rakupp API |
|---|---|
| Raku lines, kept in one session | `rk_eval` in the mainline scope |
| `{ … }` arguments | `rk_eval`, then `rk_elems`/`rk_at_pos`/`rk_str_get` |
| `{ … }` stages | `rk_eval` compiles a closure once; `rk_call_value` per line |
| `cd`, callable from Raku as `cd(...)` | `rk_register` |

A `{ … }` stage runs in a forked copy of the shell, as bash runs a function
in a pipeline in a subshell: it sees the session's variables but cannot
change them. `$( … )` runs its line in a forked copy too, so it can hold
anything a prompt line can, Raku lines included: `echo $( say 6 * 7 )`.

## What is missing

- Redirections (`>`, `>>`, `<`, `2>&1`), `&&`, `||`, `;` and `&`.
- Subshells `( … )` and group commands `{ a; b; }`; in rakush a `{ … }`
  stage is always Raku.
- Job control: Ctrl-Z, `fg`, `bg`.
- Running script files (`rakush -c 'line'` works).
- Interrupting a Raku line: it runs inside the shell, so Ctrl-C cannot stop
  a runaway `loop {}`.
- Fork safety: once Raku code has started threads (`start { }`), forking a
  `{ … }` stage or a `$( … )` can deadlock. The fix is either a helper process forked
  before any thread exists, or running stages in-process.

## Related work

- [RakuSH](https://github.com/FCO/RakuSH) — an earlier idea for a Raku shell
  that passes data structures between commands.
- [xonsh](https://xon.sh) — Python and shell in one language.
- [Rash](https://docs.racket-lang.org/rash/) — a shell language embedded in Racket.
- [Nushell](https://www.nushell.sh) — structured data in pipelines.
- [Oils](https://oils.pub) — a bash-compatible shell plus a new language.

## License

Artistic License 2.0, the same as rakupp.
