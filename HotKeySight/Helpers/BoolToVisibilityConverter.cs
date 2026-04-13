using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Data;

namespace HotKeySight.Helpers
{
    public class BoolToVisibilityConverter : IValueConverter
    {
        public object Convert(object value, System.Type targetType, object parameter, string language)
        {
            if (value is bool boolValue && boolValue)
            {
                return Visibility.Visible;
            }
            return Visibility.Collapsed;
        }

        public object ConvertBack(object value, System.Type targetType, object parameter, string language)
        {
            if (value is Visibility visibility && visibility == Visibility.Visible)
            {
                return true;
            }
            return false;
        }
    }
}
