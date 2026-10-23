# File sharing (fork extension)

This fork adds bidirectional file and folder transfer between Windows and macOS.
Both computers must use this fork. File data uses a separate TLS connection; the
keyboard/mouse protocol remains compatible with upstream Deskflow.

## Setup

1. Open **File → File sharing…** (also available from the tray menu) on both computers.
2. Enable file sharing, select a receiving directory and apply settings.
3. Check **My address** is reachable from the other computer. Copy **my pairing code**
   to the other computer's **Other computer** field, and exchange codes in both directions.
4. Apply settings again. Enable automatic sending of copied files if wanted.
5. Copy files or folders in Finder/Explorer. Approve the incoming request on the
   destination, wait for the completion notification, then paste in Finder/Explorer.
   You can also choose files/folders in the transfer window or drop them onto it.

The transfer window shows progress, completion and failures. **Cancel transfers**
removes unfinished incoming data. Completed files remain in the receiving folder.
**Copy received files for pasting** restores their native file clipboard entry.
If you copy something else during a transfer, its clipboard contents are preserved;
use that button explicitly to prepare pasting the received files.

The GUI must stay running (it can be hidden in the tray). Clipboard sharing of text,
HTML and images continues to work. Files are copied, never cut/moved. Simultaneous
sending and receiving are supported, with one transfer in each direction. A new
clipboard copy while sending queues the latest selection, provided the clipboard
still contains that selection when the current transfer completes.

## Configuration and limits

- TCP port **24801**, configurable in the transfer window. Changing it requires a
  corresponding firewall rule. The Windows MSI installs a rule for this port and
  GUI executable on private networks, limited to the local subnet. Portable users
  must allow the port themselves. macOS may ask to allow incoming connections.
- Default transfer limit: **1 GiB**, adjustable from 1 MiB to 1 TiB. Maximum 10,000
  entries, depth 64, bounded file-list messages and 64 KiB streaming chunks.
- Files are written under a private temporary directory, checked individually with
  SHA-256, then published together in a unique timestamped folder. Existing files
  are not overwritten. Disconnected/cancelled/invalid transfers are removed.
- Symbolic links, special/unreadable files, path traversal, case-insensitive name
  collisions and names unsupported by Windows are rejected. File permissions,
  resource forks, extended attributes and Finder aliases are not transferred.
- Received files take disk space until you remove them. This is a copied-file
  transfer feature, not a continuously mounted network drive or native cross-screen
  drag operation. A paste issued before transfer completion cannot materialize the
  pending remote files; wait for completion first.

## Pairing and protocol

Pairing codes contain the receiver's address, TLS certificate SHA-256 pin and a
random 256-bit capability. Keep them private. Requests authenticate the capability
inside TLS, after the sender verifies the receiver's certificate pin. No source
file data is sent before the receiver accepts. Receiving is disabled by default;
individual approval is the default. Automatic acceptance explicitly authorizes
anyone holding that computer's pairing code. Do not forward pairing codes to others.

Preferences and pairing secrets are in `file-sharing.json` under the per-user
application data directory (`QStandardPaths::AppLocalDataLocation`), saved atomically
with owner-only permissions on POSIX systems and the user profile ACL on Windows. **Reset my pairing code** invalidates old incoming pairing codes. Deleting this file
while Deskflow is stopped resets all sharing preferences and generates a new capability. Deskflow's **Reset all settings** also stops transfers and removes these preferences.
Regenerating Deskflow's TLS certificate invalidates old pairing codes; exchange them again.

Protocol version 1 uses TLS 1.2+, a 5-byte frame header (type + big-endian uint32
length), bounded JSON offers and binary chunk/checksum frames. Files are streamed
with bounded socket buffering. Unknown/out-of-order/oversized messages, failed
checksums and excessive sizes abort the transfer. Authentication/approval and idle
transfers time out after 120 seconds; incoming TLS handshakes after 10 seconds.

The keyboard/mouse clipboard protocol carries a file-copy marker (without local
paths or file contents). Updated Windows/macOS cores preserve the native file
clipboard prepared by the GUI rather than replacing it with text when switching
computers. Older cores ignore this marker and do not support file sharing.
