// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "ui/layers/layer_widget.h"

#include "ui/layers/box_layer_widget.h"
#include "ui/widgets/shadow.h"
#include "ui/image/image_prepare.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/round_rect.h"
#include "base/qt/qt_tab_key.h"
#include "base/integration.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"
#include "styles/palette.h"

#include <QtGui/QtEvents>
#include <QtGui/QPainterPath>
#include <QtWidgets/QGraphicsEffect>

namespace Ui {
namespace {

// 主菜单外侧上下两角按 boxRadius 圆角绘制，阴影与压暗随之绕过缺角。
[[nodiscard]] const BoxShadow &MainMenuShadow() {
	static const auto result = BoxShadow(st::layerMainMenuShadow);
	return result;
}

[[nodiscard]] int MainMenuShadowExtend() {
	return MainMenuShadow().extend().right();
}

// 阴影框向内侧多伸出两个圆角，内侧的边与圆角落在可见区域之外。
void PaintMainMenuShadow(QPainter &p, int right, int height, int outerw) {
	const auto radius = st::boxRadius;
	MainMenuShadow().paint(
		p,
		style::rtlrect(-2 * radius, 0, right + 2 * radius, height, outerw),
		radius);
}

[[nodiscard]] QPixmap GrabMainMenu(not_null<RpWidget*> menu) {
	SendPendingMoveResizeEvents(menu);
	const auto extend = MainMenuShadowExtend();
	const auto size = QSize(menu->width() + extend, menu->height());
	auto result = QPixmap(size * style::DevicePixelRatio());
	result.setDevicePixelRatio(style::DevicePixelRatio());
	result.fill(Qt::transparent);
	// 带画笔渲染时图形效果取不到源图，截取期间停用，避免缓存缺少内容。
	const auto effect = menu->graphicsEffect();
	const auto effectEnabled = effect && effect->isEnabled();
	if (effectEnabled) {
		effect->setEnabled(false);
	}
	{
		auto p = QPainter(&result);
		PaintMainMenuShadow(p, menu->width(), menu->height(), size.width());
		RenderWidget(
			p,
			menu,
			QPoint(style::RightToLeft() ? extend : 0, 0));
	}
	if (effectEnabled) {
		effect->setEnabled(true);
	}
	return result;
}

// 两类图层各用一份阴影缓存，圆角不同时不会互相冲掉。
[[nodiscard]] const BoxShadow &SpecialLayerShadow() {
	static const auto result = BoxShadow(st::boxRoundShadow);
	return result;
}

[[nodiscard]] const BoxShadow &LayerShadow() {
	static const auto result = BoxShadow(st::boxRoundShadow);
	return result;
}

// 贴着容器边缘的一侧不画阴影，也不留阴影空间。
[[nodiscard]] RectParts ShadowSides(const QRect &box, const QRect &outer) {
	return RectPart::None
		| ((box.x() > outer.x()) ? RectPart::Left : RectPart::None)
		| ((box.y() > outer.y()) ? RectPart::Top : RectPart::None)
		| ((box.x() + box.width() < outer.x() + outer.width())
			? RectPart::Right
			: RectPart::None)
		| ((box.y() + box.height() < outer.y() + outer.height())
			? RectPart::Bottom
			: RectPart::None);
}

[[nodiscard]] QMargins ShadowExtend(
		const BoxShadow &shadow,
		RectParts sides) {
	const auto full = shadow.extend();
	return {
		(sides & RectPart::Left) ? full.left() : 0,
		(sides & RectPart::Top) ? full.top() : 0,
		(sides & RectPart::Right) ? full.right() : 0,
		(sides & RectPart::Bottom) ? full.bottom() : 0,
	};
}

// 不画阴影的一侧连同圆角推到可见区域外，其余各边按图层圆角生成。
void PaintLayerShadow(
		QPainter &p,
		const BoxShadow &shadow,
		const QRect &box,
		int radius,
		RectParts sides) {
	const auto skip = shadow.extend()
		+ QMargins(radius, radius, radius, radius);
	shadow.paint(p, box.marginsAdded({
		(sides & RectPart::Left) ? 0 : skip.left(),
		(sides & RectPart::Top) ? 0 : skip.top(),
		(sides & RectPart::Right) ? 0 : skip.right(),
		(sides & RectPart::Bottom) ? 0 : skip.bottom(),
	}), radius);
}

[[nodiscard]] QPixmap GrabLayer(
		not_null<LayerWidget*> layer,
		const BoxShadow &shadow,
		const QRect &outer) {
	SendPendingMoveResizeEvents(layer);
	const auto sides = ShadowSides(layer->geometry(), outer);
	const auto extend = ShadowExtend(shadow, sides);
	const auto inner = QRect(
		QPoint(extend.left(), extend.top()),
		layer->size());
	const auto ratio = style::DevicePixelRatio();
	auto result = QPixmap(inner.marginsAdded(extend).size() * ratio);
	result.setDevicePixelRatio(ratio);
	result.fill(Qt::transparent);
	{
		auto p = QPainter(&result);
		PaintLayerShadow(p, shadow, inner, layer->cornerRadius(), sides);
		RenderWidget(p, layer, inner.topLeft());
	}
	return result;
}

} // namespace

class LayerStackWidget::BackgroundWidget : public RpWidget {
public:
	using RpWidget::RpWidget;

