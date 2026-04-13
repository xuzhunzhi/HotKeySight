using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using System.Collections.ObjectModel;

namespace HotKeySight.Pages
{
    public class AppInfo
    {
        public string Name { get; set; } = "";
        public string Path { get; set; } = "";
        public string Icon { get; set; } = "";
        public string HotkeyCount => "热键数量: 0";
    }

    public class HotkeyInfo
    {
        public string Description { get; set; } = "";
        public string HotkeyText { get; set; } = "";
    }

    public sealed partial class ByAppPage : Page
    {
        public ObservableCollection<AppInfo> Apps { get; } = new();
        public ObservableCollection<HotkeyInfo> Hotkeys { get; } = new();

        public ByAppPage()
        {
            InitializeComponent();
            AppListView.ItemsSource = Apps;
            HotkeyListView.ItemsSource = Hotkeys;
            EmptyState.Visibility = Visibility.Visible;
        }

        private void AppListView_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            if (AppListView.SelectedItem is AppInfo app)
            {
                AppNameText.Text = app.Name;
                AppPathText.Text = app.Path;
                EmptyState.Visibility = Visibility.Collapsed;
                // TODO: Load hotkeys for selected app
            }
        }
    }
}
