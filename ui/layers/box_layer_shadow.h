// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

namespace Ui {

// 圆角为 st::boxRadius 的面板共用的阴影，运行时按圆角生成，圆角改动时自动跟随。
[[nodiscard]] QMargins BoxLayerShadowExtend();
void PaintBoxLayerShadow(QPainter &p, const QRect &box);

} // namespace Ui
