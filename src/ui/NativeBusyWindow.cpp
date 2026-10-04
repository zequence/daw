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
        auto* handle = CreateWindowExW (WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
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
                    SetWindowPos (handle, HWND_TOPMOST, r.left, r.top, width, height,
                                  SWP_NOACTIVATE | SWP_SHOWWINDOW);
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

void NativeBusyWindow::show (juce::Rectangle<int> screenArea, float scale)
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

#else   // other platforms: not available; BusyOverlay draws the card itself

struct NativeBusyWindow::Impl {};

NativeBusyWindow::NativeBusyWindow() {}
NativeBusyWindow::~NativeBusyWindow() = default;
bool NativeBusyWindow::isAvailable() const noexcept                       { return false; }
void NativeBusyWindow::show (juce::Rectangle<int>, float)                 {}
void NativeBusyWindow::update (const juce::String&, const juce::String&, double) {}
void NativeBusyWindow::hide()                                             {}

#endif
