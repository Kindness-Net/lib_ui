// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "ui/widgets/box_content_divider.h"
#include "ui/style/style_widget_color.h"

#include "styles/style_widgets.h"
#include "styles/palette.h"

#include <QtGui/QPainter>
#include <QtGui/QtEvents>

namespace Ui {

BoxContentDivider::BoxContentDivider(
	QWidget *parent,
	int height,
	const style::DividerBar &st)
: RpWidget(parent)
, _st(st) {
	resize(width(), height);
}

void BoxContentDivider::paintEvent(QPaintEvent *e) {
	if (property("ExtrasWindowMaterialSurfaceActive").toBool()) {
		QPainter(this).fillRect(e->rect(), ResolveWidgetColor(
			this, _st.bg->c, WidgetColorRole::Border));
		return;
	}
	QPainter p(this);
	p.fillRect(e->rect(), _st.bg);
}

} // namespace Ui
