# Maliit Keyboard with Solarized Themes

This Docker container includes Maliit keyboard with custom Solarized color themes (both Dark and Light variants).

## What Changed

- **Removed:** squeekboard (incompatible with Weston)
- **Added:** Maliit keyboard framework with Solarized themes
- **Updated:** weston.ini to use Maliit as the input method
- **Modified:** Maliit Keyboard.qml to be 70% width and centered (not full-screen)
- **Modified:** Maliit device config to set keyboard height to 30% (more compact)

## Files Structure

```
weston/
├── Dockerfile                          # Updated with Maliit packages + sed modifications
├── weston.ini                          # Configured for Maliit
├── maliit-theme-dark/                  # Solarized Dark theme
│   ├── main.ini
│   └── extended-keys.ini
└── maliit-theme-light/                 # Solarized Light theme
    ├── main.ini
    └── extended-keys.ini
```

## Theme Locations in Container

Once built, themes will be installed to:
- Dark theme: `/home/centroid/.local/share/maliit/keyboard2/themes/solarized-dark/`
- Light theme: `/home/centroid/.local/share/maliit/keyboard2/themes/solarized-light/`

## Switching Between Themes

### Method 1: Using gsettings (Inside running container)

```bash
# Switch to Dark theme
gsettings set org.maliit.keyboard.maliit theme solarized-dark

# Switch to Light theme
gsettings set org.maliit.keyboard.maliit theme solarized-light

# View current theme
gsettings get org.maliit.keyboard.maliit theme
```

### Method 2: From Flutter Application

Use the Flutter code from the earlier research to programmatically switch themes:

```dart
import 'dart:io';

Future<void> setMaliitTheme(String theme) async {
  // theme should be 'solarized-dark' or 'solarized-light'
  final result = await Process.run(
    'gsettings',
    ['set', 'org.maliit.keyboard.maliit', 'theme', theme],
  );

  if (result.exitCode == 0) {
    print('Theme changed to: $theme');
  } else {
    print('Failed to change theme: ${result.stderr}');
  }
}

// Usage
await setMaliitTheme('solarized-dark');
```

## Solarized Color Palette Used

### Dark Theme
- **Background:** #002b36 (base03)
- **Keys:** #073642 (base02)
- **Text:** #839496 (base0)
- **Special Keys:** #268bd2 (blue)
- **Modifiers:** #b58900 (yellow)
- **Locked State:** #859900 (green)

### Light Theme
- **Background:** #fdf6e3 (base3)
- **Keys:** #eee8d5 (base2)
- **Text:** #657b83 (base00)
- **Special Keys:** #268bd2 (blue)
- **Modifiers:** #b58900 (yellow)
- **Locked State:** #859900 (green)

## Building the Container

```bash
cd /Users/jonb/Projects/dockers/weston
docker build -t weston-maliit .
```

## Running the Container

Make sure to:
1. Start D-Bus session inside the container
2. Set proper environment variables
3. Start Weston

Example startup command would include setting up D-Bus before launching Weston.

## Troubleshooting

### Keyboard doesn't appear
- Check that D-Bus is running: `echo $DBUS_SESSION_BUS_ADDRESS`
- Verify Maliit is installed: `which maliit-keyboard`
- Check Weston logs for input-method errors

### Theme doesn't apply
- Verify theme files exist in the container: `ls ~/.local/share/maliit/keyboard2/themes/`
- Check current theme setting: `gsettings get org.maliit.keyboard.maliit theme`
- Make sure the theme name matches the directory name exactly

### D-Bus not available
- Start D-Bus session: `dbus-launch --sh-syntax`
- Export the session bus address before starting Weston

## Keyboard Size Customization

### Width
The keyboard is modified during Docker build to be **70% of screen width** and **centered**.

To adjust the width percentage, edit the Dockerfile and change the `0.7` value in this line:
```dockerfile
RUN sed -i '/width: parent.width/c\        width: parent.width * 0.7' ...
```

To a different value (e.g., `0.6` for 60%, `0.8` for 80%), then rebuild the container.

### Height
The keyboard height is set to **30% of screen height** in landscape mode.

To adjust the height percentage, edit the Dockerfile and change the `0.3` value in this line:
```dockerfile
RUN sed -i 's/"keyboardHeightLandscape": 0.38/"keyboardHeightLandscape": 0.3/' ...
```

To a different value (e.g., `0.25` for 25%, `0.35` for 35%), then rebuild the container.

## Notes

- Maliit uses Qt/QML rendering, not GTK CSS
- The old gtk.css, gtk-solarized-dark.css, and gtk-solarized-light.css files are no longer used
- Themes are configured via INI files following Maliit's theme format
- Some INI properties may not be supported depending on Maliit version
- The keyboard width is controlled by patching the QML file at build time

## Compatibility

- **Weston:** Uses `zwp_input_method_v1` protocol (supported)
- **Maliit:** Version 2.3.0+ in Debian Trixie
- **Qt:** Requires Qt5 Wayland platform support
- **D-Bus:** Required for Maliit framework communication
