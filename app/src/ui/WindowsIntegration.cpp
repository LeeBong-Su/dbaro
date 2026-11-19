#include "WindowsIntegration.h"

#include <QCoreApplication>
#include <QDir>
#include <QWidget>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dwmapi.h>
#include <objectarray.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shobjidl.h>
#endif

namespace {
#ifdef Q_OS_WIN
constexpr wchar_t kAppId[] = L"DBaro.Desktop";
bool g_appIdSet = false;

template <typename T>
void release(T *&pointer)
{
    if (pointer) {
        pointer->Release();
        pointer = nullptr;
    }
}

std::wstring wide(const QString &text)
{
    return text.toStdWString();
}

IShellLinkW *makeLink(const WindowsIntegration::JumpItem &item)
{
    IShellLinkW *link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))))
        return nullptr;
    const std::wstring exe = wide(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()));
    link->SetPath(exe.c_str());
    link->SetArguments(wide(item.arguments).c_str());
    link->SetDescription(wide(item.description.left(250)).c_str());
    link->SetIconLocation(exe.c_str(), 0);

    IPropertyStore *store = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&store)))) {
        PROPVARIANT title;
        if (SUCCEEDED(InitPropVariantFromString(wide(item.title).c_str(), &title))) {
            store->SetValue(PKEY_Title, title);
            store->Commit();
            PropVariantClear(&title);
        }
        release(store);
    }
    return link;
}

QString argumentsOf(IUnknown *object)
{
    IShellLinkW *link = nullptr;
    if (FAILED(object->QueryInterface(IID_PPV_ARGS(&link))))
        return {};
    wchar_t buffer[1024] = {};
    link->GetArguments(buffer, 1024);
    release(link);
    return QString::fromWCharArray(buffer);
}

IObjectArray *collect(const QList<WindowsIntegration::JumpItem> &items, const QStringList &removed)
{
    IObjectCollection *collection = nullptr;
    if (FAILED(CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&collection))))
        return nullptr;
    for (const WindowsIntegration::JumpItem &item : items) {
        if (removed.contains(item.arguments))
            continue;
        if (IShellLinkW *link = makeLink(item)) {
            collection->AddObject(link);
            release(link);
        }
    }
    IObjectArray *array = nullptr;
    collection->QueryInterface(IID_PPV_ARGS(&array));
    release(collection);
    return array;
}
#endif
} // namespace

void WindowsIntegration::setAppUserModelId()
{
#ifdef Q_OS_WIN
    g_appIdSet = SUCCEEDED(SetCurrentProcessExplicitAppUserModelID(kAppId));
#endif
}

void WindowsIntegration::updateJumpList(const QString &category, const QList<JumpItem> &items, const QList<JumpItem> &tasks)
{
#ifdef Q_OS_WIN
    if (!g_appIdSet)
        return;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ICustomDestinationList *list = nullptr;
    IObjectArray *removedArray = nullptr;
    IObjectArray *itemsArray = nullptr;
    IObjectArray *tasksArray = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&list)))) {
        list->SetAppID(kAppId);
        UINT slotCount = 0;
        if (SUCCEEDED(list->BeginList(&slotCount, IID_PPV_ARGS(&removedArray)))) {
            QStringList removed;
            UINT count = 0;
            if (removedArray && SUCCEEDED(removedArray->GetCount(&count))) {
                for (UINT i = 0; i < count; ++i) {
                    IUnknown *object = nullptr;
                    if (SUCCEEDED(removedArray->GetAt(i, IID_PPV_ARGS(&object)))) {
                        removed << argumentsOf(object);
                        release(object);
                    }
                }
            }
            itemsArray = collect(items, removed);
            UINT itemCount = 0;
            if (itemsArray && SUCCEEDED(itemsArray->GetCount(&itemCount)) && itemCount > 0)
                list->AppendCategory(wide(category).c_str(), itemsArray);
            tasksArray = collect(tasks, {});
            if (tasksArray)
                list->AddUserTasks(tasksArray);
            list->CommitList();
        }
    }
    release(tasksArray);
    release(itemsArray);
    release(removedArray);
    release(list);
    if (SUCCEEDED(init))
        CoUninitialize();
#else
    Q_UNUSED(category)
    Q_UNUSED(items)
    Q_UNUSED(tasks)
#endif
}

void WindowsIntegration::setBorderColor(QWidget *window, const QColor &color)
{
#ifdef Q_OS_WIN
    if (!window || !window->testAttribute(Qt::WA_WState_Created))
        return;
    constexpr DWORD kBorderColor = 34;              // DWMWA_BORDER_COLOR (Windows 11)
    constexpr COLORREF kDefaultColor = 0xFFFFFFFF;  // DWMWA_COLOR_DEFAULT
    const COLORREF value = color.isValid() ? RGB(color.red(), color.green(), color.blue()) : kDefaultColor;
    DwmSetWindowAttribute(reinterpret_cast<HWND>(window->winId()), kBorderColor, &value, sizeof(value));
#else
    Q_UNUSED(window)
    Q_UNUSED(color)
#endif
}

void WindowsIntegration::allowForegroundForOtherProcess()
{
#ifdef Q_OS_WIN
    AllowSetForegroundWindow(ASFW_ANY);
#endif
}

void WindowsIntegration::bringToFront(QWidget *window)
{
    if (!window)
        return;
    if (window->isMinimized())
        window->showNormal();
    else
        window->show();
    window->raise();
    window->activateWindow();
#ifdef Q_OS_WIN
    if (window->testAttribute(Qt::WA_WState_Created))
        SetForegroundWindow(reinterpret_cast<HWND>(window->winId()));
#endif
}
