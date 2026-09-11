module;

#include <Windows.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <thread>
#include <functional>

export module taskbar.Taskbar;

import taskbar.Handler;
import taskbar.Registry;

export class Taskbar {
public:
    typedef std::function<void()> Callback;

    // 任务栏布局快照（由布局线程独立测量，主线程只读应用）
    struct TaskbarLayout {
        RECT frame{};
        RECT tray{};
        RECT widgets{};
        RECT taskList{};
        bool centered = false;
        bool widgetsEnabled = false;
        // 主题快照：注册表读取（微秒级，无 explorer 依赖），主线程 applyLayout
        // 中 diff 后应用——作为 WM_SETTINGCHANGE 广播的兜底（锁屏/唤醒/
        // 第三方工具直写注册表等错过广播的场景，2s 心跳内收敛）
        bool lightTheme = false;
    };

private:
    Microsoft::WRL::ComPtr<Handler> handler{};
    Microsoft::WRL::ComPtr<IUIAutomation> automation{};
    Microsoft::WRL::ComPtr<IUIAutomationElement> root{};

    static auto createConditionByProperty(IUIAutomation *automation, PROPERTYID propertyId, const wchar_t *value) -> Microsoft::WRL::ComPtr<IUIAutomationCondition> {
        VARIANT var{};
        VariantInit(&var);
        var.vt = VT_BSTR;
        var.bstrVal = SysAllocString(value);
        Microsoft::WRL::ComPtr<IUIAutomationCondition> condition{};
        automation->CreatePropertyCondition(propertyId, var, &condition);
        SysFreeString(var.bstrVal);
        VariantClear(&var);
        return condition;
    }

public:
    Taskbar() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
    }

    ~Taskbar() {
        CoUninitialize();
    }

    auto initialize() {
        CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_IUIAutomation, &this->automation);
        Microsoft::WRL::ComPtr<IUIAutomationElement> element{};
        this->automation->ElementFromHandle(Taskbar::getHWND(), &element);
        const auto condition = this->createConditionByProperty(this->automation.Get(), UIA_ClassNamePropertyId, L"Windows.UI.Input.InputSite.WindowClass");
        element->FindFirst(TreeScope_Children, condition.Get(), &this->root);
    }

    auto setListener(const Taskbar::Callback &callback) {
        this->handler = new Handler(callback);
        this->automation->AddStructureChangedEventHandler(this->root.Get(), TreeScope_Descendants, nullptr, this->handler.Get());
        std::thread([callback] {
            Registry::onWatch(callback);
        }).detach();
    }

    // 独立测量任务栏布局（布局线程专用）：每次新建 UIAutomation 实例，
    // 无跨线程共享对象；阻塞/失败均不影响主线程。调用线程需已 CoInitialize。
    static auto measureLayout() -> TaskbarLayout {
        TaskbarLayout out{};
        out.centered = Registry::isTaskbarCentered();
        out.widgetsEnabled = Registry::isWidgetsEnabled();
        out.lightTheme = Registry::isLightTheme();
        Microsoft::WRL::ComPtr<IUIAutomation> automation{};
        if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_IUIAutomation, &automation))) {
            return out;
        }
        Microsoft::WRL::ComPtr<IUIAutomationElement> element{};
        if (FAILED(automation->ElementFromHandle(Taskbar::getHWND(), &element))) {
            return out;
        }
        Microsoft::WRL::ComPtr<IUIAutomationElement> root{};
        const auto condition = createConditionByProperty(automation.Get(), UIA_ClassNamePropertyId, L"Windows.UI.Input.InputSite.WindowClass");
        element->FindFirst(TreeScope_Children, condition.Get(), &root);
        if (!root) {
            return out;
        }
        out.frame = getRectForTaskbarFrame(automation.Get(), root.Get());
        out.tray = getRectForTrayFrame(automation.Get(), root.Get());
        out.widgets = getRectForWidgetsButton(automation.Get(), root.Get(), out.widgetsEnabled);
        out.taskList = getRectForTaskList(automation.Get(), root.Get());
        return out;
    }

    static auto getRectForTaskbarFrame(IUIAutomation *automation, IUIAutomationElement *root) -> RECT {
        RECT rect{};
        const auto condition = createConditionByProperty(automation, UIA_ClassNamePropertyId, L"Taskbar.TaskbarFrameAutomationPeer");
        Microsoft::WRL::ComPtr<IUIAutomationElement> element{};
        root->FindFirst(TreeScope_Children, condition.Get(), &element);
        element->get_CurrentBoundingRectangle(&rect);
        return rect;
    }

    static auto getRectForTaskList(IUIAutomation *automation, IUIAutomationElement *root) -> RECT {
        RECT rect{
            .left = LONG_MAX,
            .top = LONG_MAX,
            .right = LONG_MIN,
            .bottom = LONG_MIN
        };
        const auto conditionID = createConditionByProperty(automation, UIA_AutomationIdPropertyId, L"StartButton");
        const auto conditionCN = createConditionByProperty(automation, UIA_ClassNamePropertyId, L"Taskbar.TaskListButtonAutomationPeer");
        Microsoft::WRL::ComPtr<IUIAutomationCondition> condition{};
        automation->CreateOrCondition(conditionID.Get(), conditionCN.Get(), &condition);
        Microsoft::WRL::ComPtr<IUIAutomationElementArray> elements{};
        root->FindAll(TreeScope_Descendants, condition.Get(), &elements);
        int length = 0;
        elements->get_Length(&length);
        for (int i = 0; i < length; i++) {
            RECT tempRect{};
            Microsoft::WRL::ComPtr<IUIAutomationElement> element{};
            elements->GetElement(i, &element);
            element->get_CurrentBoundingRectangle(&tempRect);
            rect = {
                .left = min(rect.left, tempRect.left),
                .top = min(rect.top, tempRect.top),
                .right = max(rect.right, tempRect.right),
                .bottom = max(rect.bottom, tempRect.bottom)
            };
        }
        return rect;
    }

    static auto getRectForTrayFrame(IUIAutomation *automation, IUIAutomationElement *root) -> RECT {
        RECT rect{
            .left = LONG_MAX,
            .top = LONG_MAX,
            .right = LONG_MIN,
            .bottom = LONG_MIN
        };
        const auto condition = createConditionByProperty(automation, UIA_AutomationIdPropertyId, L"SystemTrayIcon");
        Microsoft::WRL::ComPtr<IUIAutomationElementArray> elements{};
        root->FindAll(TreeScope_Children, condition.Get(), &elements);
        int length = 0;
        elements->get_Length(&length);
        for (int i = 0; i < length; i++) {
            RECT tempRect{};
            Microsoft::WRL::ComPtr<IUIAutomationElement> element{};
            elements->GetElement(i, &element);
            element->get_CurrentBoundingRectangle(&tempRect);
            rect = {
                .left = min(rect.left, tempRect.left),
                .top = min(rect.top, tempRect.top),
                .right = max(rect.right, tempRect.right),
                .bottom = max(rect.bottom, tempRect.bottom)
            };
        }
        return rect;
    }

    static auto getRectForWidgetsButton(IUIAutomation *automation, IUIAutomationElement *root, const bool widgetsEnabled) -> RECT {
        RECT rect{};
        if (widgetsEnabled) {
            const auto condition = createConditionByProperty(automation, UIA_AutomationIdPropertyId, L"WidgetsButton");
            Microsoft::WRL::ComPtr<IUIAutomationElement> element{};
            root->FindFirst(TreeScope_Descendants, condition.Get(), &element);
            element->get_CurrentBoundingRectangle(&rect);
        }
        return rect;
    }

    static auto getHWND() -> HWND {
        return FindWindow(L"Shell_TrayWnd", nullptr);
    }
};
