# Running unsigned Asciify builds

Everything this project releases is built and published unsigned. This page explains
what each operating system will say, exactly how to get past it, and how to sign the
artifacts yourself if you'd rather not click through warnings.

## Why they're unsigned

Clearing these warnings for redistribution costs money, not effort:

| Platform | What's needed | Cost |
|---|---|---|
| macOS | Apple Developer Program + a *Developer ID Application* certificate, plus notarization of every build | $99/year |
| Windows | An Authenticode code-signing certificate (OV, or EV to skip SmartScreen reputation-building) | ~$200–500/year |

This project carries neither. Nothing is wrong with the downloads — the OS simply has
no publisher identity to check them against, so it assumes the worst once and then
remembers your answer.

> If you'd rather not trust a stranger's binary at all, every release is reproducible
> from source — see the build instructions in the README.

## macOS — Gatekeeper

macOS quarantines anything downloaded from a browser. For a plugin this is worse than
for an app: your host doesn't prompt, it just **skips the plugin or fails validation**,
usually with no useful error. Clear the flag after copying the bundle into place:

```sh
xattr -dr com.apple.quarantine "$HOME/Documents/Resolume Arena/Extra Effects/Asciify.bundle"
```

Use `Resolume Avenue` in place of `Resolume Arena` for Avenue, or
`/Library/Graphics/FreeFrame Plug-Ins/` if you installed system-wide. Then
restart Resolume — it reads the plugin folder at startup.

### Apple Silicon and the `.zip` trap

If you copy an app out of a `.zip` with Finder the quarantine flag comes with it. Prefer the
`.dmg` or `.pkg`, or run the `xattr` command above after copying.

## Windows — SmartScreen

**"Windows protected your PC — Microsoft Defender SmartScreen prevented an
unrecognised app from starting."** Click **More info**, confirm the publisher line reads
*Unknown publisher*, then **Run anyway**.

To clear the mark-of-the-web before running instead — useful when the block is silent
rather than a prompt:

```powershell
Unblock-File .\<file>.exe
```

Or right-click the file → **Properties** → tick **Unblock** → **OK**. If you downloaded a
`.zip`, **unblock the `.zip` first, then extract** — otherwise every extracted file
inherits the flag and you'll be unblocking them one at a time.

### Defender antivirus false positives

Unsigned binaries that bundle a runtime occasionally get quarantined outright by
Defender's heuristics rather than merely warned about. If the download vanishes from
your Downloads folder, check **Windows Security → Virus & threat protection →
Protection history** and choose **Restore**. Add an exclusion for the install folder if
it keeps happening.

## Signing it yourself

### macOS — ad-hoc (local machine only)

An ad-hoc signature stops the OS re-prompting on **your own machine**. It is **not**
notarization and will do nothing for anyone else:

```sh
codesign --force --deep --sign - "$HOME/Documents/Resolume Arena/Extra Effects/Asciify.bundle"
```

Verify it took:

```sh
codesign -dv --verbose=4 "$HOME/Documents/Resolume Arena/Extra Effects/Asciify.bundle"
spctl -a -vv "$HOME/Documents/Resolume Arena/Extra Effects/Asciify.bundle"   # still reports "rejected" — ad-hoc is not notarization
```

### macOS — real signing and notarization

With an Apple Developer Program membership and a *Developer ID Application* certificate:

```sh
codesign --force --deep --options runtime --timestamp \
  --sign "Developer ID Application: Your Name (TEAMID)" "<artifact>"
xcrun notarytool submit "<artifact>.zip" --apple-id you@example.com \
  --team-id TEAMID --password "app-specific-password" --wait
xcrun stapler staple "<artifact>"
```

Note the **hardened runtime** (`--options runtime`) — notarization rejects builds without
it, and a hardened build with an ad-hoc signature won't launch at all.

### Windows — Authenticode

```powershell
signtool sign /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 `
  /f mycert.pfx /p <password> .\<file>.exe
```

An OV certificate still needs to build SmartScreen reputation over time; an EV
certificate is trusted immediately. Neither is free.

## Verifying a download

Signing proves *who* built it; a checksum proves you got *what they built* — worth doing
even unsigned. Compare against the release notes:

```sh
shasum -a 256 <file>        # macOS / Linux
certutil -hashfile <file> SHA256   # Windows
```

You can also confirm the artifact came from this repo's CI by checking the release page it
was downloaded from — GitHub shows the workflow run that produced each asset.
