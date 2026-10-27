// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "ui/widgets/menu/menu_separator.h"
#include "ui/style/style_widget_color.h"

#include "ui/painter.h"
#include "ui/widgets/rp_window.h"
#include "styles/style_widgets.h"

namespace Ui::Menu {

Separator::Separator(
	not_null<Menu*> parent,
	const style::Menu &st,
	const style::MenuSeparator &separator,
	not_null<QAction*> action)
: ItemBase(parent, st)
, _lineWidth(separator.width)
, _padding(separator.padding)
, _bg(st.itemBg)
, _height(_padding.top() + _lineWidth + _padding.bottom())
, _action(action) {
	setEnabled(false);
	fitToMenuWidth();
	paintRequest(
	) | rpl::on_next([=] {
		Painter p(this);

		p.fillRect(0, 0, width(), _height, ResolveWidgetColor(
			this, _bg->c, WidgetColorRole::Background));
		p.fillRect(
			_padding.left(),
			_padding.top(),
			width() - _padding.left() - _padding.right(),
			_lineWidth,
			WindowDividerColor());
	}, lifetime());
}

not_null<QAction*> Separator::action() const {
	return _action;
}

bool Separator::isEnabled() const {
	return false;
}

int Separator::contentHeight() const {
	return _height;
}

} // namespace Ui::Menu
