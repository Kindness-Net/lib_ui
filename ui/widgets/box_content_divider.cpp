// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "ui/widgets/box_content_divider.h"

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
	// 窗口材质表面不画分组条背景，交给材质显示。
	if (property("ExtrasWindowMaterialSurfaceActive").toBool()) {
		return;
	}
	QPainter p(this);
	p.fillRect(e->rect(), _st.bg);
}

} // namespace Ui
