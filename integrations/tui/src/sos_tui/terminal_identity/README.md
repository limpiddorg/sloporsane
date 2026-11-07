# Terminal identity

The client calls `resolve_terminal_identity()` once at startup. It returns an
immutable `TerminalIdentity(key, source)` or `None`. Persist only the opaque key;
`source` is diagnostic. The app sends the key through `integration.configure`;
the daemon persists conversation openings and `/resume` groups matching sessions.
Identity detection itself has no storage or UI side effects.
Detectors implement `TerminalDetector.detect(context)` and share OS probes.

Resolution order is multiplexers, native emulator, then OS fallback. All keys
are versioned SHA-256 digests scoped by host name and OS user. This is grouping
metadata, not authentication or a security boundary.

| Environment | Particularity |
| --- | --- |
| tmux | Uses the pane's TTY and OS session birth, not the outer emulator. |
| Zellij | Uses the same pane scope. Session names are neither unique per pane nor reliable after renaming; no undocumented pane variable is assumed. |
| Nested multiplexers / screen | Ignore inherited emulator IDs and use the actual TTY fallback. |
| iTerm2 | `ITERM_SESSION_ID`; includes native session identity. |
| Terminal.app / other session-aware terminals | `TERM_SESSION_ID` when provided; generic native session identity. |
| Ghostty | `GHOSTTY_SURFACE_ID`, scoped to the TTY/session birth. Older versions fall back. |
| WezTerm | `WEZTERM_PANE`, scoped to the TTY/session birth to avoid recycled numeric pane IDs. |
| kitty | `KITTY_PID` and `KITTY_WINDOW_ID`, also scoped to session birth. |
| Konsole | `KONSOLE_DBUS_SERVICE` and `KONSOLE_DBUS_SESSION`, also scoped to session birth. |
| Windows Terminal | `WT_SESSION` GUID, including WSL when forwarded. |
| VS Code, Cursor, GNOME Terminal, Alacritty, xterm | OS fallback; no assumed native pane ID. |
| SSH | Ignore forwarded native emulator variables. Identify the remote TTY/session; a new SSH connection generally creates a new identity. Remote tmux/Zellij panes retain their identities on reattachment. |

On Linux, the OS fallback uses TTY, session-leader PID, process start ticks and
boot ID. On macOS it uses TTY, session-leader PID, process start date from `ps`
and boot time from `sysctl` (start-date precision is one second). Session leaders
must still be observable. Missing probes return `None` rather than guessing.
Other OSes currently have no fallback; native GUIDs can still work.

Client and daemon restarts do not contribute to identity. Closing a pane,
restarting its login/session leader, rebooting, changing host name or changing
OS user can create a new identity. Restored emulator windows are not guaranteed
to keep their old OS-scoped identity. Environment variables inherited after
moving shells between terminals can be stale; there is no universal emulator
introspection protocol.

Unit tests cover detector selection and failure cases. A real Unix PTY test
checks two independent clients within the same OS session. Actual emulator
and SSH/multiplexer end-to-end checks still require those applications.
