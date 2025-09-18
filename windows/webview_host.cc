#include "webview_host.h"

#include <wrl.h>
#include <future>
#include <iostream>
#include <filesystem>

#include "util/rohelper.h"
#include "webview_platform.h"  // for GetDefaultDataDirectory

using namespace Microsoft::WRL;

namespace {

// Normalize a browser exe path into a folder
    std::wstring NormalizeBrowserPath(const std::optional<std::wstring>& raw) {
        if (raw.has_value() && !raw->empty()) {
            std::filesystem::path p = *raw;
            if (std::filesystem::is_regular_file(p) &&
                p.filename().wstring() == L"msedgewebview2.exe") {
                return p.parent_path().wstring();
            }
            return p.wstring();
        }

        // If not supplied, check environment variable
        wchar_t buf[MAX_PATH];
        DWORD len = GetEnvironmentVariableW(L"WEBVIEW2_BROWSER_EXECUTABLE_FOLDER", buf, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            std::filesystem::path p(buf);
            if (std::filesystem::is_regular_file(p / L"msedgewebview2.exe")) {
                return p.wstring();
            }
        }
        return L"";  // empty means "let WebView2 pick default"
    }

    std::wstring NormalizeUserDataDir(
            const std::optional<std::wstring>& provided,
            WebviewPlatform* platform) {
        if (provided.has_value() && !provided->empty()) {
            return *provided;
        }
        auto fallback = platform->GetDefaultDataDirectory();
        return fallback.value_or(L"");
    }

}  // namespace

// static
std::unique_ptr<WebviewHost> WebviewHost::Create(
        WebviewPlatform* platform,
        std::optional<std::wstring> user_data_directory,
        std::optional<std::wstring> browser_exe_path,
        std::optional<std::string> arguments) {
    wil::com_ptr<CoreWebView2EnvironmentOptions> opts;
    if (arguments.has_value()) {
        opts = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
        std::wstring warguments(arguments->begin(), arguments->end());
        opts->put_AdditionalBrowserArguments(warguments.c_str());
    }

    std::wstring exePath = NormalizeBrowserPath(browser_exe_path);
    std::wstring dataDir = NormalizeUserDataDir(user_data_directory, platform);

    std::wcerr << L"[WebViewHost] Using browser folder: "
               << (exePath.empty() ? L"<default>" : exePath) << std::endl;
    std::wcerr << L"[WebViewHost] Using user data dir: "
               << (dataDir.empty() ? L"<default>" : dataDir) << std::endl;

    std::promise<HRESULT> result_promise;
    wil::com_ptr<ICoreWebView2Environment> env;

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
            exePath.empty() ? nullptr : exePath.c_str(),
            dataDir.empty() ? nullptr : dataDir.c_str(),
            opts.get(),
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                    [&promise = result_promise, &ptr = env](
                            HRESULT r, ICoreWebView2Environment* createdEnv) -> HRESULT {
                        std::wcerr << L"[WebViewHost] Completion handler: HRESULT=0x"
                                   << std::hex << r << std::dec << std::endl;
                        promise.set_value(r);
                        ptr.swap(createdEnv);
                        return S_OK;
                    })
                    .Get());

    if (FAILED(hr)) {
        std::wcerr << L"[WebViewHost] Initial call to CreateCoreWebView2EnvironmentWithOptions failed. HRESULT=0x"
                   << std::hex << hr << std::dec << std::endl;
        return {};
    }

    HRESULT result = result_promise.get_future().get();
    if ((SUCCEEDED(result) || result == RPC_E_CHANGED_MODE) && env) {
        auto webview_env3 = env.try_query<ICoreWebView2Environment3>();
        if (webview_env3) {
            return std::unique_ptr<WebviewHost>(
                    new WebviewHost(platform, std::move(webview_env3)));
        }
    }

    std::wcerr << L"[WebViewHost] Environment creation failed. HRESULT=0x"
               << std::hex << result << std::dec << std::endl;
    return {};
}

WebviewHost::WebviewHost(WebviewPlatform* platform,
                         wil::com_ptr<ICoreWebView2Environment3> webview_env)
        : webview_env_(webview_env) {
    compositor_ = platform->graphics_context()->CreateCompositor();
}

void WebviewHost::CreateWebview(HWND hwnd, bool offscreen_only,
                                bool owns_window,
                                WebviewCreationCallback callback) {
    CreateWebViewCompositionController(
            hwnd, [=, self = this](
                    wil::com_ptr<ICoreWebView2CompositionController> controller,
                    std::unique_ptr<WebviewCreationError> error) {
                if (controller) {
                    std::unique_ptr<Webview> webview(new Webview(
                            std::move(controller), self, hwnd, owns_window, offscreen_only));
                    callback(std::move(webview), nullptr);
                } else {
                    callback(nullptr, std::move(error));
                }
            });
}

void WebviewHost::CreateWebViewPointerInfo(PointerInfoCreationCallback callback) {
    ICoreWebView2PointerInfo* pointer;
    auto hr = webview_env_->CreateCoreWebView2PointerInfo(&pointer);

    if (FAILED(hr)) {
        callback(nullptr, WebviewCreationError::create(hr, "CreateWebViewPointerInfo failed."));
    } else {
        callback(std::move(wil::com_ptr<ICoreWebView2PointerInfo>(pointer)), nullptr);
    }
}

void WebviewHost::CreateWebViewCompositionController(
        HWND hwnd, CompositionControllerCreationCallback callback) {
    auto hr = webview_env_->CreateCoreWebView2CompositionController(
            hwnd,
            Callback<
                    ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
                    [callback](HRESULT hr,
                               ICoreWebView2CompositionController* compositionController)
                            -> HRESULT {
                        if (SUCCEEDED(hr)) {
                            callback(
                                    std::move(wil::com_ptr<ICoreWebView2CompositionController>(
                                            compositionController)),
                                    nullptr);
                        } else {
                            callback(nullptr, WebviewCreationError::create(
                                    hr,
                                    "CreateCoreWebView2CompositionController "
                                    "completion handler failed."));
                        }
                        return S_OK;
                    })
                    .Get());

    if (FAILED(hr)) {
        callback(nullptr,
                 WebviewCreationError::create(
                         hr, "CreateCoreWebView2CompositionController failed."));
    }
}
