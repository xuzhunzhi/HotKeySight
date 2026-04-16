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
        // 侧边栏背景色（浅色模式）
        private static readonly Color LightSidebarColor = Color.FromArgb(255, 243, 243, 243);
        // 侧边栏背景色（深色模式）
        private static readonly Color DarkSidebarColor = Color.FromArgb(255, 20, 20, 20);

        // 窗口最小尺寸
        private const double MinWindowWidth = 1000;
        private const double MinWindowHeight = 750;

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

            // 设置窗口最小尺寸
            var windowHandle = WinRT.Interop.WindowNative.GetWindowHandle(this);
            SetWindowPos(windowHandle, IntPtr.Zero, 0, 0, (int)MinWindowWidth, (int)MinWindowHeight, SWP_NOZORDER);

            // 设置初始主题
            var initialTheme = Microsoft.UI.Xaml.ElementTheme.Light;
            NavView.RequestedTheme = initialTheme;
            ContentFrame.RequestedTheme = initialTheme;
            NavView.Background = new SolidColorBrush(LightSidebarColor);

            // 移除 NavigationView 内容区域的默认内边距
            NavView.RegisterPropertyChangedCallback(NavigationView.ContentProperty, (sender, args) =>
            {
                if (NavView.Content is FrameworkElement fe)
                {
                    fe.Margin = new Thickness(0);
                }
            });
            if (NavView.Content is FrameworkElement fe)
            {
                fe.Margin = new Thickness(0);
            }

            // 自定义标题栏
            CustomizeTitleBar(initialTheme);

            ContentFrame.Navigate(typeof(ByAppPage));

            if (NavView.MenuItems.Count > 0)
            {
                NavView.SelectedItem = NavView.MenuItems[0];
            }
        }

        public void UpdateTitleBarTheme(ElementTheme theme)
        {
            CustomizeTitleBar(theme);
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

        private void CustomizeTitleBar(ElementTheme theme)
        {
            // 获取 AppWindow
            var windowHandle = WinRT.Interop.WindowNative.GetWindowHandle(this);
            var windowId = Microsoft.UI.Win32Interop.GetWindowIdFromWindow(windowHandle);
            var appWindow = Microsoft.UI.Windowing.AppWindow.GetFromWindowId(windowId);

            if (appWindow != null)
            {
                var titleBar = appWindow.TitleBar;
                var sidebarColor = theme == ElementTheme.Dark ? DarkSidebarColor : LightSidebarColor;
                var contentColor = theme == ElementTheme.Dark
                    ? Color.FromArgb(255, 30, 30, 30)
                    : Colors.White;

                // 设置标题栏颜色
                titleBar.BackgroundColor = sidebarColor;
                titleBar.ForegroundColor = contentColor;
                titleBar.InactiveBackgroundColor = sidebarColor;
                titleBar.InactiveForegroundColor = contentColor;

                // 设置按钮颜色（关闭、最大化、最小化）
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

    }
}
