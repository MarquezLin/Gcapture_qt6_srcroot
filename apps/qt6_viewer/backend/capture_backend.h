#pragma once

enum class CaptureBackend
{
    DirectShow,
    Gvfg
};

constexpr int captureBackendUiValue(CaptureBackend backend)
{
    return static_cast<int>(backend);
}

constexpr CaptureBackend captureBackendFromUiValue(int value)
{
    return value == captureBackendUiValue(CaptureBackend::Gvfg)
               ? CaptureBackend::Gvfg
               : CaptureBackend::DirectShow;
}
