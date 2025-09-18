#include "webview_host.h"

#include <wrl.h>
#include <future>
#include <iostream>

#include "util/rohelper.h"

using namespace Microsoft::WRL;

static std::wstring GetBrowserFolderFromEnvOrArg(
        const std::optional<std::wstring>& argPathOpt) {
    // If Dart passed a path, prefer it
    if (argPathOpt.has_value() && !argPathOpt->empty()) {
        return *argPathOpt;
    }
    // Otherwise read env var – it must be a folder containing msedgewebview2.exe
    wchar_t* envp = _wgetenv(L"WEBVIEW2_BROWSER_EXECUTABLE_FOLDER");
    if (envp && *envp) {
        return std::wstring(envp);
    }
    return L"";
}

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

    // Compute folder to pass to WebView2 (explicit beats relying on env-only)
    std::wstring exeFolder = GetBrowserFolderFromEnvOrArg(browser_exe_path);
    const wchar_t* browserFolderArg =
            exeFolder.empty() ? nullptr : exeFolder.c_str();

    const wchar_t* dataDirArg =
            (user_data_directory.has_value() && !user_data_directory->empty())
            ? user_data_directory->c_str()
            : nullptr;

    std::promise<HRESULT> result_promise;
    wil::com_ptr<ICoreWebView2Environment> env;

    HRESULT beginHr = CreateCoreWebView2EnvironmentWithOptions(
            browserFolderArg,
            dataDirArg,
            opts.get(),
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                    [&result_promise, &env](HRESULT r, ICoreWebView2Environment* createdEnv) -> HRESULT {
                        result_promise.set_value(r);
                        env.swap(createdEnv);
                        return S_OK;
                    })
                    .Get());

    if (FAILED(beginHr)) {
        std::cerr << "CreateCoreWebView2EnvironmentWithOptions call failed immediately. HRESULT=0x"
                  << std::hex << beginHr << std::dec << std::endl;
        return {};
    }

    HRESULT completedHr = result_promise.get_future().get();
    if (FAILED(completedHr)) {
        std::cerr << "CreateCoreWebView2EnvironmentWithOptions completion failed. HRESULT=0x"
                  << std::hex << completedHr << std::dec << std::endl;
        return {};
    }

    if (!env) {
        std::cerr << "WebView2 environment pointer is null after successful creation.\n";
        return {};
    }

    auto env3 = env.try_query<ICoreWebView2Environment3>();
    if (!env3) {
        std::cerr << "Failed to QI ICoreWebView2Environment3.\n";
        return {};
    }

    return std::unique_ptr<WebviewHost>(new WebviewHost(platform, std::move(env3)));
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
    ICoreWebView2PointerInfo* pointer = nullptr;
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
            Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
                    [callback](HRESULT hr, ICoreWebView2CompositionController* ctrl) -> HRESULT {
                        if (SUCCEEDED(hr)) {
                            callback(std::move(wil::com_ptr<ICoreWebView2CompositionController>(ctrl)), nullptr);
                        } else {
                            callback(nullptr, WebviewCreationError::create(
                                    hr,
                                    "CreateCoreWebView2CompositionController completion failed."));
                        }
                        return S_OK;
                    }).Get());

    if (FAILED(hr)) {
        callback(nullptr, WebviewCreationError::create(
                hr, "CreateCoreWebView2CompositionController failed."));
    }
}
