#include "pch.h"

#include <set>

#include <common/Display/dpi_aware.h>
#include <common/interop/shared_constants.h>
#include <common/themes/windows_colors.h>
#include <common/utils/EventLocker.h>

#include <KeyboardManagerConstants.h>
#include <KeyboardManagerState.h>

#include "EditKeyboardWindow.h"
#include "ErrorTypes.h"
#include "SingleKeyRemapControl.h"
#include "KeyDropDownControl.h"
#include "XamlBridge.h"
#include "Styles.h"
#include "Dialog.h"
#include "LoadingAndSavingRemappingHelper.h"
#include "UIHelpers.h"
#include <common/utils/winapi_error.h>

using namespace winrt::Windows::Foundation;

LRESULT CALLBACK EditKeyboardWindowProc(HWND, UINT, WPARAM, LPARAM);

// This Hwnd will be the window handler for the Xaml Island: A child window that contains Xaml.
HWND hWndXamlIslandEditKeyboardWindow = nullptr;

// This variable is used to check if window registration has been done to avoid repeated registration leading to an error.
bool isEditKeyboardWindowRegistrationCompleted = false;

// Holds the native window handle of EditKeyboard Window.
HWND hwndEditKeyboardNativeWindow = nullptr;
std::mutex editKeyboardWindowMutex;

// Stores a pointer to the Xaml Bridge object so that it can be accessed from the window procedure
static XamlBridge* xamlBridgePtr = nullptr;

static IAsyncOperation<bool> OrphanKeysConfirmationDialog(
    KeyboardManagerState& state,
    const std::vector<DWORD>& keys,
    XamlRoot root)
{
    ContentDialog confirmationDialog;
    confirmationDialog.XamlRoot(root);
    confirmationDialog.Title(box_value(GET_RESOURCE_STRING(IDS_EDITKEYBOARD_ORPHANEDDIALOGTITLE)));
    confirmationDialog.Content(nullptr);
    confirmationDialog.IsPrimaryButtonEnabled(true);
    confirmationDialog.DefaultButton(ContentDialogButton::Primary);
    confirmationDialog.PrimaryButtonText(winrt::hstring(GET_RESOURCE_STRING(IDS_CONTINUE_BUTTON)));
    confirmationDialog.IsSecondaryButtonEnabled(true);
    confirmationDialog.SecondaryButtonText(winrt::hstring(GET_RESOURCE_STRING(IDS_CANCEL_BUTTON)));

    TextBlock orphanKeysBlock;
    std::wstring orphanKeyString;
    for (auto k : keys)
    {
        orphanKeyString.append(state.keyboardMap.GetKeyName(k));
        orphanKeyString.append(L", ");
    }

    orphanKeyString = orphanKeyString.substr(0, max(0, orphanKeyString.length() - 2));
    orphanKeysBlock.Text(winrt::hstring(orphanKeyString));
    orphanKeysBlock.TextWrapping(TextWrapping::Wrap);
    confirmationDialog.Content(orphanKeysBlock);

    ContentDialogResult res = co_await confirmationDialog.ShowAsync();

    co_return res == ContentDialogResult::Primary;
}

static IAsyncAction OnClickAccept(KeyboardManagerState& keyboardManagerState, XamlRoot root, std::function<void()> ApplyRemappings)
{
    KeyboardManagerHelper::ErrorType isSuccess = LoadingAndSavingRemappingHelper::CheckIfRemappingsAreValid(SingleKeyRemapControl::singleKeyRemapBuffer);

    if (isSuccess != KeyboardManagerHelper::ErrorType::NoError)
    {
        if (!co_await Dialog::PartialRemappingConfirmationDialog(root, GET_RESOURCE_STRING(IDS_EDITKEYBOARD_PARTIALCONFIRMATIONDIALOGTITLE)))
        {
            co_return;
        }
    }

    // Check for orphaned keys
    // Draw content Dialog
    std::vector<DWORD> orphanedKeys = LoadingAndSavingRemappingHelper::GetOrphanedKeys(SingleKeyRemapControl::singleKeyRemapBuffer);
    if (orphanedKeys.size() > 0)
    {
        if (!co_await OrphanKeysConfirmationDialog(keyboardManagerState, orphanedKeys, root))
        {
            co_return;
        }
    }

    ApplyRemappings();
}

