# Tide fonts

The two fonts `theme.toml` names, and the licences they are released under.
They ship with the application: the build copies this whole pack, fonts
included, into the app bundle (or beside the executable on Windows and Linux),
and configuring warns if either file has gone missing.

| File | Font | Upstream |
|---|---|---|
| `IBMPlexSans-Regular.ttf` | IBM Plex Sans 3.005, the interface | <https://github.com/IBM/plex> |
| `JetBrainsMono-Regular.ttf` | JetBrains Mono, code and numbers | <https://github.com/JetBrains/JetBrainsMono> |

Both are released under the SIL Open Font License 1.1, which asks that the
licence travel with the font - `IBMPlexSans-OFL.txt` and `JetBrainsMono-OFL.txt`
are those licences, as each project publishes it. They are shipped unmodified;
"Plex" is a Reserved Font Name, so a modified copy would have to be renamed.

Tide Light inherits both entries, so one copy here serves both packs. Your own
**Font file** setting (Settings → Editor) still wins over the editor font named
here. If a file is removed, the interface falls back to the built-in font and
says so in Diagnostics; nothing else about the theme changes.
