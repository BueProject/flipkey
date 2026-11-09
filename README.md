# FlipKey

Typed in the wrong keyboard language? Press one key and the text is fixed.

FlipKey is a small Windows tray program for any two keyboard languages, such as Hebrew and English or Russian and English. `akuo` becomes `שלום`, and pressing the key again brings the original back.

## Download

Get the latest `.exe` from [Releases](../../releases/latest). There is nothing to install: put it in a permanent folder and run it. The first time it opens, you choose your two languages and your key.

Windows SmartScreen may warn you because the program is not code-signed.

## How it works

- **Your hotkey** (F10, Ctrl + L, or your own) flips the highlighted text, or the current line from its start to the cursor.
- **One-word shortcut:** your hotkey plus Ctrl (or plus Shift if it already has Ctrl) flips only the last word.
- **Press again** to restore the original.
- **Lines wrong in both directions** are handled word by word for languages with different alphabets: `tbh ודנ` becomes `אני usb`.
- **Words in all capital English letters stay as they are**, so `USB` is never flipped.

## Privacy

FlipKey does not record or watch your typing. Windows tells it only when the hotkey is pressed. The clipboard is used briefly during a flip and then restored.

The program connects to the internet only if you tick **Check for updates once a day**, which is off by default. It then asks GitHub for the latest version, and nothing is installed until you agree.

## Known limits

- An English word typed on the Hebrew keyboard may lose its capitals, so `YouTube` can become `youtube`.
- On a line that wraps, only the visible part with the cursor is flipped.
- Every word on the line flips except all-capital English words, including words that were already correct. Highlight only the wrong part, or use the one-word shortcut, for those lines.
- For languages that share an alphabet, such as English and French, the letters can't show the language. FlipKey converts the whole line from the keyboard currently active, so press the hotkey before switching keyboards. Letters made with two key presses, like `ê`, may stay as they are.
- It does nothing in CMD or PowerShell, where Ctrl+C would stop the running program.
- To work in windows running as administrator, FlipKey must also run as administrator.

## Building

With [Zig](https://ziglang.org):

```
zig cc -target x86_64-windows-gnu -Os -s -Wl,--subsystem,windows flipkey.c flipkey.rc -o FlipKey.exe -luser32 -lgdi32 -lshell32 -ladvapi32 -lwinhttp
```

## License

MIT
