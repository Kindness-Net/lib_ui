// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "ui/layers/box_layer_shadow.h"

#include "ui/widgets/shadow.h"
#include "styles/style_layers.h"

namespace Ui {
namespace {

// 使用方圆角相同，共用一份缓存不会互相冲掉。
[[nodiscard]] const BoxShadow &SharedShadow() {
	static const auto result = BoxShadow(st::boxRoundShadow);
	return result;
}

} // namespace

QMargins BoxLayerShadowExtend() {
	return BoxShadow::ExtendFor(st::boxRoundShadow);
}

void PaintBoxLayerShadow(QPainter &p, const QRect &box) {
	SharedShadow().paint(p, box, st::boxRadius);
}

} // namespace Ui
