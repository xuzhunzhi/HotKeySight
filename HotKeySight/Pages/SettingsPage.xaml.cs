using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace HotKeySight.Pages
{
    public sealed partial class SettingsPage : Page
    {
        public SettingsPage()
        {
            InitializeComponent();
            Loaded += OnLoaded;
        }

        private void OnLoaded(object sender, RoutedEventArgs e)
        {
            // 同步当前主题到 ComboBox
            SyncThemeComboBox();
        }

        private void SyncThemeComboBox()
        {
            if (MainWindow.Instance is MainWindow mainWindow)
            {
                var currentTheme = mainWindow.GetCurrentTheme();
                string themeTag = currentTheme switch
                {
                    ElementTheme.Light => "Light",
                    ElementTheme.Dark => "Dark",
                    _ => "System"
                };

                foreach (ComboBoxItem item in ThemeComboBox.Items)
                {
                    if (item.Tag?.ToString() == themeTag)
                    {
                        ThemeComboBox.SelectedItem = item;
                        return;
                    }
                }
            }
        }

        private void ThemeComboBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            if (ThemeComboBox.SelectedItem is ComboBoxItem item)
            {
                var tag = item.Tag?.ToString() ?? "System";

                if (MainWindow.Instance is MainWindow mainWindow)
                {
                    ElementTheme theme = tag switch
                    {
                        "Light" => ElementTheme.Light,
                        "Dark" => ElementTheme.Dark,
                        _ => ElementTheme.Default
                    };

                    mainWindow.SetTheme(theme);
                }
            }
        }
    }
}
