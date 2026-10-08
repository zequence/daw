#include "NativeBusyWindow.h"

#if JUCE_WINDOWS

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>

namespace
{
    constexpr UINT msgShow = WM_APP + 1, msgHide = WM_APP + 2, msgQuit = WM_APP + 3;
    constexpr UINT_PTR frameTimerId = 1;
    const wchar_t* const windowClassName = L"OrchestralDAWBusyCard";

    COLORREF rgb (juce::uint32 argb)
    {
        return RGB ((argb >> 16) & 0xff, (argb >> 8) & 0xff, argb & 0xff);
    }
}

struct NativeBusyWindow::Impl
{
    std::thread thread;
    std::atomic<HWND> hwnd { nullptr };
    std::atomic<bool> ready { false }, failed { false };
    std::atomic<HWND> appWindow { nullptr };   // the card sits directly above this window

    // Shared state (written by the message thread, read by the card thread)
    std::mutex lock;
    std::wstring title, detail;
    double progress = -1.0;
    RECT cardRect {};
    float scale = 1.0f;

    // Smoothness measurement (card thread only)
    int frames = 0;
    ULONGLONG shownAt = 0, lastFrame = 0, longestGap = 0;

    Impl()
    {
        thread = std::thread ([this] { run(); });

        // The window is created on the card thread; wait (briefly) for it
        for (int i = 0; i < 400 && ! ready.load() && ! failed.load(); ++i)
            Sleep (5);
    }

    ~Impl()
    {
        if (auto handle = hwnd.load())
            PostMessageW (handle, msgQuit, 0, 0);

        if (thread.joinable())
            thread.join();
    }

    // Keeps the card centred on the app window and directly above it in the
    // Z-order (not topmost over everything: other programs must be able to cover
    // both). Follows the app when it is moved, resized or dragged to a display
    // with another scale. The app can rise above us whenever it is activated, so
    // this runs on every frame. Card thread only.
    void placeAboveApp (HWND handle)
    {
        const auto app = appWindow.load();

        if (app == nullptr || ! IsWindow (app))
            return;

        if (IsIconic (app) || ! IsWindowVisible (app))
        {
            if (IsWindowVisible (handle))
                ShowWindow (handle, SW_HIDE);

            return;
        }

        if (! IsWindowVisible (handle))
            ShowWindow (handle, SW_SHOWNOACTIVATE);

        // Target rectangle: the app's client area (what the UI draws in), centred
        RECT client;
        POINT origin { 0, 0 };
        GetClientRect (app, &client);
        ClientToScreen (app, &origin);

        const auto dpi = GetDpiForWindow (app);
        const auto s = dpi > 0 ? (float) dpi / 96.0f : scale;
        const auto clientWidth = (int) (client.right - client.left), clientHeight = (int) (client.bottom - client.top);
        const auto width = (int) std::lround (juce::jmin (480.0f * s, (float) clientWidth - 32.0f * s));
        const auto height = (int) std::lround (130.0f * s);
        const auto x = (int) origin.x + (clientWidth - width) / 2;
        const auto y = (int) origin.y + (clientHeight - height) / 2;

        RECT current;
        GetWindowRect (handle, &current);
        const auto above = GetWindow (app, GW_HWNDPREV);
        const auto moved = current.left != x || current.top != y;
        const auto resized = current.right - current.left != width || current.bottom - current.top != height;

        if (resized)
        {
            {
                std::lock_guard<std::mutex> guard (lock);
                scale = s;
            }

            const auto corner = (int) std::lround (16.0f * s);
            SetWindowRgn (handle, CreateRoundRectRgn (0, 0, width + 1, height + 1, corner, corner), FALSE);
        }

        if (moved || resized || above != handle)
            SetWindowPos (handle, above != nullptr ? above : HWND_TOP, x, y, width, height, SWP_NOACTIVATE);
    }

