# FICture2

FD2D is a lightweight modern C++ UI scaffold that wraps Win32 with Direct2D/DirectWrite. The project ships with a minimal boilerplate so you can drop in custom elements quickly.

## Architecture

- `FD2D::Core` initializes shared factories (D2D, DWrite, WIC) and keeps the process lifetime tidy.
- `FD2D::Backplate` owns an `HWND` and its `ID2D1HwndRenderTarget`, driving render invalidation and resize.
- `FD2D::Wnd` is the base class for visual/input components (Text, Image, Button, Window, Scroller, ...). Wnds receive render and Win32 message callbacks.

## Typical Usage

1) Call `FD2D::Application::Instance().Initialize` once at startup.  
2) Create a backplate:
   - Either hand an existing `HWND` to `Backplate::Attach`, or
   - Ask FD2D to create one with `Application::CreateWindowedBackplate` using `WindowOptions`.  
3) Add `Wnd`-derived controls to the backplate (constructors accept names; use `SetName` only when renaming).  
4) Run the message loop via `Application::RunMessageLoop` (or your own loop if you attached to an external `HWND`).

### Minimal Sketch

```cpp
FD2D::InitContext ctx {};
ctx.instance = hInstance;
FD2D::Application::Instance().Initialize(ctx);

FD2D::Backplate backplate;
auto backplate = FD2D::Application::Instance().CreateWindowedBackplate(L"main", opts);
backplate->AddWnd(L"content", std::make_shared<MyWnd>());

ShowWindow(backplate->Window(), nCmdShow);
UpdateWindow(backplate->Window());
return FD2D::Application::Instance().RunMessageLoop();
```

## Layout Principles (Modern model)
- Measure/Arrange split: `Measure(available)` asks “how much do you need?”, `Arrange(finalRect)` places it. Render is side-effect-free and uses computed bounds.
- Containers: StackPanel (vertical/horizontal), Panel (placeholder), future Grid/Dock/Overlay fit the same pattern.
- Intrinsic size: elements report desired size from content (text, image aspect, padding).
- Spacing semantics: margin (outside), padding (inside), spacing (between children) kept distinct.
- Alignment & stretch are first-class (start/center/end/stretch) rather than manual coordinates.
- Dirty layout: window resize/content/DPI changes must invalidate layout, recompute Measure/Arrange, then Render.
- DPI-aware: coordinates are logical; rendering scales via Direct2D.
- Declarative mindset: describe relationships (stack, grid, flex/star) instead of manual positioning; internally resolved via Measure/Arrange.
### Window creation modes

- **Standard chrome**: `ChromeStyle::Standard` (title bar with min/max/close).  
- **Borderless**: `ChromeStyle::Borderless` (no chrome, ideal for custom-drawn windows).  
`WindowOptions` also exposes `style` and `exStyle` so you can override defaults later.

## Status

This is a skeleton only. Rendering and input pipelines are stubbed but ready for specialized controls.
