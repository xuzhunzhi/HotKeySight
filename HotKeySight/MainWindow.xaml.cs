using System;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Navigation;
using HotKeySight.Pages;
using Windows.UI;

namespace HotKeySight
{
    public sealed partial class MainWindow : Window
    {
        public MainWindow()
        {
            InitializeComponent();

            // 设置初始主题
            var initialTheme = Microsoft.UI.Xaml.ElementTheme.Light;
            NavView.RequestedTheme = initialTheme;
            ContentFrame.RequestedTheme = initialTheme;
            NavView.Background = new SolidColorBrush(Color.FromArgb(255, 243, 243, 243));

            ContentFrame.Navigate(typeof(ByAppPage));

            if (NavView.MenuItems.Count > 0)
            {
                NavView.SelectedItem = NavView.MenuItems[0];
            }
        }

        private void NavView_SelectionChanged(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
        {
            if (args.SelectedItem is NavigationViewItem item)
            {
                // 检测是否是设置项 (Tag = "Settings")
                if (item.Tag?.ToString() == "Settings")
                {
                    // 切换主题
                    var newTheme = NavView.RequestedTheme == Microsoft.UI.Xaml.ElementTheme.Dark
                        ? Microsoft.UI.Xaml.ElementTheme.Light
                        : Microsoft.UI.Xaml.ElementTheme.Dark;

                    // 应用到内容区域
                    ContentFrame.RequestedTheme = newTheme;

                    // 强制刷新 NavigationView 背景
                    UpdateNavigationViewTheme(newTheme);

                    // 取消选中设置项，保持在当前页面
                    sender.SelectedItem = null;
                    return;
                }

                string tag = item.Tag?.ToString() ?? string.Empty;
                Type pageType = tag switch
                {
                    "ByApp" => typeof(ByAppPage),
                    "ByHotKey" => typeof(ByHotKeyPage),
                    "Conflicts" => typeof(ConflictsPage),
                    _ => typeof(ByAppPage)
                };
                ContentFrame.Navigate(pageType);
            }
        }

        private void UpdateNavigationViewTheme(Microsoft.UI.Xaml.ElementTheme theme)
        {
            // 同时更新 NavigationView 的主题和背景
            NavView.RequestedTheme = theme;
            NavView.Background = new SolidColorBrush(
                theme == Microsoft.UI.Xaml.ElementTheme.Dark
                    ? Color.FromArgb(255, 20, 20, 20)  // 深色背景
                    : Color.FromArgb(255, 243, 243, 243)  // 浅色背景
            );
        }
    }
}
