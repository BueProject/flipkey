# FlipKey

Typed in the wrong keyboard language? Press one key and the text is fixed.

FlipKey is a small Windows tray program for any two keyboard languages, such as Hebrew and English or Russian and English. `akuo` becomes `שלום`, and pressing the key again right away brings the original back.

## Download

Get the latest `.exe` from [Releases](../../releases/latest). There is nothing to install: put it in a permanent folder and run it. The first time it opens, you choose your two languages and your key. Closing that window without saving keeps the defaults.

Windows SmartScreen may warn you because the program is not code-signed.

## How it works

- **Your hotkey** (F10, Ctrl + L, or your own) flips the highlighted text, or the current line from its start to the cursor.
- **One-word shortcut:** your hotkey plus Ctrl (or plus Shift if it already has Ctrl) flips only the last word, meaning the text after the last space, tab or line break. A key that already uses both Ctrl and Shift has no one-word shortcut.
- **Press again** within 30 seconds, in the same window, to restore the original. This works for up to 4,096 characters. If the flipped text spans several lines, highlight it again first.
- **Lines wrong in both directions** are handled word by word for languages with different alphabets: `tbh ודנ` becomes `אני usb`.
- **A single word in all capital English letters stays as it is**, so `USB` on its own is not flipped. When there are two or more words, all of them flip, capitals included.
- **To change your languages, key or other settings,** click the FlipKey icon in the system tray and choose "Change hotkey".

## Privacy

FlipKey does not record or log your typing. In normal use, Windows tells it only when the hotkey is pressed. While you choose a key in the settings, it listens to the keyboard temporarily to see what you press. The clipboard is used briefly during a flip and restored on a best-effort basis (unusual formats such as metafiles are not kept), so the original and flipped text can appear in your Windows clipboard history (Win+V) if that is turned on.

The program connects to the internet only if you tick **Check for updates at startup and daily**, which is off by default. It then asks GitHub for the latest version, and nothing is installed until you agree. A download is refused unless it matches the fingerprint GitHub publishes for it. This protects against damaged downloads; the program itself is not code-signed.

## Known limits

- If a correct English word is flipped to Hebrew and later flipped back as new text, its capitals are lost.
- On a line that wraps, only the visible part with the cursor is flipped.
- Every word in the flipped text changes, including words that were already correct. Highlight only the wrong part, or use the one-word shortcut, for those lines.
- For languages that share an alphabet, such as English and French, the letters can't show the language. FlipKey converts all of the flipped text from the keyboard currently active, so press the hotkey before switching keyboards. Letters made with two key presses, like `ê`, may stay as they are.
- FlipKey stays out of console windows and common terminal programs such as Windows Terminal, Git Bash, ConEmu and PuTTY, because Ctrl+C would stop the running program. **It can't detect the terminal inside VS Code, Cursor or other editors, so don't press the hotkey there while something is running.**
- In editors that copy the whole line when nothing is highlighted (VS Code, Cursor, Visual Studio, JetBrains IDEs), one highlighted line that includes its line break counts as nothing highlighted.
- To work in windows running as administrator, FlipKey must also run as administrator.

## Building

With [Zig](https://ziglang.org):

```
zig cc -target x86_64-windows-gnu -Os -s -Wl,--subsystem,windows flipkey.c flipkey.rc -o FlipKey.exe -luser32 -lgdi32 -lshell32 -ladvapi32 -lwinhttp -lbcrypt
```

## License

MIT
