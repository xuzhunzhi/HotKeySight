using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using System.Collections.ObjectModel;

namespace HotKeySight.Pages
{
    public class HotkeyResult
    {
        public string AppName { get; set; } = "";
        public string Description { get; set; } = "";
        public string Icon { get; set; } = "";
        public bool IsConflict { get; set; }
    }

    public sealed partial class ByHotKeyPage : Page
    {
        public ObservableCollection<HotkeyResult> Results { get; } = new();

        public ByHotKeyPage()
        {
            InitializeComponent();
            ResultListView.ItemsSource = Results;
        }
    }
}
