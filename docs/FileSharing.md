# File sharing (fork extension)

This fork copies files and folders between macOS Finder and Windows File Explorer through the normal clipboard. Select files on one computer, press **Cmd+C** on macOS or **Ctrl+C** on Windows, move to the other computer, and press **Cmd+V** or **Ctrl+V** in the destination folder. Deskflow transfers the files and prepares the destination's native file clipboard automatically. There is no pairing code, separate transfer window, or per-transfer approval step.

## Requirements and use

1. Install this fork on **both** computers and connect them in Deskflow. Enable Deskflow encryption (TLS) and **peer verification** on both. Keep the Deskflow GUI running; it may be hidden in the tray/menu bar.
2. Allow the computers to reach each other on TCP port **24801**. The Windows MSI adds a private-network firewall rule for the Deskflow GUI, limited to the local subnet. A portable Windows installation may need its own firewall rule; macOS may prompt to allow incoming connections.
3. Copy one or more files or folders in Finder or File Explorer, then paste in a folder on the other computer with the usual keyboard shortcut. Files are **copied**, never moved or cut.

The **File → File sharing…** menu (also available from the tray menu) shows transfer progress and errors. It lets you change the received-files folder and maximum transfer size, open that folder, or cancel the current transfer. The default receive location is **Downloads/Deskflow Received**. You do not need to open this view to start a transfer. Text, HTML, and image clipboard sharing continue to use Deskflow's normal clipboard path.

If you paste before a large transfer finishes, Deskflow defers the **keyboard** paste until the downloaded files are ready in the native clipboard. A paste chosen from a Finder/File Explorer menu or right-click menu cannot be deferred: wait for completion, then choose Paste again. Changing the clipboard or cancelling a transfer can invalidate a pending paste. If a paste does not occur after completion, press the paste shortcut again.

## How the transfer works

Each GUI listens for file transfers on TCP **24801**. When a native file copy reaches Deskflow's clipboard channel, the source core creates a fresh 256-bit capability for that clipboard copy and sends its network addresses, port, TLS certificate SHA-256 pin, capability, and clipboard sequence through the existing Deskflow connection. Local file paths and file contents are not sent through that channel.

The destination GUI connects back to the source over a separate TLS connection. It checks the advertised certificate pin before requesting files. The source accepts a request only when the capability and sequence match its current copied files and native clipboard state; copying something else revokes that offer. The destination receives a bounded file manifest and streams file contents, then publishes the verified local copies to Finder's or File Explorer's file clipboard. Deskflow does not automatically paste them into a folder: the user's paste action chooses the destination.

The core and GUI coordinate through small private bridge files. On macOS they use the signed-in user's Deskflow data directory. On Windows they use a session- and user-specific directory in ProgramData whose access is limited to that user and SYSTEM, so the installed elevated core can communicate with the GUI. An older Deskflow build on either computer does not understand this file offer, so both sides need this fork.

## Limits and behavior

- The default maximum is **1 GiB** per transfer, adjustable from **1 MiB to 1 TiB** in File sharing. A selection can contain at most **10,000 entries**, at most **64 path components** per entry, and a bounded file-list message. Data streams in **64 KiB** chunks.
- Incoming files are written to a private temporary location and checked with SHA-256 before being published together under a new folder in the chosen receive directory. Existing files are not overwritten. Failed, disconnected, and cancelled partial transfers are removed. Completed copies remain until you delete them.
- Symbolic links, junctions, special or unreadable files, unsafe paths, case-insensitive name collisions, and names unsupported across Windows/macOS are rejected. Permissions, resource forks, extended attributes, and Finder aliases are not transferred.
- This supports Finder/File Explorer **copy and paste** of files and folders. It does not provide a mounted network drive or cross-screen dragging. A changed clipboard can cancel or supersede an in-progress file offer.

Automated tests cover the transfer protocol and local clipboard behavior. End-to-end keyboard paste and firewall behavior on two physical Windows/macOS machines still need validation.
