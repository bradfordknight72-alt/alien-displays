# Mini alien (kids' version)

151 x 145 x 27.5 mm head (the original is 171 x 177). Same electronics, firmware and app.

| File | Print notes |
|---|---|
| `mini_alien_front_shell.stl` | Face-down as exported. The eyes are a 0.4 mm skin of the head colour (2 layers at 0.2 mm), so the front is one flat piece with no lens. A dark or matte head colour hides the unlit dots best |
| `mini_alien_back_lid.stl` | Flip so the flat back is on the bed. Holds the USB-C opening only, nothing sticks out |
| `mini_alien_stand.stl` | Base-down, 4 walls. The 20 x 6.2 mm post slides 25 mm up into the slot in the chin, toward the back |
| `mini_alien_display.sc` / `.step` | SolidCraft design / STEP |

Assembly:
- **LED board**: LEDs facing the face, resting on the inner face around the eye pocket (2 mm air gap behind the skin).
  **Bend the 5 input header pins 90 degrees toward the back first**: there is no room for them to stick out sideways.
- **ESP32-C3 SuperMini**: stands on end in the holder by the lower-left screw post, USB-C side facing up and its
  port end toward the lid. The holder leaves the antenna (red C3 chip) end clear.
- **Lid**: 4 M3 pan-head screws.
