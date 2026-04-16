using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace HotKeySight.Pages
{
    public sealed partial class SettingsPage : Page
    {
        public SettingsPage()
        {
            InitializeComponent();

            // 同步当前主题状态到 ToggleSwitch
            Loaded += OnLoaded;
        }

        private void OnLoaded(object sender, RoutedEventArgs e)
        {
            if (Window.Current?.Content is FrameworkElement rootElement)
            {
                ThemeToggle.IsOn = rootElement.RequestedTheme == ElementTheme.Dark;
            }
        }

        private void ThemeToggle_Toggled(object sender, RoutedEventArgs e)
        {
            // 设置主窗口的主题
            if (Window.Current?.Content is FrameworkElement rootElement)
            {
                var newTheme = ThemeToggle.IsOn
                    ? ElementTheme.Dark
                    : ElementTheme.Light;

                rootElement.RequestedTheme = newTheme;

                // 更新标题栏颜色
                if (Window.Current is MainWindow mainWindow)
                {
                    mainWindow.UpdateTitleBarTheme(newTheme);
                }
            }
        }
    }
}
