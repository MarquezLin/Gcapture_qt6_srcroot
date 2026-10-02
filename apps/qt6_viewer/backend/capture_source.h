#pragma once

#include "capture_backend.h"

class CaptureSource
{
public:
    virtual ~CaptureSource() = default;

    virtual CaptureBackend backend() const = 0;
    virtual bool isOpen() const = 0;
    virtual bool isRunning() const = 0;
    virtual void stop() = 0;
    virtual void close() = 0;
};
