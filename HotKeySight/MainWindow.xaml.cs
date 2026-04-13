using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Navigation;
using HotKeySight.Pages;

namespace HotKeySight
{
    public sealed partial class MainWindow : Window
    {
        public MainWindow()
        {
            InitializeComponent();
            NavView.SelectionChanged += NavView_SelectionChanged;
            ContentFrame.Navigate(typeof(ByAppPage));

            // Select first item by default
            if (NavView.MenuItems.Count > 0)
            {
                NavView.SelectedItem = NavView.MenuItems[0];
            }
        }

        private void NavView_SelectionChanged(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
        {
            if (args.SelectedItem is NavigationViewItem item)
            {
                string tag = item.Tag.ToString();
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
    }
}
