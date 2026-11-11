# PSP home-menu art (XMB)

Drop finished files here and rebuild; each is optional and used only if present:

| File        | Size            | Shown                                                    |
|-------------|-----------------|----------------------------------------------------------|
| `ICON0.png` | 144x80          | the game's icon in the Game list                         |
| `PIC1.png`  | 480x272         | full-screen background while the game is highlighted     |
| `PIC0.png`  | 310x180         | an extra picture over the background (logo, info), optional |
| `SND0.at3`  | ATRAC3, < ~500 KB | music while highlighted (needs Sony's at3tool or similar) |

`templates/` has blank files at the right sizes (transparent for ICON0/PIC0, black for
PIC1) and `*_guide.png` overlays to put on a top layer while drawing (delete or hide
the layer before saving). The PIC1 guide's XMB areas are approximate: check on the PSP.

Tips: PNG, 24-bit or 32-bit. ICON0 and PIC0 can use transparency; PIC1 can't (it is
drawn full screen). Keep ICON0's important parts 4 px inside its edges.
