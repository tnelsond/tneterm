# tneterm

A small SDL3 terminal emulator that renders Khmer (and other complex scripts)
properly: text is shaped with HarfBuzz and drawn at its natural width instead
of being squeezed into monospace cells.

## Build

Needs SDL3, HarfBuzz, FreeType, fontconfig.

    make
    ./tneterm                      # runs $SHELL
    ./tneterm -s 14 -k "Khmer OS System" -e vim file.txt

Options: `-f` monospace font pattern (default `Terminus,monospace`; bitmap
fonts are drawn pixel-exact at their nearest native size), `-k` Khmer font
family (default `Noto Sans Khmer`), `-K` Khmer size relative to the main font
(default: automatic, so Khmer consonants sit between the main font's x-height
and cap height), `-s` size in points, `-l` extra pixels between lines (may be
negative, e.g. `-l -2` for tighter lines), `-e cmd args...`.

## How layout works ("elastic grid")

The terminal model is a normal cell grid using `wcwidth`, so cursor addressing
agrees with bash/readline, vim, etc. Rendering is where it differs:

- Runs of complex-script text (Khmer, Thai, Indic, Arabic, anything with
  combining marks) are shaped as a unit and drawn at natural width.
- A single space after shaped text keeps flowing like prose.
- Runs of 2+ blanks (column padding, tabs) absorb the slack, so the next field
  snaps back to its grid column when there's room. Box-drawing characters
  always snap, so TUI borders line up.
- The cursor covers whole grapheme clusters, e.g. all of `ស្តី`.
- Line height is measured from everyday Khmer stacks (above-vowels,
  subscripts, a vowel under a subscript as in ខ្ញុំ) so they don't collide
  between lines; rare stacked marks (ក៉ី, ញ៉ុំ) may overhang slightly rather than
  making every line taller. Box/block characters are drawn procedurally so
  they join up across lines.

## Rendering

Glyphs are packed into shared atlas textures and each frame is drawn with a
few batched `SDL_RenderGeometry` calls (backgrounds, then text, then
underlines/box drawing) instead of one texture per glyph. HarfBuzz results
are cached per run, so unchanged Khmer text isn't reshaped every frame.
Scrollback lines are stored without their trailing blank cells.

## Keys

- Ctrl+Shift+C / Ctrl+Shift+V: copy / paste clipboard
- Ctrl+= / Ctrl+- / Ctrl+0: zoom font in / out / reset (Ctrl+Shift+- still sends ^_)
- Select with the mouse (copies to PRIMARY); middle-click or Shift+Insert pastes it
- Shift+PageUp/PageDown, mouse wheel: scrollback (5000 lines)
- Shift+mouse bypasses application mouse reporting

## Supported

xterm-256color subset: cursor movement, scroll regions, insert/delete
line/char, alt screen (1049), bracketed paste, app cursor keys, SGR (bold,
faint, italic, underline, reverse, strike, 16/256/truecolor), DEC line
drawing charset, synchronized output (2026), mouse reporting (1000/1002/1003 + SGR 1006), DECSCUSR cursor
shapes, OSC 0/2 title, OSC 10/11 colour queries, IME preedit display.

Not implemented: reflow on resize, bidi reordering for RTL scripts, sixel,
ligatures for Latin text, config file.

## Testing aid

`TNETERM_DUMP=out.png TNETERM_DUMP_MS=1500 ./tneterm -e ...` saves a
screenshot after the given delay and exits.
