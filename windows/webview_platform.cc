#include "webview_platform.h"

#include <DispatcherQueue.h>
#include <shlobj.h>
#include <windows.graphics.capture.h>

#include <filesystem>
#include <iostream>

WebviewPlatform::WebviewPlatform()
    : rohelper_(std::make_unique<rx::RoHelper>(RO_INIT_SINGLETHREADED)) {
  if (rohelper_->WinRtAvailable()) {
      // Try STA first
      DispatcherQueueOptions optionsSta{
              sizeof(DispatcherQueueOptions),
              DQTYPE_THREAD_CURRENT,
              DQTAT_COM_STA};

      HRESULT hr = rohelper_->CreateDispatcherQueueController(
              optionsSta, dispatcher_queue_controller_.put());

      if (FAILED(hr)) {
          // Fallback: try COM_NONE
          DispatcherQueueOptions optionsNone{
                  sizeof(DispatcherQueueOptions),
                  DQTYPE_THREAD_CURRENT,
                  DQTAT_COM_NONE};

          hr = rohelper_->CreateDispatcherQueueController(
                  optionsNone, dispatcher_queue_controller_.put());
      }

      if (FAILED(hr)) {
          std::wcerr << L"CreateDispatcherQueueController failed. HRESULT="
                     << std::hex << hr << std::endl;
          return;
      }

    if (!IsGraphicsCaptureSessionSupported()) {
      std::cerr << "Windows::Graphics::Capture::GraphicsCaptureSession is not "
                   "supported."
                << std::endl;
      return;
    }

    graphics_context_ = std::make_unique<GraphicsContext>(rohelper_.get());
    valid_ = graphics_context_->IsValid();
  }
}

bool WebviewPlatform::IsGraphicsCaptureSessionSupported() {
  HSTRING className;
  HSTRING_HEADER classNameHeader;

  if (FAILED(rohelper_->GetStringReference(
          RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureSession,
          &className, &classNameHeader))) {
    return false;
  }

  ABI::Windows::Graphics::Capture::IGraphicsCaptureSessionStatics*
      capture_session_statics;
  if (FAILED(rohelper_->GetActivationFactory(
          className,
          __uuidof(
              ABI::Windows::Graphics::Capture::IGraphicsCaptureSessionStatics),
          (void**)&capture_session_statics))) {
    return false;
  }

  boolean is_supported = false;
  if (FAILED(capture_session_statics->IsSupported(&is_supported))) {
    return false;
  }

  return !!is_supported;
}

std::optional<std::wstring> WebviewPlatform::GetDefaultDataDirectory() {
  PWSTR path_tmp;
  if (!SUCCEEDED(
          SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path_tmp))) {
    return std::nullopt;
  }
  auto path = std::filesystem::path(path_tmp);
  CoTaskMemFree(path_tmp);

  wchar_t filename[MAX_PATH];
  GetModuleFileName(nullptr, filename, MAX_PATH);
  path /= "flutter_webview_windows";
  path /= std::filesystem::path(filename).stem();

  return path.wstring();
}