	void setDoneCallback(Fn<void()> callback) {
		_doneCallback = std::move(callback);
	}

	void setLayerBoxes(
		const QRect &specialLayerBox,
		int specialLayerRadius,
		const QRect &layerBox,
		int layerRadius);
	void setCacheImages(
		QPixmap &&bodyCache,
		QPixmap &&mainMenuCache,
		QPixmap &&specialLayerCache,
		QPixmap &&layerCache,
		bool mainMenuHasWindowBackdrop);
	void removeBodyCache();
	[[nodiscard]] bool hasBodyCache() const;
	void refreshBodyCache(QPixmap &&bodyCache);
	void startAnimation(Action action, crl::time duration = 0);
	void skipAnimation(Action action, crl::time duration = 0);
	void finishAnimating();

	bool animating() const {
		return _a_mainMenuShown.animating() || _a_specialLayerShown.animating() || _a_layerShown.animating();
	}

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	bool isShown() const {
		return _mainMenuShown || _specialLayerShown || _layerShown;
	}
	void checkIfDone();
	void setMainMenuShown(bool shown);
	void setSpecialLayerShown(bool shown);
	void setLayerShown(bool shown);
	void checkWasShown(bool wasShown);
	void animationCallback();

	QPixmap _bodyCache;
	QPixmap _mainMenuCache;
	bool _mainMenuHasWindowBackdrop = false;
	int _mainMenuCacheWidth = 0;
	QPixmap _specialLayerCache;
	QPixmap _layerCache;

	Fn<void()> _doneCallback;

	bool _wasAnimating = false;
	bool _inPaintEvent = false;
	bool _repaintIssued = false;
	Ui::Animations::Simple _a_shown;
	Ui::Animations::Simple _a_mainMenuShown;
	Ui::Animations::Simple _a_specialLayerShown;
	Ui::Animations::Simple _a_layerShown;

	QRect _specialLayerBox, _specialLayerCacheBox;
	QRect _layerBox, _layerCacheBox;
	int _specialLayerRadius = 0;
	int _layerRadius = 0;
	int _mainMenuRight = 0;

