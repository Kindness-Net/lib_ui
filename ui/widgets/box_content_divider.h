// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include "ui/rp_widget.h"

namespace style {
struct DividerBar;
} // namespace style

namespace st {
extern const style::DividerBar &defaultDividerBar;
extern const int &boxDividerHeight;
} // namespace st

namespace Ui {

class BoxContentDivider : public Ui::RpWidget {
public:
	BoxContentDivider(
		QWidget *parent,
		int height = st::boxDividerHeight,
		const style::DividerBar &st = st::defaultDividerBar);

	QAccessible::Role accessibilityRole() override {
		return QAccessible::Role::Separator;
	}

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	const style::DividerBar &_st;

};

} // namespace Ui
