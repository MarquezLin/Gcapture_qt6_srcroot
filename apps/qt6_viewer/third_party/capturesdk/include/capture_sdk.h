#ifndef CAPTURE_SDK_H
#define CAPTURE_SDK_H

#pragma once
#include <stdint.h>

/**
 * @file capture_sdk.h
 * @brief Public C API for CaptureSDK.dll (or a static/shared library on non-Windows).
 *
 * This header is intended to be shipped to application developers who integrate with
 * the Capture SDK. The API is C-compatible and can be used from C/C++ and other
 * languages via FFI.
 *
 * Notes:
 * - All functions return a cap_result_t error code (CAP_OK on success).
 * - A cap_handle_t is an opaque handle returned by cap_create().
 * - The SDK delivers video frames through a user-provided callback (cap_video_cb_t).
 */

#ifdef _WIN32
/**
 * @brief DLL import/export macro.
 *
 * Define CAPTURESDK_LIBRARY when building the DLL itself to export symbols.
 * Consumers should NOT define CAPTURESDK_LIBRARY so the symbols are imported.
 */
#ifdef CAPTURESDK_LIBRARY
#define CAPSDK_API __declspec(dllexport)
#else
#define CAPSDK_API __declspec(dllimport)
#endif
#else
#define CAPSDK_API
#endif

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @brief Opaque SDK handle type.
   *
   * The actual implementation is private to the SDK. You must obtain a handle
   * via cap_create() and release it via cap_destroy().
   */
  typedef void *cap_handle_t;

  /**
   * @brief Common result/error codes returned by SDK APIs.
   */
  typedef enum
  {
    /** Success. */
    CAP_OK = 0,

    /** Invalid argument or invalid state for the requested operation. */
    CAP_E_INVALID = -1,

    /** Capture device is not found / not present. */
    CAP_E_DEVICE_NOT_FOUND = -2,

    /** Failed to open the capture device. */
    CAP_E_OPEN_DEVICE = -3,

    /** Unspecified internal error. */
    CAP_E_INTERNAL = -100
  } cap_result_t;

  /**
   * @brief Capture mode selection.
   */
  typedef enum
  {
    /**
     * Capture exactly one frame (the callback is expected to be invoked once).
     */
    CAP_MODE_SINGLE = 0,

    /**
     * Capture continuously (the callback is expected to be invoked repeatedly)
     * until cap_stop_capture() is called.
     */
    CAP_MODE_CONTINUOUS = 1
  } cap_mode_t;

  /**
   * @brief Video frame callback.
   *
   * The SDK calls this function when a video frame is available.
   *
   * @param buf             Pointer to the beginning of the frame buffer (read-only).
   * @param width           Frame width in pixels.
   * @param height          Frame height in pixels.
   * @param bytes_per_pixel Bytes per pixel (e.g., 3 for RGB24, 4 for RGBA32).
   *                        The exact pixel format depends on the SDK implementation.
   * @param user            User context pointer passed to cap_start_capture().
   *
   * @note The memory pointed to by @p buf is owned by the SDK unless otherwise
   *       documented by the SDK implementation. If you need to keep the frame,
   *       copy the data within the callback.
   */
  typedef void (*cap_video_cb_t)(
      const uint8_t *buf,
      int width,
      int height,
      int bytes_per_pixel,
      void *user);

  /**
   * @brief Create a new SDK instance.
   *
   * @param out Receives the created handle on success.
   * @return CAP_OK on success; otherwise an error code.
   */
  CAPSDK_API cap_result_t cap_create(cap_handle_t *out);

  /**
   * @brief Destroy an SDK instance and release all resources.
   *
   * @param h Handle created by cap_create().
   * @return CAP_OK on success; otherwise an error code.
   *
   * @note After calling this, the handle is no longer valid.
   */
  CAPSDK_API cap_result_t cap_destroy(cap_handle_t h);

  /**
   * @brief Initialize the capture pipeline with the desired output frame size.
   *
   * @param h      Handle created by cap_create().
   * @param width  Desired output width in pixels.
   * @param height Desired output height in pixels.
   * @return CAP_OK on success; otherwise an error code.
   *
   * @note Call cap_uninit() when you no longer need capturing, or before re-init.
   */
  CAPSDK_API cap_result_t cap_init(cap_handle_t h, int width, int height);

  /**
   * @brief Uninitialize the capture pipeline and release device-related resources.
   *
   * @param h Handle created by cap_create().
   * @return CAP_OK on success; otherwise an error code.
   *
   * @note If capture is running, stop it first via cap_stop_capture().
   */
  CAPSDK_API cap_result_t cap_uninit(cap_handle_t h);

  /**
   * @brief Start capturing frames and delivering them through the callback.
   *
   * @param h    Handle created by cap_create().
   * @param mode Capture mode (single-frame or continuous).
   * @param cb   Callback to receive video frames.
   * @param user User pointer forwarded to the callback.
   * @return CAP_OK on success; otherwise an error code.
   *
   * @note In CAP_MODE_CONTINUOUS, the callback may be invoked from an internal
   *       worker thread (implementation-dependent). Keep the callback lightweight.
   */
  CAPSDK_API cap_result_t cap_start_capture(cap_handle_t h, cap_mode_t mode, cap_video_cb_t cb, void *user);

  /**
   * @brief Stop capturing.
   *
   * @param h Handle created by cap_create().
   * @return CAP_OK on success; otherwise an error code.
   *
   * @note After stopping, you may start again with cap_start_capture() (depending
   *       on the SDK implementation) or uninitialize via cap_uninit().
   */
  CAPSDK_API cap_result_t cap_stop_capture(cap_handle_t h);

#ifdef __cplusplus
}
#endif

#endif // CAPTURE_SDK_H
