# Notes for agents working on shadgt7

## Switch names: SHADGT_, RYGT_ or SHARED_ (FEAT-GT7-175)

From FEAT-GT7-175 on, an environment switch is named after where it takes effect:

| Prefix | Read by | Example |
|---|---|---|
| `SHADGT_` | only the renderer (shadgt7: `src/`, `ryty_gpu/`) | `SHADGT_PARALLEL_WORKERS` |
| `RYGT_` | only the runtime (ryGT7: `core/`, `app/`, `plugins/`) | `RYGT_LOG_DIRECTORY` |
| `SHARED_` | both the renderer and the runtime | `SHARED_DIAG` |

- **A new switch** gets the prefix of the code that reads it. Never add a new `RYGT_` switch to the
  renderer or a `SHADGT_` one to the runtime.
- **A switch that starts being read on the other side too** is renamed to `SHARED_` in every
  repository (renderer, runtime, launcher, docs), and added to `SHARED` in the renderer's
  `tools/switch_prefix.py`.
- **The rename is a hard one**: the old names are not read any more. At start the launchers list
  switches saved under an old name in their obsolete-settings question and rename them on Yes
  (`SwitchWalker.FindRenames`), so players keep their settings.
- **A branch from before the rename** brings old names back on merge. Run the tool there after
  merging; a second run changes nothing:

  ```
  python tools/switch_prefix.py --game gt7 --write-map switch-map.json .   # in shadgt7
  python <shadgt7>/tools/switch_prefix.py --apply-map switch-map.json <ryGT7>   # the launcher and runtime
  ```

  It renames exact switch names only (not macros such as `RYGT_SWITCH_EXPORT`), in code, comments
  and docs. `CHANGES.MD` and `CHANGELOG.md` keep the names of their time.

## Every switch needs a description line

The ryGT7 launcher's Advanced page (and the RyGTS one, which reads shadGT the same way) lists the renderer's experimental switches by reading
`libSceVideoOut.prx` as a file: every `"SHADGT_..."`, `"SHARED_..."` or `"RYGT_..."` string the renderer passes to `getenv`
(or `Common::DiagEnv`) becomes a row. The hover tooltip, editor and heading come from metadata
strings in the same DLL, kept in **`src/video_core/rygt_switches.cpp`**.

**When you add a switch** (a new `getenv("SHADGT_...")` or `getenv("SHARED_...")`, in `src/` or `ryty_gpu/`), add one line to
`RyGT_SwitchDescriptions` in that file, under the matching group comment:

```cpp
"RYGTSW1|SHADGT_EXAMPLE_SWITCH|flag|Rendering workarounds|off|1 = on, 0 = off|One sentence on what it does.",
```

Fields, separated by `|` (only the description may contain `|`):

| Field | What to write |
|---|---|
| `RYGTSW1` | Format tag; always this. |
| name | The exact variable name. |
| kind | `flag` (0/1 or presence), `int`, `hex`, `float`, `string` or `choice`. Picks the launcher's editor and validation. Use `choice` (FEAT-GT7-176) when the switch takes only a fixed set of values: the launcher shows a dropdown instead of a text box, where anything else would be ignored. |
| group | One of: `Shader objects & caches`, `Shader compiling`, `Native recorder & sync`, `Memory & VRAM`, `Tessellation`, `Frame pacing & present`, `Rendering workarounds`, `AMD`, `NVIDIA`, `Runtime`, `Diagnostics & tracing` (hidden until the user asks; use it for logs, traces, dumps and stats). |
| default | What happens when the variable is not set (`off`, `on`, `300`, ...). |
| values | What the accepted values mean; say "Diagnostics builds only", "Debug builds only" or "any value turns it on" when true. For a `choice`, only the values, separated by `; `: `value` or `value = meaning`, and `A..B` for every whole number from A to B (e.g. `0 = off; 1 = log; 2 = per frame`, `1..8`); put any other note in the description. |
| description | One plain sentence, about 150 characters at most, for a user deciding whether to set it. Say if it slows the game or writes large logs. |

**Switches the ryGT7 runtime reads** (`core/` in rygt7: libkernel, libc, libScePad, libSceNet,
libSceUserService, ...) are described in this table too: the launcher reads every `libs\*.prx`, the
runtime modules included, and takes a description from whichever file has it. Add their lines under
`Runtime` (or the group that fits) and say which module reads them.

**When you change or remove a switch**, update or delete its line.

- A switch without a line still shows in the launcher, under "Not described in this renderer build".
- A line whose name the build never reads is not shown.
- Keep the table `extern "C"` and exported (`RYGT_SWITCH_EXPORT`): the renderer never reads it, so
  the export is what stops the linker from dropping the strings.
- Keep the switch name a plain string literal at the `getenv` call. The launcher finds names that way,
  so a name built at run time (concatenated or formatted) will not be found.

The launcher side is `loader/AdvancedPage.cs` (`SwitchWalker`) and `loader/ADVANCED-PAGE.md` in ryGT7.
