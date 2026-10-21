# dm/i18n: the multilingual string table and the literal gate

中文: [README.zh-CN.md](README.zh-CN.md)

All UI text we added on top of the original game exists in Chinese, English, Spanish and Russian and is registered in one table here; a scanning script makes sure no text bypasses that table. The game's own text is not handled here; it follows the game's own string tables.

## The three files

| File | What it is |
|---|---|
| `ui_text.tsv` | The string table, the single source of the UI text we added. Six tab-separated columns: `键` (key), `中文` (Chinese), `English`, `Español`, `Español MX` (empty = same as `Español`), `Русский`, `用在哪` (where used) |
| `scan_literals.py` | The literal gate: scans the sources for string literals containing Chinese and reports any that reach the screen without going through the table |
| `literal_whitelist.tsv` | The gate's whitelist: Chinese literals that really never reach the screen are registered here. Three columns: `文件` (file), `匹配` (match), `理由` (reason) |

The table is read by `dm/ios/tools/dm_i18n.py`, which does two things with it:

- **Text drawn inside the game**: when the overlay is built, every row is appended to the game's string table, with Chinese in the Simplified and Traditional Chinese columns, Spanish in esES / esMX, Russian in ruRU and English in all the remaining languages. Layouts write `@key`, and the host fetches by key with `dm_str`. Switching language is done by the game itself.
- **Text drawn outside the game** (system alerts, the resource download page and so on): it generates `dm/host/dm_i18n_tab.h`, which is compiled in and read by key with `dm_ui`.

The lookup interface is in `dm/host/dm_i18n.h`.

## Adding a UI string

1. Add a row to `ui_text.tsv` and fill in every language column (`Español MX` may stay empty when it matches `Español`). The rules are enforced by `dm_i18n.py`, which stops with an error when one is broken:

   - The key starts with `dm` followed by letters and digits only, no underscore (`@key` in a layout breaks at an underscore). Keys must be unique and must not clash with a key of the original game.
   - No language column may be empty (except `Español MX`), and none but the Chinese may contain Chinese characters (the language row of the Custom page is the one exception, see `EN_HAS_ZH`).
   - Placeholders (`%d`, `%s` and so on) must match the Chinese column in number, order and type, and the number of line breaks must match the English column.
   - For a line break, write the two characters `\n`.
   - Some keys (the Rift panel, Rift messages and others) have a length limit on the English; exceeding it reports what the limit is. The limits are `EN_LIMITS` in `dm_i18n.py`. Character count is only an approximation, so new English wording still needs a look at a screenshot to see that it stays inside its box.
   - The "where used" column is not checked by any script. Existing rows say which screen the text appears on, which file uses it, and what each placeholder stands for; do the same.

2. Regenerate the header and commit it together with the table:

   ```
   python3 dm/ios/tools/dm_i18n.py --header > dm/host/dm_i18n_tab.h
   ```

3. Use the key in code, never a literal: write `@key` in layout scripts (`dm/ios/tools/touch_layout.py` and others); in the host use `dm_str(main_base, "key")` for text inside the game and `dm_ui("key")` for text outside it.

4. Run the two checks below.

## The two checks

```
python3 dm/ios/tools/dm_i18n.py --check
python3 dm/i18n/scan_literals.py
```

Both only read source files and need no game files; `make -C dm/host/tests run` runs them too.

The first checks every row of the table against the rules above, and that the checked-in `dm/host/dm_i18n_tab.h` was generated from the current table. On success it prints `dm_i18n OK <count> 条`; when the header is stale it prints the command to regenerate it.

The second is the literal gate.

## How the literal gate decides

`scan_literals.py` scans string literals containing Chinese in these files:

- `*.c` and `*.m` under `dm/host/`, and `*.m` and `*.c` under `dm/ios/Sources/`;
- the layout and asset generation scripts whose output is written into the game, listed in `PY_FILES` in the script. A new script of that kind must be added to the list, or the gate will not see it.

These do not count: comments, Python docstrings, and literals whose innermost call is a logging or error function (`LOG`, `fprintf`, `NSLog`, `print` and so on; the list is `SINK` in the script).

Every remaining literal must have a row in `literal_whitelist.tsv`, or the gate is red. The match column takes four forms:

| Form | Meaning |
|---|---|
| A piece of text | The literal contains this text |
| `=` followed by the whole literal | The literal is exactly this |
| `@` followed by a function name | The literal's innermost call is this function |
| `*` | The whole file |

The reason column says why this Chinese never reaches the screen (for example, it only goes to logs). A whitelist row that no longer matches any literal is red as well and has to be removed.

Results:

- Pass: `I18N OK：中文字面量 N 条全在白名单（M 行），其余走中英表`, exit code 0.
- Fail: lines starting with `I18N 红`, listing file, line number and literal one by one, exit code 1.
- `python3 dm/i18n/scan_literals.py --list` lists every literal it found with its innermost call, for use when writing whitelist rows.

When the gate is red, first ask whether the player can see this text. If so, put it in the string table and use the key in code; add a whitelist row only when it really never reaches the screen. The gate only recognizes literals containing Chinese, so it cannot catch hard-coded English; that belongs in the table all the same.
