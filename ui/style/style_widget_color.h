#pragma once

#include "base/basic_types.h"
#include <QtGui/QColor>

class QWidget;

namespace Ui {

enum class WidgetColorRole {
	Text,
	Background,
	Hover,
	Border,
};

// 宿主提供表面配色，独立使用界面库时保留原样式。
void SetWidgetColorResolver(QColor (*resolver)(
	not_null<const QWidget*>, QColor, WidgetColorRole));
[[nodiscard]] QColor ResolveWidgetColor(
	not_null<const QWidget*> widget,
	QColor color,
	WidgetColorRole role = WidgetColorRole::Text);

} // namespace Ui