// Function to create the Edit Keyboard Window
inline void CreateEditKeyboardWindowImpl(HINSTANCE hInst, KeyboardManagerState& keyboardManagerState)
{
    Logger::trace("CreateEditKeyboardWindowImpl()");
    auto locker = EventLocker::Get(KeyboardManagerConstants::EditorWindowEventName.c_str());
    if (!locker.has_value())
    {
        Logger::error(L"Failed to lock event {}. {}", KeyboardManagerConstants::EditorWindowEventName, get_last_error_or_default(GetLastError()));
    }

    Logger::trace(L"Signaled {} event to suspend the KBM engine", KeyboardManagerConstants::EditorWindowEventName);

    // Window Registration
    const wchar_t szWindowClass[] = L"EditKeyboardWindow";
    if (!isEditKeyboardWindowRegistrationCompleted)
    {
        WNDCLASSEX windowClass = {};
        windowClass.cbSize = sizeof(WNDCLASSEX);
        windowClass.lpfnWndProc = EditKeyboardWindowProc;
        windowClass.hInstance = hInst;
        windowClass.lpszClassName = szWindowClass;
        windowClass.hbrBackground = (HBRUSH)(COLOR_WINDOW);
        windowClass.hIcon = (HICON)LoadImageW(
            windowClass.hInstance,
            MAKEINTRESOURCE(IDS_KEYBOARDMANAGER_ICON),
            IMAGE_ICON,
            48,
            48,
            LR_DEFAULTCOLOR);
        
        if (RegisterClassEx(&windowClass) == NULL)
        {
            MessageBox(NULL, GET_RESOURCE_STRING(IDS_REGISTERCLASSFAILED_ERRORMESSAGE).c_str(), GET_RESOURCE_STRING(IDS_REGISTERCLASSFAILED_ERRORTITLE).c_str(), NULL);
            return;
        }

        isEditKeyboardWindowRegistrationCompleted = true;
    }

    // Find coordinates of the screen where the settings window is placed.
    RECT desktopRect = UIHelpers::GetForegroundWindowDesktopRect();

    // Calculate DPI dependent window size
    int windowWidth = KeyboardManagerConstants::DefaultEditKeyboardWindowWidth;
    int windowHeight = KeyboardManagerConstants::DefaultEditKeyboardWindowHeight;
    DPIAware::Convert(nullptr, windowWidth, windowHeight);
    
    // Window Creation
    HWND _hWndEditKeyboardWindow = CreateWindow(
        szWindowClass,
        GET_RESOURCE_STRING(IDS_EDITKEYBOARD_WINDOWNAME).c_str(),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MAXIMIZEBOX,
        ((desktopRect.right + desktopRect.left) / 2) - (windowWidth / 2),
        ((desktopRect.bottom + desktopRect.top) / 2) - (windowHeight / 2),
        windowWidth,
        windowHeight,
        NULL,
        NULL,
        hInst,
        NULL);
    
    if (_hWndEditKeyboardWindow == NULL)
    {
        MessageBox(NULL, GET_RESOURCE_STRING(IDS_CREATEWINDOWFAILED_ERRORMESSAGE).c_str(), GET_RESOURCE_STRING(IDS_CREATEWINDOWFAILED_ERRORTITLE).c_str(), NULL);
        return;
    }

    // Ensures the window is in foreground on first startup. If this is not done, the window appears behind because the thread is not on the foreground.
    if (_hWndEditKeyboardWindow)
    {
        SetForegroundWindow(_hWndEditKeyboardWindow);
    }

    // Store the newly created Edit Keyboard window's handle.
    std::unique_lock<std::mutex> hwndLock(editKeyboardWindowMutex);
    hwndEditKeyboardNativeWindow = _hWndEditKeyboardWindow;
    hwndLock.unlock();

    // Create the xaml bridge object
    XamlBridge xamlBridge(_hWndEditKeyboardWindow);

    // DesktopSource needs to be declared before the RelativePanel xamlContainer object to avoid errors
    winrt::Windows::UI::Xaml::Hosting::DesktopWindowXamlSource desktopSource;
    
    // Create the desktop window xaml source object and set its content
    hWndXamlIslandEditKeyboardWindow = xamlBridge.InitDesktopWindowsXamlSource(desktopSource);

    // Set the pointer to the xaml bridge object
    xamlBridgePtr = &xamlBridge;

    // Header for the window
    Windows::UI::Xaml::Controls::RelativePanel header;
    header.Margin({ 10, 10, 10, 30 });

    // Header text
    TextBlock headerText;
    headerText.Text(GET_RESOURCE_STRING(IDS_EDITKEYBOARD_WINDOWNAME));
    headerText.FontSize(30);
    headerText.Margin({ 0, 0, 0, 0 });
    header.SetAlignLeftWithPanel(headerText, true);

    // Header Cancel button
    Button cancelButton;
    cancelButton.Content(winrt::box_value(GET_RESOURCE_STRING(IDS_CANCEL_BUTTON)));
    cancelButton.Margin({ 10, 0, 0, 0 });
    cancelButton.Click([&](winrt::Windows::Foundation::IInspectable const& sender, RoutedEventArgs const&) {
        // Close the window since settings do not need to be saved
        PostMessage(_hWndEditKeyboardWindow, WM_CLOSE, 0, 0);
    });

    //  Text block for information about remap key section.
    TextBlock keyRemapInfoHeader;
    keyRemapInfoHeader.Text(GET_RESOURCE_STRING(IDS_EDITKEYBOARD_INFO));
    keyRemapInfoHeader.Margin({ 10, 0, 0, 10 });
    keyRemapInfoHeader.FontWeight(Text::FontWeights::SemiBold());
    keyRemapInfoHeader.TextWrapping(TextWrapping::Wrap);

    TextBlock keyRemapInfoExample;
    keyRemapInfoExample.Text(GET_RESOURCE_STRING(IDS_EDITKEYBOARD_INFOEXAMPLE));
    keyRemapInfoExample.Margin({ 10, 0, 0, 20 });
    keyRemapInfoExample.FontStyle(Text::FontStyle::Italic);
    keyRemapInfoExample.TextWrapping(TextWrapping::Wrap);

    // Table to display the key remaps
    StackPanel keyRemapTable;

    // First header textblock in the header row of the keys remap table
    TextBlock originalKeyRemapHeader;
    originalKeyRemapHeader.Text(GET_RESOURCE_STRING(IDS_EDITKEYBOARD_SOURCEHEADER));
    originalKeyRemapHeader.FontWeight(Text::FontWeights::Bold());
    StackPanel originalKeyHeaderContainer = UIHelpers::GetWrapped(originalKeyRemapHeader, KeyboardManagerConstants::RemapTableDropDownWidth + KeyboardManagerConstants::TableArrowColWidth).as<StackPanel>();

    // Second header textblock in the header row of the keys remap table
    TextBlock newKeyRemapHeader;
    newKeyRemapHeader.Text(GET_RESOURCE_STRING(IDS_EDITKEYBOARD_TARGETHEADER));
    newKeyRemapHeader.FontWeight(Text::FontWeights::Bold());

    StackPanel tableHeader = StackPanel();
    tableHeader.Orientation(Orientation::Horizontal);
    tableHeader.Margin({ 10, 0, 0, 10 });
    tableHeader.Children().Append(originalKeyHeaderContainer);
    tableHeader.Children().Append(newKeyRemapHeader);

    // Store handle of edit keyboard window
    SingleKeyRemapControl::EditKeyboardWindowHandle = _hWndEditKeyboardWindow;

    // Store keyboard manager state
    SingleKeyRemapControl::keyboardManagerState = &keyboardManagerState;
    KeyDropDownControl::keyboardManagerState = &keyboardManagerState;
    
    // Clear the single key remap buffer
    SingleKeyRemapControl::singleKeyRemapBuffer.clear();
    
    // Vector to store dynamically allocated control objects to avoid early destruction
    std::vector<std::vector<std::unique_ptr<SingleKeyRemapControl>>> keyboardRemapControlObjects;

    // Set keyboard manager UI state so that remaps are not applied while on this window
    keyboardManagerState.SetUIState(KeyboardManagerUIState::EditKeyboardWindowActivated, _hWndEditKeyboardWindow);

    // Load existing remaps into UI
    SingleKeyRemapTable singleKeyRemapCopy = keyboardManagerState.singleKeyReMap;

    LoadingAndSavingRemappingHelper::PreProcessRemapTable(singleKeyRemapCopy);

    for (const auto& it : singleKeyRemapCopy)
    {
        SingleKeyRemapControl::AddNewControlKeyRemapRow(keyRemapTable, keyboardRemapControlObjects, it.first, it.second);
    }

    // Main Header Apply button
    Button applyButton;
    applyButton.Content(winrt::box_value(GET_RESOURCE_STRING(IDS_OK_BUTTON)));
    applyButton.Style(AccentButtonStyle());
    applyButton.MinWidth(KeyboardManagerConstants::HeaderButtonWidth);
    cancelButton.MinWidth(KeyboardManagerConstants::HeaderButtonWidth);
    header.SetAlignRightWithPanel(cancelButton, true);
    header.SetLeftOf(applyButton, cancelButton);

    auto ApplyRemappings = [&keyboardManagerState, _hWndEditKeyboardWindow]() {
        LoadingAndSavingRemappingHelper::ApplySingleKeyRemappings(keyboardManagerState, SingleKeyRemapControl::singleKeyRemapBuffer, true);
        bool saveResult = keyboardManagerState.SaveConfigToFile();
        PostMessage(_hWndEditKeyboardWindow, WM_CLOSE, 0, 0);
    };

    applyButton.Click([&keyboardManagerState, ApplyRemappings, applyButton](winrt::Windows::Foundation::IInspectable const& sender, RoutedEventArgs const&) {
        OnClickAccept(keyboardManagerState, applyButton.XamlRoot(), ApplyRemappings);
    });

    header.Children().Append(headerText);
    header.Children().Append(applyButton);
    header.Children().Append(cancelButton);

    ScrollViewer scrollViewer;
    scrollViewer.VerticalScrollMode(ScrollMode::Enabled);
    scrollViewer.HorizontalScrollMode(ScrollMode::Enabled);
    scrollViewer.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
    scrollViewer.HorizontalScrollBarVisibility(ScrollBarVisibility::Auto);

    // Add remap key button
    Windows::UI::Xaml::Controls::Button addRemapKey;
    FontIcon plusSymbol;
    plusSymbol.FontFamily(Media::FontFamily(L"Segoe MDL2 Assets"));
    plusSymbol.Glyph(L"\xE109");
    addRemapKey.Content(plusSymbol);
    addRemapKey.Margin({ 10, 10, 0, 25 });
    addRemapKey.Click([&](winrt::Windows::Foundation::IInspectable const& sender, RoutedEventArgs const&) {
        SingleKeyRemapControl::AddNewControlKeyRemapRow(keyRemapTable, keyboardRemapControlObjects);

        // Whenever a remap is added move to the bottom of the screen
        scrollViewer.ChangeView(nullptr, scrollViewer.ScrollableHeight(), nullptr);

        // Set focus to the first Type Button in the newly added row
        UIHelpers::SetFocusOnTypeButtonInLastRow(keyRemapTable, KeyboardManagerConstants::RemapTableColCount);
    });

    // Set accessible name for the addRemapKey button
    addRemapKey.SetValue(Automation::AutomationProperties::NameProperty(), box_value(GET_RESOURCE_STRING(IDS_ADD_KEY_REMAP_BUTTON)));

    // Add tooltip for add button which would appear on hover
    ToolTip addRemapKeytoolTip;
    addRemapKeytoolTip.Content(box_value(GET_RESOURCE_STRING(IDS_ADD_KEY_REMAP_BUTTON)));
    ToolTipService::SetToolTip(addRemapKey, addRemapKeytoolTip);

    // Header and example text at the top of the window
    StackPanel helperText;
    helperText.Children().Append(keyRemapInfoHeader);
    helperText.Children().Append(keyRemapInfoExample);

    // Remapping table
    StackPanel mappingsPanel;
    mappingsPanel.Children().Append(tableHeader);
    mappingsPanel.Children().Append(keyRemapTable);
    mappingsPanel.Children().Append(addRemapKey);

    // Remapping table should be scrollable
    scrollViewer.Content(mappingsPanel);

    // Creating the Xaml content. xamlContainer is the parent UI element
    RelativePanel xamlContainer;
    xamlContainer.SetBelow(helperText, header);
    xamlContainer.SetBelow(scrollViewer, helperText);
    xamlContainer.SetAlignLeftWithPanel(header, true);
    xamlContainer.SetAlignRightWithPanel(header, true);
    xamlContainer.SetAlignLeftWithPanel(helperText, true);
    xamlContainer.SetAlignRightWithPanel(helperText, true);
    xamlContainer.SetAlignLeftWithPanel(scrollViewer, true);
    xamlContainer.SetAlignRightWithPanel(scrollViewer, true);
    xamlContainer.Children().Append(header);
    xamlContainer.Children().Append(helperText);
    xamlContainer.Children().Append(scrollViewer);
    xamlContainer.UpdateLayout();

    desktopSource.Content(xamlContainer);
    ////End XAML Island section
    if (_hWndEditKeyboardWindow)
    {
        ShowWindow(_hWndEditKeyboardWindow, SW_SHOW);
        UpdateWindow(_hWndEditKeyboardWindow);
    }

    // Message loop:
    xamlBridge.MessageLoop();

    // Reset pointers to nullptr
    xamlBridgePtr = nullptr;
    hWndXamlIslandEditKeyboardWindow = nullptr;
    hwndLock.lock();
    hwndEditKeyboardNativeWindow = nullptr;
    keyboardManagerState.ResetUIState();
    keyboardManagerState.ClearRegisteredKeyDelays();

    // Cannot be done in WM_DESTROY because that causes crashes due to fatal app exit
    xamlBridge.ClearXamlIslands();
}

