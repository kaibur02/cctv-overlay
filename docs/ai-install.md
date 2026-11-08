# Installing with an AI assistant

This page is written **for an AI coding assistant** (Claude Code, Codex, opencode, …) that a user has asked to
install cctv-overlay. A person can follow it too. Hand your assistant this sentence:

> Install cctv-overlay from this repository by following `docs/ai-install.md` exactly.

---

## Instructions for the assistant

You install everything except the password. **Never ask the user for their camera/recorder password in chat,
never put it in a file, command line or environment variable you create, and never read
`~/.config/cctv-overlay/secrets.ini`.** The user types it into the installer's hidden prompt themselves (step 4).

### 1. Check the machine

```sh
./install.sh --check
```

- Exit 0: continue.
- Missing packages are listed with the exact `sudo pacman -S --needed ...` command. Ask the user before running
  it (it needs their sudo password, which they type into the terminal, not into chat). On non-Arch systems,
  install the equivalents listed in [`requirements.md`](requirements.md).
- If the session is not Wayland, or the compositor is GNOME, stop: the overlay cannot work there.

### 2. Ask the user these questions (in chat)

Ask them together, offer the defaults, and accept "default" for anything they don't care about.

1. **Camera system**: one of `dahua` (also Amcrest, Lorex), `hikvision` (also HiLook, Annke), `reolink`,
   `uniview`, `standalone` (separate IP cameras, no recorder) or `custom` (they know the RTSP path).
2. Recorder **IP address** (not for `standalone`) and RTSP **port** (default 554).
3. Recorder **user name** (not the password). Suggest a view-only user.
4. **How many cameras**, and for each one: a **name** to show on screen and its recorder **channel number**
   (default: 1, 2, 3, …). For `standalone`: each camera's IP address and make
   (`dahua`, `hikvision`, `reolink`, `tapo`, `uniview`, or `custom` with a path).
5. **Stream**: `main` (sharper; on many NVRs it also appears faster) or `sub` (lighter). Default `main`.
6. **Monitor** for the overlay. Get the choices with `hyprctl monitors -j | jq -r '.[].name'`; default is the
   first one. `focused` follows the active monitor.
7. **Size**: `480x270`, `600x338`, `720x405` (default) or `960x540`, and **corner**: `top-right` (default),
   `top-left`, `bottom-right`, `bottom-left`.
8. **Keys** for: show/hide the overlay, next camera, previous camera, audio on/off. Defaults:
   `CTRL + KP_Multiply`, `KP_Multiply`, `KP_Divide`, `CTRL + KP_Subtract` (numpad `*`, `/`, `-`). Hyprland
   syntax is `MODIFIERS + KEY`. Mention that a key without a modifier stops working in other apps. `none`
   skips a binding.

If a configuration already exists (`~/.config/cctv-overlay/config.ini`), ask whether to **keep** it (default)
or replace it (the installer backs it up). Keeping it skips questions 1–7.

### 3. Write the answers file and run the installer

Copy [`examples/install.answers.example`](../examples/install.answers.example) to a temporary file, fill it in
with the user's answers (it must not contain a password; the installer refuses one), then run:

```sh
./install.sh --answers /path/to/answers.txt --yes
```

Without a terminal, the installer uses the answers file and the defaults and never prompts. It:
builds, installs to `~/.local/bin`, writes `~/.config/cctv-overlay/config.ini` (mode 0600), validates it,
adds a marked block to the Hyprland bindings file (after a timestamped backup; it restores the backup by itself
if `hyprctl configerrors` reports a new error) and reports what to do about the password.

Show the user the installer's final summary. On an error, read the message: it names the field to fix.

### 4. The user sets the password themselves

Tell the user to run this **in their own terminal**. In Claude Code they can type it at the prompt with the `!`
prefix, which runs it in the session's terminal without the password ever reaching the model:

```sh
! ./install.sh --set-password
```

It asks twice with hidden input and writes `secrets.ini` with mode 0600. Do not run it for them, and do not try
to supply input to it.

### 5. Test

```sh
./install.sh --test
```

It opens the overlay on the 4-camera grid, waits for the cameras, prints one line per camera
(`✓ Front: live` or `✗ Garage: error`) and closes the overlay. On failure, `cctv-overlay status` and
`~/.local/state/cctv-overlay/daemon.log` contain the reason; they never contain the password.
Common fixes: wrong channel numbers, RTSP disabled on the recorder, wrong user name or password
(`./install.sh --set-password` again), or a firewall.

### 6. Tell the user how to use it

- The **show/hide** key opens the overlay on the grid of the first four cameras.
- **Next/previous** step through the grid, then each camera on its own, then back to the grid.
- The **audio** key turns camera sound on and off. A small speaker icon appears in the corner while it is on.
  The grid view never plays sound.
- Change settings by running `./install.sh` again, or by editing `config.ini` and running `cctv-overlay reload`.
- Remove everything with `./install.sh --uninstall` (it asks before deleting the configuration).
