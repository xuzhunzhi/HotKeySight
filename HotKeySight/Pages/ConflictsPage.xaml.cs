using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using System.Collections.ObjectModel;

namespace HotKeySight.Pages
{
    public class ConflictInfo
    {
        public string HotkeyText { get; set; } = "";
        public ObservableCollection<AppInfo> Apps { get; } = new();
    }

    public sealed partial class ConflictsPage : Page
    {
        public ObservableCollection<ConflictInfo> Conflicts { get; } = new();

        public ConflictsPage()
        {
            InitializeComponent();
            ConflictsListView.ItemsSource = Conflicts;
        }
    }
}