    //==========================================================================
    // The card thread: its own window and message loop
    void run()
    {
        WNDCLASSEXW wc {};
        wc.cbSize = sizeof (wc);
        wc.lpfnWndProc = &Impl::wndProc;
        wc.hInstance = GetModuleHandleW (nullptr);
        wc.lpszClassName = windowClassName;
        wc.hCursor = LoadCursor (nullptr, IDC_WAIT);
        RegisterClassExW (&wc);   // failing because it exists already is fine

        // NOT owned by the app window (that would couple the threads' input queues)
        auto* handle = CreateWindowExW (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                        windowClassName, L"", WS_POPUP,
                                        0, 0, 10, 10, nullptr, nullptr, wc.hInstance, nullptr);

        if (handle == nullptr)
        {
            failed = true;
            return;
        }

        SetWindowLongPtrW (handle, GWLP_USERDATA, (LONG_PTR) this);
        hwnd = handle;
        ready = true;

        MSG message;

        while (GetMessageW (&message, nullptr, 0, 0) > 0)
        {
            if (message.message == msgQuit)
                break;

            TranslateMessage (&message);
            DispatchMessageW (&message);
        }

        DestroyWindow (handle);
        hwnd = nullptr;
    }

    static LRESULT CALLBACK wndProc (HWND handle, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<Impl*> (GetWindowLongPtrW (handle, GWLP_USERDATA));

        switch (message)
        {
            case msgShow:
                if (self != nullptr)
                {
                    RECT r;
                    {
                        std::lock_guard<std::mutex> guard (self->lock);
                        r = self->cardRect;
                    }

                    const auto width = r.right - r.left, height = r.bottom - r.top;
                    const auto corner = (int) std::lround (16.0f * self->scale);
                    SetWindowRgn (handle, CreateRoundRectRgn (0, 0, width + 1, height + 1, corner, corner), FALSE);
                    SetWindowPos (handle, nullptr, r.left, r.top, width, height,
                                  SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                    self->placeAboveApp (handle);
                    SetTimer (handle, frameTimerId, 16, nullptr);   // ~60 fps
                    self->frames = 0;
                    self->longestGap = 0;
                    self->shownAt = self->lastFrame = GetTickCount64();
                    InvalidateRect (handle, nullptr, FALSE);
                }
                return 0;

            case msgHide:
                KillTimer (handle, frameTimerId);
                ShowWindow (handle, SW_HIDE);

                if (self != nullptr && self->frames > 0)
                {
                    const auto seconds = (GetTickCount64() - self->shownAt) * 0.001;
                    juce::Logger::writeToLog ("Native busy card: " + juce::String (self->frames) + " frames in "
                                              + juce::String (seconds, 1) + " s ("
                                              + juce::String (self->frames / juce::jmax (0.001, seconds), 0)
                                              + " fps), longest gap " + juce::String ((int) self->longestGap) + " ms");
                }

                return 0;

            case WM_TIMER:
                if (self != nullptr)
                    self->placeAboveApp (handle);

                InvalidateRect (handle, nullptr, FALSE);
                return 0;

            case WM_ERASEBKGND:
                return 1;

            case WM_MOUSEACTIVATE:
                return MA_NOACTIVATE;

            case WM_PAINT:
                if (self != nullptr)
                    self->paint (handle);
                else
                    ValidateRect (handle, nullptr);
                return 0;

            default:
                break;
        }

        return DefWindowProcW (handle, message, wParam, lParam);
    }

    //==========================================================================
    void paint (HWND handle)
    {
        const auto now = GetTickCount64();
        longestGap = juce::jmax (longestGap, now - lastFrame);
        lastFrame = now;
        ++frames;

        PAINTSTRUCT ps;
        auto* dc = BeginPaint (handle, &ps);

        RECT client;
        GetClientRect (handle, &client);
        const auto w = client.right, h = client.bottom;

        // Double buffered
        auto* mem = CreateCompatibleDC (dc);
        auto* bitmap = CreateCompatibleBitmap (dc, w, h);
        auto* oldBitmap = SelectObject (mem, bitmap);

        std::wstring titleText, detailText;
        double progressValue;
        float s;
        {
            std::lock_guard<std::mutex> guard (lock);
            titleText = title;
            detailText = detail;
            progressValue = progress;
            s = scale;
        }

        const auto px = [s] (float v) { return (int) std::lround (v * s); };
        const auto seconds = (double) GetTickCount64() * 0.001;

        // Card
        auto* background = CreateSolidBrush (rgb (0xff23262b));
        FillRect (mem, &client, background);
        DeleteObject (background);

        auto* borderPen = CreatePen (PS_SOLID, 1, rgb (0xff43464d));
        auto* oldPen = SelectObject (mem, borderPen);
        auto* oldBrush = SelectObject (mem, GetStockObject (NULL_BRUSH));
        RoundRect (mem, 0, 0, w, h, px (16.0f), px (16.0f));
        SelectObject (mem, oldBrush);
        SelectObject (mem, oldPen);
        DeleteObject (borderPen);

        const auto pad = px (22.0f);
        SetBkMode (mem, TRANSPARENT);

        // Title
        auto* titleFont = CreateFontW (-px (17.0f), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                       DEFAULT_PITCH, L"Segoe UI");
        auto* oldFont = SelectObject (mem, titleFont);
        SetTextColor (mem, RGB (255, 255, 255));
        RECT titleRect { pad, pad, w - pad - px (30.0f), pad + px (26.0f) };
        DrawTextW (mem, titleText.c_str(), -1, &titleRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

        // Detail
        auto* detailFont = CreateFontW (-px (13.0f), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                        DEFAULT_PITCH, L"Segoe UI");
        SelectObject (mem, detailFont);
        SetTextColor (mem, RGB (205, 205, 210));
        RECT detailRect { pad, pad + px (26.0f), w - pad, pad + px (50.0f) };
        DrawTextW (mem, detailText.c_str(), -1, &detailRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

        SelectObject (mem, oldFont);
        DeleteObject (titleFont);
        DeleteObject (detailFont);

        // Spinner: 10 dots around a circle, brightness rotating
        {
            const auto cx = (float) (w - pad - px (10.0f));
            const auto cy = (float) (pad + px (13.0f));
            const auto radius = 8.0f * s, dot = 1.9f * s;
            const auto head = std::fmod (seconds * 1.4, 1.0) * 10.0;

            for (int i = 0; i < 10; ++i)
            {
                auto age = std::fmod (head - i + 10.0, 10.0) / 10.0;   // 0 = brightest
                const auto level = (int) (70 + (1.0 - age) * 150);
                auto* brush = CreateSolidBrush (RGB (level * 70 / 220, level * 130 / 220, level * 180 / 220));
                auto* prevBrush = SelectObject (mem, brush);
                auto* prevPen = SelectObject (mem, GetStockObject (NULL_PEN));

                const auto angle = i * (2.0 * juce::MathConstants<double>::pi / 10.0);
                const auto x = cx + radius * (float) std::sin (angle);
                const auto y = cy - radius * (float) std::cos (angle);
                Ellipse (mem, (int) (x - dot), (int) (y - dot), (int) (x + dot) + 1, (int) (y + dot) + 1);

                SelectObject (mem, prevPen);
                SelectObject (mem, prevBrush);
                DeleteObject (brush);
            }
        }

        // Progress bar
        {
            const int top = pad + px (62.0f), height = px (8.0f);
            const int left = pad, right = (int) w - pad;
            const auto barWidth = right - left;
            const auto corner = height;

            auto* track = CreateSolidBrush (rgb (0xff15171a));
            auto* prevBrush = SelectObject (mem, track);
            auto* prevPen = SelectObject (mem, GetStockObject (NULL_PEN));
            RoundRect (mem, left, top, right + 1, top + height + 1, corner, corner);

            auto* fill = CreateSolidBrush (RGB (70, 130, 180));
            SelectObject (mem, fill);

            if (progressValue >= 0.0)
            {
                const auto filled = (int) std::lround (barWidth * juce::jlimit (0.0, 1.0, progressValue));
                RoundRect (mem, left, top, left + filled + 1, top + height + 1, corner, corner);

                // Travelling shimmer over the filled part
                auto* shimmer = CreateSolidBrush (RGB (130, 175, 215));
                SelectObject (mem, shimmer);
                const auto shimmerX = left + (int) (filled * std::fmod (seconds * 0.8, 1.0));
                const auto shimmerW = px (20.0f);
                const auto x0 = juce::jmax (left, shimmerX - shimmerW / 2);
                const auto x1 = juce::jmin (left + filled, shimmerX + shimmerW / 2);

                if (x1 > x0)
                    RoundRect (mem, x0, top, x1 + 1, top + height + 1, corner, corner);

                SelectObject (mem, fill);
                DeleteObject (shimmer);
            }
            else
            {
                const auto phase = 0.5 - 0.5 * std::cos (seconds * juce::MathConstants<double>::pi);
                const auto segment = barWidth / 4;
                const auto x0 = left + (int) ((barWidth - segment) * phase);
                RoundRect (mem, x0, top, x0 + segment + 1, top + height + 1, corner, corner);
            }

            SelectObject (mem, prevPen);
            SelectObject (mem, prevBrush);
            DeleteObject (fill);
            DeleteObject (track);
        }

        BitBlt (dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);

        SelectObject (mem, oldBitmap);
        DeleteObject (bitmap);
        DeleteDC (mem);
        EndPaint (handle, &ps);
    }
};

//==============================================================================
NativeBusyWindow::NativeBusyWindow() : impl (std::make_unique<Impl>()) {}
NativeBusyWindow::~NativeBusyWindow() = default;

bool NativeBusyWindow::isAvailable() const noexcept
{
    return impl != nullptr && impl->ready.load() && impl->hwnd.load() != nullptr;
}

void NativeBusyWindow::show (juce::Rectangle<int> screenArea, float scale, void* appWindow)
{
    if (! isAvailable())
        return;

    {
        std::lock_guard<std::mutex> guard (impl->lock);
        impl->scale = scale;

        const auto width = (int) std::lround (juce::jmin (480.0f * scale, (float) screenArea.getWidth() - 32.0f * scale));
        const auto height = (int) std::lround (130.0f * scale);
        const auto card = screenArea.withSizeKeepingCentre (width, height);
        impl->cardRect = { card.getX(), card.getY(), card.getRight(), card.getBottom() };
    }

    impl->appWindow = (HWND) appWindow;
    PostMessageW (impl->hwnd.load(), msgShow, 0, 0);
}

void NativeBusyWindow::update (const juce::String& title, const juce::String& detail, double progress)
{
    if (impl == nullptr)
        return;

    std::lock_guard<std::mutex> guard (impl->lock);
    impl->title = title.toWideCharPointer();
    impl->detail = detail.toWideCharPointer();
    impl->progress = progress;
}

void NativeBusyWindow::hide()
{
    if (isAvailable())
        PostMessageW (impl->hwnd.load(), msgHide, 0, 0);
}

#elif JUCE_LINUX

// Linux: an X11 window on its own connection to the X server and its own thread (the app's
// connection is JUCE's, used on the message thread only). The card is drawn with JUCE's software
// renderer into an image (no message thread needed) and put on the window ~60 times a second.
// Override-redirect (not managed by the window manager): it follows the app window itself, each
// frame, and hides while the app is minimised.
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>

struct NativeBusyWindow::Impl
{
    std::thread thread;
    std::atomic<bool> ready { false }, failed { false }, quit { false }, wantShown { false };
    std::atomic<unsigned long> appWindow { 0 };

    // Shared state (written by the message thread, read by the card thread)
    std::mutex lock;
    juce::String title, detail;
    double progress = -1.0;
    juce::Rectangle<int> cardRect;
    float scale = 1.0f;

    // Card thread only
    Display* display = nullptr;
    Window window = 0;
    bool shown = false;
    juce::Rectangle<int> placed, shaped;
    int frames = 0;
    double shownAt = 0.0, lastFrame = 0.0, longestGap = 0.0;

    Impl()
    {
        thread = std::thread ([this] { run(); });

        for (int i = 0; i < 400 && ! ready.load() && ! failed.load(); ++i)
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }

    ~Impl()
    {
        quit = true;

        if (thread.joinable())
            thread.join();
    }

    static double now()   { return juce::Time::getMillisecondCounterHiRes(); }

    void run()
    {
        display = XOpenDisplay (nullptr);

        if (display == nullptr)
        {
            failed = true;
            return;
        }

        const auto screen = DefaultScreen (display);
        XSetWindowAttributes attributes {};
        attributes.override_redirect = True;
        attributes.background_pixel = 0x23262b;
        attributes.border_pixel = 0;
        window = XCreateWindow (display, RootWindow (display, screen), 0, 0, 10, 10, 0, CopyFromParent, InputOutput,
                                CopyFromParent, CWOverrideRedirect | CWBackPixel | CWBorderPixel, &attributes);
        XStoreName (display, window, "Busy");
        XFlush (display);
        ready = true;

        while (! quit.load())
        {
            while (XPending (display) > 0)   // (nothing asked for; keep the queue empty)
            {
                XEvent event;
                XNextEvent (display, &event);
            }

            if (wantShown.load() != shown)
                wantShown.load() ? begin() : end();

            if (shown)
            {
                place();
                paint();
            }

            std::this_thread::sleep_for (std::chrono::milliseconds (16));
        }

        if (shown)
            end();

        XDestroyWindow (display, window);
        XCloseDisplay (display);
        display = nullptr;
    }

    void begin()
    {
        shown = true;
        placed = shaped = {};
        frames = 0;
        longestGap = 0.0;
        shownAt = lastFrame = now();
        place();
    }

    void end()
    {
        shown = false;
        XUnmapWindow (display, window);
        XFlush (display);

        if (frames > 0)
        {
            const auto seconds = (now() - shownAt) * 0.001;
            juce::Logger::writeToLog ("Native busy card: " + juce::String (frames) + " frames in " + juce::String (seconds, 1)
                                      + " s (" + juce::String (frames / juce::jmax (0.001, seconds), 0)
                                      + " fps), longest gap " + juce::String (juce::roundToInt (longestGap)) + " ms");
        }
    }

    // Centred on the app window (where it is now), else on the area given to show(); hidden while
    // the app window isn't viewable (minimised)
    void place()
    {
        juce::Rectangle<int> area;
        float s;
        {
            std::lock_guard<std::mutex> guard (lock);
            area = cardRect;
            s = scale;
        }

        if (const auto app = (Window) appWindow.load(); app != 0)
        {
            XWindowAttributes attributes {};

            if (XGetWindowAttributes (display, app, &attributes) != 0)
            {
                if (attributes.map_state != IsViewable)
                {
                    if (! placed.isEmpty())
                        XUnmapWindow (display, window);

                    placed = {};
                    return;
                }

                int x = 0, y = 0;
                Window child;
                XTranslateCoordinates (display, app, RootWindow (display, DefaultScreen (display)), 0, 0, &x, &y, &child);
                area = { x, y, attributes.width, attributes.height };
            }
        }

        const auto width = (int) std::lround (juce::jmin (480.0f * s, (float) area.getWidth() - 32.0f * s));
        const auto height = (int) std::lround (130.0f * s);
        const auto card = area.withSizeKeepingCentre (juce::jmax (40, width), juce::jmax (20, height));

        if (card != placed)
        {
            XMoveResizeWindow (display, window, card.getX(), card.getY(), (unsigned) card.getWidth(), (unsigned) card.getHeight());

            if (placed.isEmpty())
                XMapRaised (display, window);
            else
                XRaiseWindow (display, window);

            placed = card;
        }
        else
        {
            XRaiseWindow (display, window);   // over the app, which may have risen
        }

        if (card.getWidth() != shaped.getWidth() || card.getHeight() != shaped.getHeight())   // the rounded corners
        {
            const auto w = card.getWidth(), h = card.getHeight(), corner = juce::jmax (2, (int) std::lround (16.0f * s));
            auto mask = XCreatePixmap (display, window, (unsigned) w, (unsigned) h, 1);
            auto gc = XCreateGC (display, mask, 0, nullptr);
            XSetForeground (display, gc, 0);
            XFillRectangle (display, mask, gc, 0, 0, (unsigned) w, (unsigned) h);
            XSetForeground (display, gc, 1);
            XFillRectangle (display, mask, gc, corner / 2, 0, (unsigned) (w - corner), (unsigned) h);
            XFillRectangle (display, mask, gc, 0, corner / 2, (unsigned) w, (unsigned) (h - corner));

            for (auto [x, y] : { std::pair (0, 0), std::pair (w - corner, 0), std::pair (0, h - corner), std::pair (w - corner, h - corner) })
                XFillArc (display, mask, gc, x, y, (unsigned) corner, (unsigned) corner, 0, 360 * 64);

            XShapeCombineMask (display, window, ShapeBounding, 0, 0, mask, ShapeSet);
            XFreeGC (display, gc);
            XFreePixmap (display, mask);
            shaped = card;
        }
    }

    void paint()
    {
        if (placed.isEmpty())
            return;

        const auto t = now();
        longestGap = juce::jmax (longestGap, t - lastFrame);
        lastFrame = t;
        ++frames;

        juce::String titleText, detailText;
        double progressValue;
        float s;
        {
            std::lock_guard<std::mutex> guard (lock);
            titleText = title;
            detailText = detail;
            progressValue = progress;
            s = scale;
        }

        const auto w = placed.getWidth(), h = placed.getHeight();
        juce::Image image (juce::Image::ARGB, w, h, false, juce::SoftwareImageType());
        {
            juce::Graphics g (image);
            drawCard (g, (float) w, (float) h, s, titleText, detailText, progressValue, t * 0.001);
        }

        // To the window: 32-bit pixels (B, G, R, A in memory: the usual 24-bit TrueColor layout)
        const auto screen = DefaultScreen (display);

        if (DefaultDepth (display, screen) < 24)
            return;

        juce::Image::BitmapData pixels (image, juce::Image::BitmapData::readOnly);
        std::vector<char> data ((size_t) (w * h * 4));

        for (int y = 0; y < h; ++y)
            std::memcpy (data.data() + (size_t) (y * w * 4), pixels.getLinePointer (y), (size_t) (w * 4));

        auto* ximage = XCreateImage (display, DefaultVisual (display, screen), (unsigned) DefaultDepth (display, screen), ZPixmap,
                                     0, data.data(), (unsigned) w, (unsigned) h, 32, w * 4);

        if (ximage == nullptr)
            return;

        auto gc = DefaultGC (display, screen);
        XPutImage (display, window, gc, ximage, 0, 0, 0, 0, (unsigned) w, (unsigned) h);
        ximage->data = nullptr;   // (ours: the vector frees it)
        XDestroyImage (ximage);
        XFlush (display);
    }

    // The card as the Windows one draws it: title, detail, spinner, progress bar (or a sweeping segment)
    static void drawCard (juce::Graphics& g, float w, float h, float s, const juce::String& titleText,
                          const juce::String& detailText, double progressValue, double seconds)
    {
        const auto bounds = juce::Rectangle<float> (w, h);
        g.setColour (juce::Colour (0xff23262b));
        g.fillRect (bounds);
        g.setColour (juce::Colour (0xff43464d));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 8.0f * s, 1.0f);

        const auto pad = 22.0f * s;
        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (17.0f * s, juce::Font::bold));
        g.drawText (titleText, juce::Rectangle<float> (pad, pad, w - 2.0f * pad - 30.0f * s, 26.0f * s), juce::Justification::centredLeft, true);
        g.setColour (juce::Colour (0xffcdcdd2));
        g.setFont (juce::FontOptions (13.0f * s));
        g.drawText (detailText, juce::Rectangle<float> (pad, pad + 26.0f * s, w - 2.0f * pad, 24.0f * s), juce::Justification::centredLeft, true);

        {   // Spinner: 10 dots around a circle, the brightness going round
            const auto cx = w - pad - 10.0f * s, cy = pad + 13.0f * s, radius = 8.0f * s, dot = 1.9f * s;
            const auto head = std::fmod (seconds * 1.4, 1.0) * 10.0;

            for (int i = 0; i < 10; ++i)
            {
                const auto age = std::fmod (head - i + 10.0, 10.0) / 10.0;
                const auto level = (float) (70.0 + (1.0 - age) * 150.0) / 220.0f;
                g.setColour (juce::Colour::fromFloatRGBA (level * 70.0f / 255.0f, level * 130.0f / 255.0f, level * 180.0f / 255.0f, 1.0f));
                const auto angle = (float) i * juce::MathConstants<float>::twoPi / 10.0f;
                g.fillEllipse (juce::Rectangle<float> (2.0f * dot, 2.0f * dot).withCentre ({ cx + radius * std::sin (angle), cy - radius * std::cos (angle) }));
            }
        }

        const auto bar = juce::Rectangle<float> (pad, pad + 62.0f * s, w - 2.0f * pad, 8.0f * s);
        const auto corner = bar.getHeight() * 0.5f;
        g.setColour (juce::Colour (0xff15171a));
        g.fillRoundedRectangle (bar, corner);
        g.setColour (juce::Colour (0xff4682b4));

        if (progressValue >= 0.0)
        {
            const auto filled = bar.withWidth (bar.getWidth() * (float) juce::jlimit (0.0, 1.0, progressValue));
            g.fillRoundedRectangle (filled, corner);
            const auto shimmerX = filled.getX() + filled.getWidth() * (float) std::fmod (seconds * 0.8, 1.0);
            const auto shimmer = filled.getIntersection (juce::Rectangle<float> (20.0f * s, bar.getHeight()).withCentre ({ shimmerX, bar.getCentreY() }));

            if (! shimmer.isEmpty())
            {
                g.setColour (juce::Colour (0xff82afd7));
                g.fillRoundedRectangle (shimmer, corner);
            }
        }
        else
        {
            const auto phase = 0.5 - 0.5 * std::cos (seconds * juce::MathConstants<double>::pi);
            const auto segment = bar.getWidth() / 4.0f;
            g.fillRoundedRectangle (bar.withWidth (segment).withX (bar.getX() + (bar.getWidth() - segment) * (float) phase), corner);
        }
    }
};

//==============================================================================
NativeBusyWindow::NativeBusyWindow() : impl (std::make_unique<Impl>()) {}
NativeBusyWindow::~NativeBusyWindow() = default;

bool NativeBusyWindow::isAvailable() const noexcept
{
    return impl != nullptr && impl->ready.load() && ! impl->failed.load();
}

void NativeBusyWindow::show (juce::Rectangle<int> screenArea, float scale, void* appWindow)
{
    if (! isAvailable())
        return;

    {
        std::lock_guard<std::mutex> guard (impl->lock);
        impl->scale = scale;
        impl->cardRect = screenArea;
    }

    impl->appWindow = (unsigned long) (juce::pointer_sized_uint) appWindow;   // (JUCE's peer handle on Linux: the X Window id)
    impl->wantShown = true;
}

void NativeBusyWindow::update (const juce::String& title, const juce::String& detail, double progress)
{
    if (impl == nullptr)
        return;

    std::lock_guard<std::mutex> guard (impl->lock);
    impl->title = title;
    impl->detail = detail;
    impl->progress = progress;
}

void NativeBusyWindow::hide()
{
    if (impl != nullptr)
        impl->wantShown = false;
}

#else   // other platforms: not available; BusyOverlay draws the card itself

struct NativeBusyWindow::Impl {};

NativeBusyWindow::NativeBusyWindow() {}
NativeBusyWindow::~NativeBusyWindow() = default;
bool NativeBusyWindow::isAvailable() const noexcept                       { return false; }
void NativeBusyWindow::show (juce::Rectangle<int>, float, void*)       {}
void NativeBusyWindow::update (const juce::String&, const juce::String&, double) {}
void NativeBusyWindow::hide()                                             {}

#endif
