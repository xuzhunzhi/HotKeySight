using System;
using System.Runtime.InteropServices;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Navigation;
using Windows.UI;
using HotKeySight.Pages;
using WinRT;

namespace HotKeySight
{
    public sealed partial class MainWindow : Window
    {
        // 窗口最小尺寸
        private const double MinWindowWidth = 1000;
        private const double MinWindowHeight = 750;

        // 当前主题
        private ElementTheme _currentTheme = ElementTheme.Light;

        // 静态引用，用于从其他页面获取 MainWindow 实例
        private static MainWindow? _instance;
        public static MainWindow Instance => _instance;

        // 缓存的背景刷子（避免内存泄漏）
        private static readonly SolidColorBrush LightSidebarBrush = new(Color.FromArgb(255, 243, 243, 243));
        private static readonly SolidColorBrush DarkSidebarBrush = new(Color.FromArgb(255, 20, 20, 20));

        // Win32 API for SetWindowPos
        [DllImport("user32.dll")]
        private static extern bool SetWindowPos(IntPtr hWnd, IntPtr hWndInsertAfter, int X, int Y, int cx, int cy, uint uFlags);

        [DllImport("user32.dll")]
        private static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);

        [StructLayout(LayoutKind.Sequential)]
        private struct RECT
        {
            public int Left;
            public int Top;
            public int Right;
            public int Bottom;
        }

        private const uint SWP_NOZORDER = 0x0004;

        public MainWindow()
        {
            InitializeComponent();

            // 保存静态实例引用
            _instance = this;

            // 设置窗口最小尺寸
            var windowHandle = WinRT.Interop.WindowNative.GetWindowHandle(this);
            SetWindowPos(windowHandle, IntPtr.Zero, 0, 0, (int)MinWindowWidth, (int)MinWindowHeight, SWP_NOZORDER);

            // 设置初始主题
            var initialTheme = Microsoft.UI.Xaml.ElementTheme.Light;
            NavView.RequestedTheme = initialTheme;
            ContentFrame.RequestedTheme = initialTheme;
            NavView.Background = LightSidebarBrush;

            // 自定义标题栏
            UpdateTitleBarTheme(initialTheme);

            ContentFrame.Navigate(typeof(ByAppPage));

            // 监听导航完成事件，确保新页面应用当前主题
            ContentFrame.Navigated += OnContentFrameNavigated;

            if (NavView.MenuItems.Count > 0)
            {
                NavView.SelectedItem = NavView.MenuItems[0];
            }
        }

        public ElementTheme GetCurrentTheme()
        {
            return _currentTheme;
        }

        public void SetTheme(ElementTheme theme)
        {
            // 保存用户选择的主题
            _currentTheme = theme;

            // 如果是跟随系统，检测系统主题
            var appliedTheme = theme == ElementTheme.Default
                ? GetSystemTheme()
                : theme;

            // 应用到内容区域
            ContentFrame.RequestedTheme = appliedTheme;

            // 强制刷新 NavigationView 主题和背景
            UpdateNavigationViewTheme(appliedTheme);

            // 更新标题栏
            UpdateTitleBarTheme(appliedTheme);

            // 通知当前页面
            if (ContentFrame.Content is FrameworkElement contentElement)
            {
                contentElement.RequestedTheme = appliedTheme;
            }
        }

        private static ElementTheme GetSystemTheme()
        {
            try
            {
                // 通过注册表检测系统主题
                // HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize
                // AppsUseLightTheme = 1 表示浅色模式，0 表示深色模式
                using var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(
                    @"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize");
                if (key != null)
                {
                    var value = key.GetValue("AppsUseLightTheme");
                    if (value is int lightTheme)
                    {
                        return lightTheme == 0 ? ElementTheme.Dark : ElementTheme.Light;
                    }
                }
                return ElementTheme.Light;
            }
            catch
            {
                return ElementTheme.Light;
            }
        }

        private void UpdateTitleBarTheme(ElementTheme theme)
        {
            var windowHandle = WinRT.Interop.WindowNative.GetWindowHandle(this);
            var windowId = Microsoft.UI.Win32Interop.GetWindowIdFromWindow(windowHandle);
            var appWindow = Microsoft.UI.Windowing.AppWindow.GetFromWindowId(windowId);

            if (appWindow != null)
            {
                var titleBar = appWindow.TitleBar;
                var sidebarColor = theme == ElementTheme.Dark
                    ? Color.FromArgb(255, 20, 20, 20)
                    : Color.FromArgb(255, 243, 243, 243);
                var contentColor = theme == ElementTheme.Dark
                    ? Color.FromArgb(255, 30, 30, 30)
                    : Colors.White;

                titleBar.BackgroundColor = sidebarColor;
                titleBar.ForegroundColor = contentColor;
                titleBar.InactiveBackgroundColor = sidebarColor;
                titleBar.InactiveForegroundColor = contentColor;

                titleBar.ButtonBackgroundColor = sidebarColor;
                titleBar.ButtonForegroundColor = contentColor;
                titleBar.ButtonHoverBackgroundColor = theme == ElementTheme.Dark
                    ? Color.FromArgb(255, 40, 40, 40)
                    : Color.FromArgb(255, 220, 220, 220);
                titleBar.ButtonPressedBackgroundColor = theme == ElementTheme.Dark
                    ? Color.FromArgb(255, 50, 50, 50)
                    : Color.FromArgb(255, 200, 200, 200);
                titleBar.ButtonInactiveBackgroundColor = sidebarColor;
                titleBar.ButtonInactiveForegroundColor = contentColor;
            }
        }

        private void UpdateNavigationViewTheme(ElementTheme theme)
        {
            // 设置 RequestedTheme 让 ThemeResource 生效
            NavView.RequestedTheme = theme;

            // 更新 NavigationView 背景色
            NavView.Background = theme == ElementTheme.Dark ? DarkSidebarBrush : LightSidebarBrush;
        }

        private void Window_SizeChanged(object sender, WindowSizeChangedEventArgs e)
        {
            if (e.Size.Width < MinWindowWidth || e.Size.Height < MinWindowHeight)
            {
                var windowHandle = WinRT.Interop.WindowNative.GetWindowHandle(this);
                GetWindowRect(windowHandle, out RECT rect);
                var width = Math.Max(e.Size.Width, MinWindowWidth);
                var height = Math.Max(e.Size.Height, MinWindowHeight);
                SetWindowPos(windowHandle, IntPtr.Zero, rect.Left, rect.Top, (int)width, (int)height, SWP_NOZORDER);
            }
        }

        private void NavView_SelectionChanged(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
        {
            if (args.SelectedItem is NavigationViewItem item)
            {
                string tag = item.Tag?.ToString() ?? string.Empty;
                Type pageType = tag switch
                {
                    "ByApp" => typeof(ByAppPage),
                    "ByHotKey" => typeof(ByHotKeyPage),
                    "Conflicts" => typeof(ConflictsPage),
                    "Settings" => typeof(SettingsPage),
                    _ => typeof(ByAppPage)
                };

                ContentFrame.Navigate(pageType);
            }
        }

        private void OnContentFrameNavigated(object sender, NavigationEventArgs e)
        {
            // 确保导航到的页面应用当前主题
            if (ContentFrame.Content is FrameworkElement contentElement)
            {
                var appliedTheme = _currentTheme == ElementTheme.Default
                    ? GetSystemTheme()
                    : _currentTheme;
                contentElement.RequestedTheme = appliedTheme;
            }
        }
    }
}
