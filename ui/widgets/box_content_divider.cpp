// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "ui/widgets/box_content_divider.h"

#include "styles/style_layers.h"
#include "styles/style_widgets.h"
#include "styles/palette.h"

#include <QtGui/QPainter>
#include <QtGui/QtEvents>

namespace Ui {

BoxContentDivider::BoxContentDivider(
	QWidget *parent,
	int height,
	const style::DividerBar &st,
	RectParts parts)
: RpWidget(parent)
, _st(st)
, _parts(parts) {
	resize(width(), height);
}

void BoxContentDivider::paintEvent(QPaintEvent *e) {
	// 窗口材质表面不画分组条，背景和线条都交给材质。
	if (property("ExtrasWindowMaterialSurfaceActive").toBool()) {
		return;
	}
	QPainter p(this);

	p.fillRect(e->rect(), _st.bg);
	// 短分组条只保留居中的一条线，说明区域按指定边缘绘制。
	if (height() <= st::boxDividerHeight) {
		if (_parts & RectPart::Top) {
			paintTop(p, (height() - _st.top.height()) / 2);
		} else if (_parts & RectPart::Bottom) {
			paintBottom(p, (height() - _st.bottom.height()) / 2);
		}
		return;
	}
	if (_parts & RectPart::Top) {
		paintTop(p);
	}
	if (_parts & RectPart::Bottom) {
		paintBottom(p);
	}
}

void BoxContentDivider::paintTop(QPainter &p, int skip) {
	const auto inset = st::defaultBoxDividerLabelPadding.left();
	const auto dividerFillTop = QRect(
		inset,
		skip,
		width() - 2 * inset,
		_st.top.height());
	_st.top.fill(p, dividerFillTop);
}

void BoxContentDivider::paintBottom(QPainter &p, int skip) {
	const auto inset = st::defaultBoxDividerLabelPadding.left();
	const auto dividerFillBottom = myrtlrect(
		inset,
		height() - skip - _st.bottom.height(),
		width() - 2 * inset,
		_st.bottom.height());
	_st.bottom.fill(p, dividerFillBottom);
}

const style::color &BoxContentDivider::color() const {
	return _st.bg;
}

} // namespace Ui
