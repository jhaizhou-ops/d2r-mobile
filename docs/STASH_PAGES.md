# More stash pages

中文: [STASH_PAGES.zh-CN.md](STASH_PAGES.zh-CN.md)

By default the stash is the same as the original: 5 personal pages and 5 shared pages. You can raise both with a small text file. This page covers how to turn it on, what it changes in your saves, and how to go back.

## Turning it on

Create `stash_pages.txt` in the same folder as `sw_custom.txt` (the Custom page's settings file):

| Platform | Folder |
|---|---|
| iOS | the app's Documents folder (visible in the Files app) |
| Mac App | `~/Library/Application Support/D2R Mobile` |
| Android / HarmonyOS | the folder that holds the save directory |

```
# personal stash pages per character, 1..64 (default 5)
personal_pages=10
# shared stash pages, 5..99 (default 5)
shared_pages=20
```

- `personal_pages` is read every time a character enters a game.
- `shared_pages` is read once per launch: the game's server builds its page table from it, so restart the app after changing it.
- Lowering either number never hides a page that still holds items. The count used is the larger of the setting and the last page with something on it.

Page turns are instant, and an item held in the hand stays in the hand when you turn the page.

## What it changes in your saves

- **Personal pages 2 and up** live in a side file next to the character, `<character>/<character>.swpage`.
  - While pages 6 and up are empty, it is written in the same format as before, byte for byte.
  - Once something is on page 6 or later, it is written in a newer format. A build without this change cannot read that format and treats the side file as empty.
- **Shared pages** live in the game's own shared stash file, `ModernSharedStash*/<name>.d2i`.
  - A build with fewer pages opening it most likely drops the extra pages.
  - To protect against that, every save with more than 5 shared pages also writes a full copy, `<name>.swshared`, next to it.
  - When the save is mounted and the `.d2i` has fewer pages than the copy (it was saved by a build with fewer pages), the pages that build could not see are put back from the copy. The file as it was before is kept as `<name>.d2i.before-restore`.
- iCloud sync carries both side files along with their folders.

So: every device that plays the same saves should run a build with this change. If one does not, its personal pages 6+ look empty there, and its shared pages 6+ come back once you open the save again on a device that has the change.

## Going back to the original format

Use this before moving your saves to a build without this change:

1. Move every item off personal pages 6+ and shared pages 6+, onto pages 1–5 or into your inventory.
2. Set `personal_pages=5` and `shared_pages=5` (or delete `stash_pages.txt`) and restart the app.
3. Enter a game with each character once and leave, so every file is saved again with 5 pages.
4. Delete the `<name>.swshared` copies in the `ModernSharedStash*` folders. The `.swpage` side files can stay: with pages 6+ empty they are already in the old format.