void CreateEditKeyboardWindow(HINSTANCE hInst, KeyboardManagerState& keyboardManagerState)
{
    // Move implementation into the separate method so resources get destroyed correctly
    CreateEditKeyboardWindowImpl(hInst, keyboardManagerState);

    // Calling ClearXamlIslands() outside of the message loop is not enough to prevent
    // Microsoft.UI.XAML.dll from crashing during deinitialization, see https://github.com/microsoft/PowerToys/issues/10906
    Logger::trace("Terminating process {}", GetCurrentProcessId());
    Logger::flush();
    TerminateProcess(GetCurrentProcess(), 0);
}

inline std::wstring getMessage(UINT messageCode)
{
    switch (messageCode)
    {
    case WM_SIZE:
        return L"WM_SIZE";
    case WM_NCDESTROY:
        return L"WM_NCDESTROY";
    default:
        return L"";
    }
}

LRESULT CALLBACK EditKeyboardWindowProc(HWND hWnd, UINT messageCode, WPARAM wParam, LPARAM lParam)
{
    RECT rcClient;
    auto message = getMessage(messageCode);
    if (message != L"")
    {
        Logger::trace(L"EditKeyboardWindowProc() messageCode={}", getMessage(messageCode));
    }
    
    switch (messageCode)
    {
    // Resize the XAML window whenever the parent window is painted or resized
    case WM_PAINT:
    case WM_SIZE:
    {
        GetClientRect(hWnd, &rcClient);
        SetWindowPos(hWndXamlIslandEditKeyboardWindow, 0, rcClient.left, rcClient.top, rcClient.right, rcClient.bottom, SWP_SHOWWINDOW);
    }
    break;
    // To avoid UI elements overlapping on making the window smaller enforce a minimum window size
    case WM_GETMINMAXINFO:
    {
        LPMINMAXINFO lpMMI = (LPMINMAXINFO)lParam;
        int minWidth = KeyboardManagerConstants::MinimumEditKeyboardWindowWidth;
        int minHeight = KeyboardManagerConstants::MinimumEditKeyboardWindowHeight;
        DPIAware::Convert(nullptr, minWidth, minHeight);
        lpMMI->ptMinTrackSize.x = minWidth;
        lpMMI->ptMinTrackSize.y = minHeight;
    }
    break;
    default:
        // If the Xaml Bridge object exists, then use it's message handler to handle keyboard focus operations
        if (xamlBridgePtr != nullptr)
        {
            return xamlBridgePtr->MessageHandler(messageCode, wParam, lParam);
        }
        else if (messageCode == WM_NCDESTROY)
        {
            PostQuitMessage(0);
            break;
        }

        return DefWindowProc(hWnd, messageCode, wParam, lParam);
        break;
    }

    return 0;
}

// Function to check if there is already a window active if yes bring to foreground
bool CheckEditKeyboardWindowActive()
{
    bool result = false;
    std::unique_lock<std::mutex> hwndLock(editKeyboardWindowMutex);
    if (hwndEditKeyboardNativeWindow != nullptr)
    {
        // Check if the window is minimized if yes then restore the window.
        if (IsIconic(hwndEditKeyboardNativeWindow))
        {
            ShowWindow(hwndEditKeyboardNativeWindow, SW_RESTORE);
        }
        
        // If there is an already existing window no need to create a new open bring it on foreground.
        SetForegroundWindow(hwndEditKeyboardNativeWindow);
        result = true;
    }

    return result;
}

// Function to close any active Edit Keyboard window
void CloseActiveEditKeyboardWindow()
{
    std::unique_lock<std::mutex> hwndLock(editKeyboardWindowMutex);
    if (hwndEditKeyboardNativeWindow != nullptr)
    {
        Logger::trace("CloseActiveEditKeyboardWindow()");
        PostMessage(hwndEditKeyboardNativeWindow, WM_CLOSE, 0, 0);
    }
}