	bool _mainMenuShown = false;
	bool _specialLayerShown = false;
	bool _layerShown = false;
	crl::time _duration = st::boxDuration;

};

void LayerStackWidget::BackgroundWidget::setCacheImages(
		QPixmap &&bodyCache,
		QPixmap &&mainMenuCache,
		QPixmap &&specialLayerCache,
		QPixmap &&layerCache,
		bool mainMenuHasWindowBackdrop) {
	_bodyCache = std::move(bodyCache);
	_mainMenuCache = std::move(mainMenuCache);
	_mainMenuHasWindowBackdrop = mainMenuHasWindowBackdrop;
	_specialLayerCache = std::move(specialLayerCache);
	_layerCache = std::move(layerCache);
	_specialLayerCacheBox = _specialLayerBox;
	_layerCacheBox = _layerBox;
	_repaintIssued = false;
	setAttribute(Qt::WA_OpaquePaintEvent,
		!_bodyCache.isNull() && !_bodyCache.hasAlphaChannel());
}

void LayerStackWidget::BackgroundWidget::removeBodyCache() {
	if (hasBodyCache()) {
		_bodyCache = {};
		setAttribute(Qt::WA_OpaquePaintEvent, false);
	}
}

bool LayerStackWidget::BackgroundWidget::hasBodyCache() const {
	return !_bodyCache.isNull();
}

void LayerStackWidget::BackgroundWidget::refreshBodyCache(
		QPixmap &&bodyCache) {
	_bodyCache = std::move(bodyCache);
	setAttribute(Qt::WA_OpaquePaintEvent,
		!_bodyCache.isNull() && !_bodyCache.hasAlphaChannel());
}

void LayerStackWidget::BackgroundWidget::startAnimation(
		Action action,
		crl::time duration) {
	_duration = (duration > 0) ? duration : st::boxDuration;
	if (action == Action::ShowMainMenu) {
		setMainMenuShown(true);
	} else if (action != Action::HideLayer
		&& action != Action::HideSpecialLayer) {
		setMainMenuShown(false);
	}
	if (action == Action::ShowSpecialLayer) {
		setSpecialLayerShown(true);
	} else if (action == Action::ShowMainMenu
		|| action == Action::HideAll
		|| action == Action::HideSpecialLayer) {
		setSpecialLayerShown(false);
	}
	if (action == Action::ShowLayer) {
		setLayerShown(true);
	} else if (action != Action::ShowSpecialLayer
		&& action != Action::HideSpecialLayer) {
		setLayerShown(false);
	}
	_wasAnimating = true;
	checkIfDone();
}

void LayerStackWidget::BackgroundWidget::skipAnimation(
		Action action,
		crl::time duration) {
	_repaintIssued = false;
	startAnimation(action, duration);
	finishAnimating();
}

void LayerStackWidget::BackgroundWidget::checkIfDone() {
	if (!_wasAnimating || _inPaintEvent || animating()) {
		return;
	}
	_wasAnimating = false;
	_mainMenuCache = _specialLayerCache = _layerCache = QPixmap();
	removeBodyCache();
	if (_doneCallback) {
		_doneCallback();
	}
}

void LayerStackWidget::BackgroundWidget::setMainMenuShown(bool shown) {
	auto wasShown = isShown();
	if (_mainMenuShown != shown) {
		_mainMenuShown = shown;
		_a_mainMenuShown.start(
			[this] { animationCallback(); },
			_mainMenuShown ? 0. : 1.,
			_mainMenuShown ? 1. : 0.,
			_duration,
			anim::easeOutCirc);
	}
	_mainMenuCacheWidth = (_mainMenuCache.width() / style::DevicePixelRatio())
		- MainMenuShadowExtend();
	_mainMenuRight = _mainMenuShown ? _mainMenuCacheWidth : 0;
	checkWasShown(wasShown);
}

void LayerStackWidget::BackgroundWidget::setSpecialLayerShown(bool shown) {
	auto wasShown = isShown();
	if (_specialLayerShown != shown) {
		_specialLayerShown = shown;
		_a_specialLayerShown.start(
			[this] { animationCallback(); },
			_specialLayerShown ? 0. : 1.,
			_specialLayerShown ? 1. : 0.,
			_duration);
	}
	checkWasShown(wasShown);
}

void LayerStackWidget::BackgroundWidget::setLayerShown(bool shown) {
	auto wasShown = isShown();
	if (_layerShown != shown) {
		_layerShown = shown;
		_a_layerShown.start(
			[this] { animationCallback(); },
			_layerShown ? 0. : 1.,
			_layerShown ? 1. : 0.,
			_duration);
	}
	checkWasShown(wasShown);
}

void LayerStackWidget::BackgroundWidget::checkWasShown(bool wasShown) {
	if (isShown() != wasShown) {
		_a_shown.start(
			[this] { animationCallback(); },
			wasShown ? 1. : 0.,
			wasShown ? 0. : 1.,
			_duration,
			anim::easeOutCirc);
	}
}

void LayerStackWidget::BackgroundWidget::setLayerBoxes(
		const QRect &specialLayerBox,
		int specialLayerRadius,
		const QRect &layerBox,
		int layerRadius) {
	_specialLayerBox = specialLayerBox;
	_specialLayerRadius = specialLayerRadius;
	_layerBox = layerBox;
	_layerRadius = layerRadius;
	update();
}

void LayerStackWidget::BackgroundWidget::paintEvent(QPaintEvent *e) {
	Painter p(this);

	_inPaintEvent = true;
	auto guard = gsl::finally([this] {
		_inPaintEvent = false;
		crl::on_main(this, [=] { checkIfDone(); });
	});

	if (!_bodyCache.isNull()) {
		// 缓存已包含完整背景，直接替换以免透明像素重复叠色。
		p.setCompositionMode(QPainter::CompositionMode_Source);
		p.drawPixmap(0, 0, _bodyCache);
		p.setCompositionMode(QPainter::CompositionMode_SourceOver);
	}

	auto specialLayerBox = _specialLayerCache.isNull() ? _specialLayerBox : _specialLayerCacheBox;
	auto layerBox = _layerCache.isNull() ? _layerBox : _layerCacheBox;

	auto mainMenuProgress = _a_mainMenuShown.value(-1);
	auto mainMenuRight = (_mainMenuCache.isNull() || mainMenuProgress < 0) ? _mainMenuRight : (mainMenuProgress < 0) ? _mainMenuRight : anim::interpolate(0, _mainMenuCacheWidth, mainMenuProgress);
	if (mainMenuRight) {
		// Move showing boxes to the right while main menu is hiding.
		if (!_specialLayerCache.isNull()) {
			specialLayerBox.moveLeft(specialLayerBox.left() + mainMenuRight / 2);
		}
		if (!_layerCache.isNull()) {
			layerBox.moveLeft(layerBox.left() + mainMenuRight / 2);
		}
	}
	auto bgOpacity = _a_shown.value(isShown() ? 1. : 0.);
	auto specialLayerOpacity = _a_specialLayerShown.value(_specialLayerShown ? 1. : 0.);
	auto layerOpacity = _a_layerShown.value(_layerShown ? 1. : 0.);
	if (bgOpacity == 0.) {
		return;
	}

	p.setOpacity(bgOpacity);
	auto overSpecialOpacity = (layerOpacity * specialLayerOpacity);
	auto bg = myrtlrect(mainMenuRight, 0, width() - mainMenuRight, height());

	if (_mainMenuCache.isNull() && mainMenuRight > 0) {
		// All cache images are taken together with their shadows,
		// so we paint shadow only when there is no cache.
		PaintMainMenuShadow(p, mainMenuRight, height(), width());
	}

	if (_specialLayerCache.isNull() && !specialLayerBox.isEmpty()) {
		// All cache images are taken together with their shadows,
		// so we paint shadow only when there is no cache.
		PaintLayerShadow(
			p,
			SpecialLayerShadow(),
			specialLayerBox,
			_specialLayerRadius,
			ShadowSides(specialLayerBox, rect()));
	}

	if (!layerBox.isEmpty() && !_specialLayerCache.isNull() && overSpecialOpacity < bgOpacity) {
		// In case of moving special layer below the background while showing a box
		// we need to fill special layer rect below its cache with a complex opacity
		// (alpha_final - alpha_current) / (1 - alpha_current) so we won't get glitches
		// in the transparent special layer cache corners after filling special layer
		// rect above its cache with alpha_current opacity.
		const auto region = QRegion(bg) - specialLayerBox;
		for (const auto &rect : region) {
			p.fillRect(rect, st::layerBg);
		}
		p.setOpacity((bgOpacity - overSpecialOpacity) / (1. - (overSpecialOpacity * st::layerBg->c.alphaF())));
		p.fillRect(specialLayerBox, st::layerBg);
		p.setOpacity(bgOpacity);
	} else {
		p.fillRect(bg, st::layerBg);
	}
	if (mainMenuRight > 0) {
		const auto radius = st::boxRadius;
		const auto left = mainMenuRight - radius;
		p.fillRect(myrtlrect(left, 0, radius, radius), st::layerBg);
		p.fillRect(
			myrtlrect(left, height() - radius, radius, radius),
			st::layerBg);
	}

	if (!_specialLayerCache.isNull() && specialLayerOpacity > 0) {
		p.setOpacity(specialLayerOpacity);
		const auto extend = ShadowExtend(
			SpecialLayerShadow(),
			ShadowSides(_specialLayerCacheBox, rect()));
		p.drawPixmapLeft(
			specialLayerBox.topLeft() - QPoint(extend.left(), extend.top()),
			width(),
			_specialLayerCache);
	}
	if (!layerBox.isEmpty()) {
		if (!_specialLayerCache.isNull()) {
			p.setOpacity(overSpecialOpacity);
			p.fillRect(specialLayerBox, st::layerBg);
		}
		if (_layerCache.isNull()) {
			p.setOpacity(layerOpacity);
			PaintLayerShadow(
				p,
				LayerShadow(),
				layerBox,
				_layerRadius,
				ShadowSides(layerBox, rect()));
		}
	}
	if (!_layerCache.isNull() && layerOpacity > 0) {
		p.setOpacity(layerOpacity);
		const auto extend = ShadowExtend(
			LayerShadow(),
			ShadowSides(_layerCacheBox, rect()));
		p.drawPixmapLeft(
			layerBox.topLeft() - QPoint(extend.left(), extend.top()),
			width(),
			_layerCache);
	}
	if (!_mainMenuCache.isNull() && mainMenuRight > 0) {
		p.setOpacity(1.);
		if (_mainMenuHasWindowBackdrop) {
			const auto radius = st::boxRadius;
			auto path = QPainterPath();
			path.addRoundedRect(
				myrtlrect(
					mainMenuRight - _mainMenuCacheWidth - radius,
					0,
					_mainMenuCacheWidth + radius,
					height()),
				radius,
				radius);
			auto hq = PainterHighQualityEnabler(p);
			p.setCompositionMode(QPainter::CompositionMode_Source);
			p.fillPath(path, Qt::transparent);
			p.setCompositionMode(QPainter::CompositionMode_SourceOver);
		}
		auto shownWidth = mainMenuRight + MainMenuShadowExtend();
		auto sourceWidth = shownWidth * style::DevicePixelRatio();
		auto sourceRect = style::rtlrect(_mainMenuCache.width() - sourceWidth, 0, sourceWidth, _mainMenuCache.height(), _mainMenuCache.width());
		p.drawPixmapLeft(0, 0, shownWidth, height(), width(), _mainMenuCache, sourceRect);
	}
	if (!_repaintIssued && !_a_shown.animating()) {
		_repaintIssued = true;
		update();
	}
}

void LayerStackWidget::BackgroundWidget::finishAnimating() {
	_a_shown.stop();
	_a_mainMenuShown.stop();
	_a_specialLayerShown.stop();
	_a_layerShown.stop();
	checkIfDone();
}

void LayerStackWidget::BackgroundWidget::animationCallback() {
	update();
	checkIfDone();
}

LayerStackWidget::LayerStackWidget(QWidget *parent, ShowFactory showFactory)
: RpWidget(parent)
, _background(this)
, _showFactory(std::move(showFactory)) {
	setGeometry(parentWidget()->rect());
	hide();
	_background->setDoneCallback([this] { animationDone(); });
}

int LayerWidget::cornerRadius() const {
	return st::boxRadius;
}

void LayerWidget::setInnerFocus() {
	if (!isAncestorOf(window()->focusWidget())) {
		doSetInnerFocus();
	}
}

bool LayerWidget::overlaps(const QRect &globalRect) {
	if (isHidden()) {
		return false;
	}
	auto testRect = QRect(mapFromGlobal(globalRect.topLeft()), globalRect.size());
	if (testAttribute(Qt::WA_OpaquePaintEvent)) {
		return rect().contains(testRect);
	}
	if (QRect(0, st::boxRadius, width(), height() - 2 * st::boxRadius).contains(testRect)) {
		return true;
	}
	if (QRect(st::boxRadius, 0, width() - 2 * st::boxRadius, height()).contains(testRect)) {
		return true;
	}
	return false;
}

void LayerWidget::mousePressEvent(QMouseEvent *e) {
	e->accept();
}

void LayerWidget::resizeEvent(QResizeEvent *e) {
	if (_resizedCallback) {
		_resizedCallback();
	}
}

bool LayerWidget::focusNextPrevChild(bool next) {
	return base::FocusNextPrevChildBlocked(this, next);
}

void LayerStackWidget::setHideByBackgroundClick(bool hide) {
	_hideByBackgroundClick = hide;
}

void LayerStackWidget::keyPressEvent(QKeyEvent *e) {
	if (e->key() == Qt::Key_Escape) {
		hideCurrent(anim::type::normal);
	}
}

void LayerStackWidget::mousePressEvent(QMouseEvent *e) {
	Ui::PostponeCall(this, [=] { backgroundClicked(); });
}

void LayerStackWidget::backgroundClicked() {
	if (!_hideByBackgroundClick) {
		return;
	}
	if (const auto layer = currentLayer()) {
		if (!layer->closeByOutsideClick()) {
			return;
		}
	} else if (const auto special = _specialLayer.data()) {
		if (!special->closeByOutsideClick()) {
			return;
		}
	}
	hideCurrent(anim::type::normal);
}

void LayerStackWidget::hideCurrent(anim::type animated) {
	return currentLayer() ? hideLayers(animated) : hideAll(animated);
}

void LayerStackWidget::hideLayers(anim::type animated) {
	const auto duration = (animated == anim::type::normal && currentLayer())
		? currentLayer()->animationDuration()
		: 0;
	startAnimation([] {}, [&] {
		clearLayers();
	}, Action::HideLayer, animated, duration);
}

void LayerStackWidget::hideAll(anim::type animated) {
	if (animated == anim::type::normal
		&& !layerShown()
		&& _background->animating()) {
		// Already hiding, new cache images would lose the hiding ones.
		return;
	}
	const auto duration = (animated == anim::type::normal)
		? currentLayer()
			? currentLayer()->animationDuration()
			: _specialLayer
			? _specialLayer->animationDuration()
			: _mainMenu
			? _mainMenu->animationDuration()
			: 0
		: 0;
	startAnimation([] {}, [&] {
		clearLayers();
		clearSpecialLayer();
		_mainMenu.destroy();
	}, Action::HideAll, animated, duration);
}

void LayerStackWidget::hideAllAnimatedPrepare() {
	prepareAnimation([] {}, [&] {
		clearLayers();
		clearSpecialLayer();
		_mainMenu.destroy();
	}, Action::HideAll, anim::type::normal, 0);
}

void LayerStackWidget::hideAllAnimatedRun() {
	if (_background->hasBodyCache()) {
		removeBodyCache();
		hideChildren();
		auto bodyCache = Ui::GrabWidget(parentWidget());
		showChildren();
		_background->refreshBodyCache(std::move(bodyCache));
	}
	_background->startAnimation(Action::HideAll);
}

void LayerStackWidget::hideTopLayer(anim::type animated) {
	if (_specialLayer || _mainMenu) {
		hideLayers(animated);
	} else {
		hideAll(animated);
	}
}

bool LayerStackWidget::closeCurrentByBackButton() {
	if (const auto layer = currentLayer()) {
		return layer->closeByBackButton();
	} else if (const auto special = _specialLayer.data()) {
		return special->closeByBackButton();
	} else if (const auto menu = _mainMenu.data()) {
		return menu->closeByBackButton();
	}
	return false;
}

void LayerStackWidget::removeBodyCache() {
	_background->removeBodyCache();
	setAttribute(Qt::WA_OpaquePaintEvent, false);
}

bool LayerStackWidget::layerShown() const {
	return _specialLayer || currentLayer() || _mainMenu;
}

bool LayerStackWidget::boxShown() const {
	return currentLayer() != nullptr;
}

rpl::producer<bool> LayerStackWidget::boxShownValue() const {
	return _boxShown.value();
}

void LayerStackWidget::updateBoxShown() {
	_boxShown = (currentLayer() != nullptr);
}

const LayerWidget *LayerStackWidget::topShownLayer() const {
	if (const auto result = currentLayer()) {
		return result;
	} else if (const auto special = _specialLayer.data()) {
		return special;
	} else if (const auto menu = _mainMenu.data()) {
		return menu;
	}
	return nullptr;
}

void LayerStackWidget::setStyleOverrides(
		const style::Box *boxSt,
		const style::Box *layerSt) {
	_boxSt = boxSt;
	_layerSt = layerSt;
}

void LayerStackWidget::setCacheImages() {
	auto bodyCache = QPixmap(), mainMenuCache = QPixmap();
	auto specialLayerCache = QPixmap();
	if (_specialLayer) {
		specialLayerCache = GrabLayer(
			_specialLayer.data(),
			SpecialLayerShadow(),
			rect());
	}
	auto layerCache = QPixmap();
	if (const auto layer = currentLayer()) {
		layerCache = GrabLayer(layer, LayerShadow(), rect());
	}
	if (isAncestorOf(window()->focusWidget())) {
		setFocus();
	}
	if (_mainMenu) {
		removeBodyCache();
		// 原生材质由系统实时合成，动画期间保留主窗口的实时背景。
		if (!_mainMenu->hasWindowBackdrop()) {
			hideChildren();
			bodyCache = Ui::GrabWidget(parentWidget());
			showChildren();
		}
		mainMenuCache = GrabMainMenu(_mainMenu);
	}
	setAttribute(Qt::WA_OpaquePaintEvent,
		!bodyCache.isNull() && !bodyCache.hasAlphaChannel());
	updateLayerBoxes();
	_background->setCacheImages(
		std::move(bodyCache),
		std::move(mainMenuCache),
		std::move(specialLayerCache),
		std::move(layerCache),
		_mainMenu && _mainMenu->hasWindowBackdrop());
}

void LayerStackWidget::closeLayer(not_null<LayerWidget*> layer) {
	const auto weak = base::make_weak(layer.get());
	if (Ui::InFocusChain(layer)) {
		setFocus();
	}
	if (!layer->setClosing()) {
		// This layer is already closing.
		return;
	} else if (!weak) {
		// setClosing() could've killed the layer.
		return;
	}

	if (layer == _specialLayer || layer == _mainMenu) {
		hideAll(anim::type::normal);
	} else if (layer == currentLayer()) {
		if (_layers.size() == 1) {
			hideCurrent(anim::type::normal);
		} else {
			const auto taken = std::move(_layers.back());
			_layers.pop_back();

			layer = currentLayer();
			layer->parentResized();
			if (!_background->animating()) {
				layer->show();
				showFinished();
			}
		}
	} else {
		for (auto i = _layers.begin(), e = _layers.end(); i != e; ++i) {
			if (layer == i->get()) {
				const auto taken = std::move(*i);
				_layers.erase(i);
				break;
			}
		}
	}
}

void LayerStackWidget::updateLayerBoxes() {
	const auto layerBox = [&] {
		if (const auto layer = currentLayer()) {
			return layer->geometry();
		}
		return QRect();
	}();
	const auto specialLayerBox = _specialLayer
		? _specialLayer->geometry()
		: QRect();
	const auto layer = currentLayer();
	_background->setLayerBoxes(
		specialLayerBox,
		_specialLayer ? _specialLayer->cornerRadius() : 0,
		layerBox,
		layer ? layer->cornerRadius() : 0);
	update();
}

void LayerStackWidget::finishAnimating() {
	_background->finishAnimating();
}

bool LayerStackWidget::canSetFocus() const {
	return (currentLayer() || _specialLayer || _mainMenu);
}

void LayerStackWidget::setInnerFocus() {
	if (_background->animating()) {
		setFocus();
	} else if (auto l = currentLayer()) {
		l->setInnerFocus();
	} else if (_specialLayer) {
		_specialLayer->setInnerFocus();
	} else if (_mainMenu) {
		_mainMenu->setInnerFocus();
	}
}

bool LayerStackWidget::contentOverlapped(const QRect &globalRect) {
	if (isHidden()) {
		return false;
	}
	if (_specialLayer && _specialLayer->overlaps(globalRect)) {
		return true;
	}
	if (auto layer = currentLayer()) {
		return layer->overlaps(globalRect);
	}
	return false;
}

template <typename SetupNew, typename ClearOld>
bool LayerStackWidget::prepareAnimation(
		SetupNew &&setupNewWidgets,
		ClearOld &&clearOldWidgets,
		Action action,
		anim::type animated,
		crl::time duration) {
	if (animated == anim::type::instant) {
		setupNewWidgets();
		clearOldWidgets();
		prepareForAnimation();
		_background->skipAnimation(action, duration);
	} else {
		setupNewWidgets();
		setCacheImages();
		const auto weak = base::make_weak(this);
		clearOldWidgets();
		if (weak) {
			prepareForAnimation();
			return true;
		}
	}
	return false;
}

template <typename SetupNew, typename ClearOld>
void LayerStackWidget::startAnimation(
		SetupNew &&setupNewWidgets,
		ClearOld &&clearOldWidgets,
		Action action,
		anim::type animated,
		crl::time duration) {
	const auto alive = prepareAnimation(
		std::forward<SetupNew>(setupNewWidgets),
		std::forward<ClearOld>(clearOldWidgets),
		action,
		animated,
		duration);
	if (alive) {
		_background->startAnimation(action, duration);
	}
}

void LayerStackWidget::resizeEvent(QResizeEvent *e) {
	const auto weak = base::make_weak(this);
	_background->setGeometry(rect());
	if (!weak) {
		return;
	}
	if (_specialLayer) {
		_specialLayer->parentResized();
		if (!weak) {
			return;
		}
	}
	if (const auto layer = currentLayer()) {
		layer->parentResized();
		if (!weak) {
			return;
		}
	}
	if (_mainMenu) {
		_mainMenu->parentResized();
		if (!weak) {
			return;
		}
	}
	updateLayerBoxes();
}

void LayerStackWidget::prepareForAnimation() {
	if (isHidden()) {
		show();
	}
	if (_mainMenu) {
		if (Ui::InFocusChain(_mainMenu)) {
			setFocus();
		}
		_mainMenu->hide();
	}
	if (_specialLayer) {
		if (Ui::InFocusChain(_specialLayer)) {
			setFocus();
		}
		_specialLayer->hide();
	}
	if (const auto layer = currentLayer()) {
		if (Ui::InFocusChain(layer)) {
			setFocus();
		}
		layer->hide();
	}
}

void LayerStackWidget::animationDone() {
	auto &integration = base::Integration::Instance();
	bool hidden = true;
	if (_mainMenu) {
		integration.setCrashAnnotation("ShowingWidget", u"MainMenu"_q);
		_mainMenu->show();
		hidden = false;
	}
	if (_specialLayer) {
		integration.setCrashAnnotation("ShowingWidget", u"SpecialLayer"_q);
		_specialLayer->show();
		hidden = false;
	}
	if (auto layer = currentLayer()) {
		integration.setCrashAnnotation("ShowingWidget", u"Box"_q);
		layer->show();
		hidden = false;
	}
	setAttribute(Qt::WA_OpaquePaintEvent, false);
	if (hidden) {
		_hideFinishStream.fire({});
	} else {
		integration.setCrashAnnotation("ShowingWidget", u"Finished"_q);
		showFinished();
		integration.setCrashAnnotation("ShowingWidget", QString());
	}
}

rpl::producer<> LayerStackWidget::hideFinishEvents() const {
	return _hideFinishStream.events();
}

void LayerStackWidget::showFinished() {
	fixOrder();
	sendFakeMouseEvent();
	updateLayerBoxes();
	if (_specialLayer) {
		_specialLayer->showFinished();
	}
	if (_mainMenu) {
		_mainMenu->showFinished();
	}
	if (auto layer = currentLayer()) {
		layer->showFinished();
	}
	if (canSetFocus()) {
		setInnerFocus();
	}
}

void LayerStackWidget::showSpecialLayer(
		object_ptr<LayerWidget> layer,
		anim::type animated) {
	startAnimation([&] {
		_specialLayer.destroy();
		_specialLayer = std::move(layer);
		initChildLayer(_specialLayer);
	}, [&] {
		_mainMenu.destroy();
	}, Action::ShowSpecialLayer, animated);
}

bool LayerStackWidget::showSectionInternal(
		not_null<::Window::SectionMemento*> memento,
		const ::Window::SectionShow &params) {
	if (_specialLayer) {
		return _specialLayer->showSectionInternal(memento, params);
	}
	return false;
}

void LayerStackWidget::hideSpecialLayer(anim::type animated) {
	startAnimation([] {}, [&] {
		clearSpecialLayer();
		_mainMenu.destroy();
	}, Action::HideSpecialLayer, animated);
}

void LayerStackWidget::showMainMenu(
		object_ptr<LayerWidget> layer,
		anim::type animated) {
	startAnimation([&] {
		_mainMenu = std::move(layer);
		initChildLayer(_mainMenu);
		_mainMenu->moveToLeft(0, 0);
	}, [&] {
		clearLayers();
		_specialLayer.destroy();
	}, Action::ShowMainMenu, animated);
}

void LayerStackWidget::showBox(
		object_ptr<BoxContent> box,
		LayerOptions options,
		anim::type animated) {
	showLayer(
		std::make_unique<BoxLayerWidget>(this, this, std::move(box)),
		options,
		animated);
}

void LayerStackWidget::showLayer(
		std::unique_ptr<LayerWidget> layer,
		LayerOptions options,
		anim::type animated) {
	if (options & LayerOption::KeepOther) {
		if (options & LayerOption::ShowAfterOther) {
			prependLayer(std::move(layer), animated);
		} else {
			appendLayer(std::move(layer), animated);
		}
	} else {
		replaceLayer(std::move(layer), animated);
	}
}

LayerWidget *LayerStackWidget::pushLayer(
		std::unique_ptr<LayerWidget> layer,
		anim::type animated) {
	const auto oldLayer = currentLayer();
	if (oldLayer) {
		if (Ui::InFocusChain(oldLayer)) {
			setFocus();
		}
		oldLayer->hide();
	}
	_layers.push_back(std::move(layer));
	const auto raw = _layers.back().get();
	initChildLayer(raw);

	if (_layers.size() > 1) {
		if (!_background->animating()) {
			raw->setVisible(true);
			showFinished();
		}
	} else {
		startAnimation([] {}, [&] {
			_mainMenu.destroy();
		}, Action::ShowLayer, animated);
	}

	updateBoxShown();
	return raw;
}

void LayerStackWidget::appendLayer(
		std::unique_ptr<LayerWidget> layer,
		anim::type animated) {
	pushLayer(std::move(layer), animated);
}

void LayerStackWidget::prependLayer(
		std::unique_ptr<LayerWidget> layer,
		anim::type animated) {
	if (_layers.empty()) {
		replaceLayer(std::move(layer), animated);
		return;
	}
	_layers.insert(
		begin(_layers),
		std::move(layer));
	const auto raw = _layers.front().get();
	raw->hide();
	initChildLayer(raw);
}

void LayerStackWidget::replaceLayer(
		std::unique_ptr<LayerWidget> layer,
		anim::type animated) {
	const auto pointer = pushLayer(std::move(layer), animated);
	const auto removeTill = ranges::find(
		_layers,
		pointer,
		&std::unique_ptr<LayerWidget>::get);
	_closingLayers.insert(
		end(_closingLayers),
		std::make_move_iterator(begin(_layers)),
		std::make_move_iterator(removeTill));
	_layers.erase(begin(_layers), removeTill);
	clearClosingLayers();
}

bool LayerStackWidget::takeToThirdSection() {
	return _specialLayer
		? _specialLayer->takeToThirdSection()
		: false;
}

void LayerStackWidget::clearLayers() {
	_closingLayers.insert(
		end(_closingLayers),
		std::make_move_iterator(begin(_layers)),
		std::make_move_iterator(end(_layers)));
	_layers.clear();
	updateBoxShown();
	clearClosingLayers();
}

void LayerStackWidget::clearClosingLayers() {
	const auto weak = base::make_weak(this);
	while (!_closingLayers.empty()) {
		const auto index = _closingLayers.size() - 1;
		const auto layer = _closingLayers.back().get();
		if (Ui::InFocusChain(layer)) {
			setFocus();
		}

		// This may destroy LayerStackWidget (by calling Ui::hideLayer).
		// So each time we check a weak pointer (if we are still alive).
		layer->setClosing();

		// setClosing() could destroy 'this' or could call clearLayers().
		if (weak && !_closingLayers.empty()) {
			// We could enqueue more closing layers, so we remove by index.
			Assert(index < _closingLayers.size());
			Assert(_closingLayers[index].get() == layer);
			_closingLayers.erase(begin(_closingLayers) + index);
		} else {
			// Everything was destroyed in clearLayers or ~LayerStackWidget.
			break;
		}
	}
}

void LayerStackWidget::clearSpecialLayer() {
	if (_specialLayer) {
		_specialLayer->setClosing();
		_specialLayer.destroy();
	}
}

void LayerStackWidget::initChildLayer(LayerWidget *layer) {
	layer->setParent(this);
	layer->setClosedCallback([=] { closeLayer(layer); });
	layer->setResizedCallback([=] { updateLayerBoxes(); });
	Ui::SendPendingMoveResizeEvents(layer);
	layer->parentResized();
}

void LayerStackWidget::fixOrder() {
	if (const auto layer = currentLayer()) {
		_background->raise();
		layer->raise();
	} else if (_specialLayer) {
		_specialLayer->raise();
	}
	if (_mainMenu) {
		_mainMenu->raise();
	}
}

void LayerStackWidget::sendFakeMouseEvent() {
	SendSynteticMouseEvent(this, QEvent::MouseMove, Qt::NoButton);
}

LayerStackWidget::~LayerStackWidget() {
	// Some layer destructors call back into LayerStackWidget.
	while (!_layers.empty() || !_closingLayers.empty()) {
		hideAll(anim::type::instant);
		clearClosingLayers();
	}
}

} // namespace Ui
