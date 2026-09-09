# Toast UI Contract

- Business callers use `ThemedUi::ShowToast` and semantic options only.
- `ThemedUi::MeasureToast` returns device-pixel window, text and close-button geometry.
  Painting and hit testing consume that same result.
- `maxWidth` remains a logical-pixel text-width limit, excluding padding and the
  close button. `toast.closeSize` is 16 and `toast.closeGap` is 6 by default.
  Text is measured again at its final width to account for fractional-DPI rounding.
- DirectWrite is preferred; GDI fallback must measure with the same wrapping
  flags it uses when painting, not an empty DirectWrite result.
- Translucent role backgrounds are composed over the normal toast surface.
  Status badges compose over their public parent surface. `Color::Over` retains
  alpha until a known surface is supplied; other D2D overlay tokens stay unchanged.
- Owner anchors track owner position and size through a public-layer subclass,
  including hosts that do not forward common messages. Hiding or minimizing the
  owner dismisses those toasts; restoring it does not revive stale messages.
  Screen anchors do not follow ordinary owner movement.
- Lifetime timers belong to the toast HWND. Destruction removes the subclass,
  timer and owned window. Toasts never activate the owner.
- Acceptance covers text fit, close hit testing, movement, resize, hiding,
  replacement, expiration, teardown and role pixels at 96/120/144 DPI through
  both Direct2D and forced GDI fallback. Captures are background HWND-only.
