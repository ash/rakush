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
~ rakush$ printf '[%s]\n' $(@w)
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
  - `{ … }` as a pipeline stage is a Raku block called once per input line,
    with the line in `$_`. If it returns a Bool, the line is kept or dropped;
    `Nil` drops the line; any other value replaces it.
  - `$( … )` in an argument is a Raku expression. Each element of its list
    becomes one argument.
  - `$NAME`, `$0`, `$$` and `$?` expand. Unquoted `*`, `?`, `[…]` and a
    leading `~` glob.
  - Nothing is ever split on whitespace: `$VAR` holding `x  y` is one argument
    whether it is quoted or not.
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
| `$( … )` arguments | `rk_eval`, then `rk_elems`/`rk_at_pos`/`rk_str_get` |
| `{ … }` stages | `rk_eval` compiles a closure once; `rk_call_value` per line |
| `cd`, callable from Raku as `cd(...)` | `rk_register` |

A `{ … }` stage runs in a forked copy of the shell, as bash runs a function
in a pipeline in a subshell: it sees the session's variables but cannot
change them.

## What is missing

- Redirections (`>`, `>>`, `<`, `2>&1`), `&&`, `||`, `;` and `&`.
- POSIX command substitution: `$( … )` holds Raku at the moment.
- Job control: Ctrl-Z, `fg`, `bg`.
- `rakush -c '…'` and running script files.
- Interrupting a Raku line: it runs inside the shell, so Ctrl-C cannot stop
  a runaway `loop {}`.
- Fork safety: once Raku code has started threads (`start { }`), forking a
  `{ … }` stage can deadlock. The fix is either a helper process forked
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
