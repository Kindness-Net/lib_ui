#include "ui/style/style_widget_color.h"

namespace Ui {
namespace {

QColor (*ColorResolver)(not_null<const QWidget*>, QColor, WidgetColorRole) = nullptr;

} // namespace

void SetWidgetColorResolver(QColor (*resolver)(
		not_null<const QWidget*>, QColor, WidgetColorRole)) {
	ColorResolver = resolver;
}

QColor ResolveWidgetColor(
		not_null<const QWidget*> widget,
		QColor color,
		WidgetColorRole role) {
	return ColorResolver ? ColorResolver(widget, color, role) : color;
}

} // namespace Ui
