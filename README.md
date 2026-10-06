# iPod Uploader


A small GTK4/libadwaita app that syncs the songs under `~/Music`
(recursively) to a connected iPod using
[libgpod](https://github.com/fadingred/libgpod), and lets you remove songs
from it again.

## Slop warning!

This app was completely vibecoded. I don't care, I just want something that 
uploaded my music to my newly purchased iPod classic with minimal fuss and
that wasn't also an audio app. 

The rest of this readme is also generated.

## What it does

- Finds a mounted iPod by looking for an `iPod_Control` directory on
  mounted volumes (via GIO, with a `/run/media` and `/media` fallback).
- Scans `$XDG_MUSIC_DIR` (or `~/Music`) for `mp3`, `m4a`, `m4b`, `aac`,
  `wav`, `aif` and `aiff` files and reads their tags with TagLib.
- **Upload page**: the library as a collapsible artist → album → song tree.
  Every song shows whether it is new, already on the iPod, or on
  the iPod with a wrong length. New and wrong-length songs are ticked by
  default; tick any "already on iPod" song to re-upload it (the old copy is
  replaced, keeping its rating and play count). While uploading, every
  song, album and artist row shows its own progress bar.
- **Manage page**: the songs on the iPod as a collapsible artist → album →
  song tree. Tick songs, albums or whole artists and press *Remove* to
  delete them from the iPod (after a confirmation).
- A dropdown next to each search field chooses how the list is grouped:
  Artist › Album, Artists, Albums (each album row names its artist) or a
  flat Songs list.
  The choice is remembered per page in `~/.config/ipod-uploader.conf`.
  Compilations (COMPILATION tag, or an album whose tracks have different
  artists and no album artist) are grouped under "Various Artists".
- A fuzzy search field above each list filters it as you type
  (`nujabs feathr` finds *Nujabes – Feather*; matching artists and albums
  show all their songs).
- Errors are shown in a dialog with a selectable, copyable details box.
  Successful runs just show a toast.
- The eject button in the header bar unmounts the iPod so it can be
  unplugged.
- Songs already on the iPod are matched by artist/album/title/track
  number/file size, so re-running is safe.
- Cancelling or closing the window mid-job finishes the current song and
  still writes the database, so nothing copied is lost.

## Exact MP3 lengths

TagLib estimates the length of an MP3 from the first frame's bitrate when
the file has no Xing/VBRI header, which is wildly wrong for VBR files
(a 3-minute song can show up as 22 minutes on the iPod). The app counts
the MPEG frames itself (`src/mp3.c`) and uses that length, and it flags
songs whose length on the iPod differs from the real one so they can be
re-uploaded with one click.

## Device information (iPod Classic / nano)

Newer iPods (Classic, nano 3G and later) only accept an iTunesDB that
carries a checksum derived from the device's FireWire ID. libgpod reads that
ID from `iPod_Control/Device/SysInfoExtended`, which a freshly restored iPod
does not have. When the file is missing the app shows a banner with a
"Read device info" button that runs

    pkexec ipod-read-sysinfo-extended /dev/sdX /path/to/mountpoint

(the tool ships with libgpod and needs root to query the USB device).
You only have to do this once per iPod.

## Building

Dependencies (Fedora package names): `gtk4-devel`, `libadwaita-devel`,
`libgpod-devel`, `taglib-devel`.

    make
    ./ipod-uploader
    sudo make install      # optional, installs to /usr/local

## Layout

- `src/library.c` – recursive scan of the music folder, cached tag reading
- `src/mp3.c` – exact MP3 duration by counting frames
- `src/fuzzy.c` – the fuzzy matcher used by the search fields
- `src/node.c` – artist/album/song tree nodes shown in the lists
- `src/ipod.c` – iPod discovery, track listing, upload/replace and remove jobs
- `src/main.c` – the libadwaita window
