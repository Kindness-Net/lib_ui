// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "ui/layers/box_layer_widget.h"

#include "ui/layers/box_layer_shadow.h"
#include "ui/effects/radial_animation.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/painter.h"
#include "base/timer.h"
#include "styles/style_layers.h"
#include "styles/palette.h"

#include <crl/crl_on_main.h>
#include <QtGui/QWindow>

#include <limits>
#include <optional>

namespace Ui {

struct BoxLayerWidget::ButtonsLayout {
	struct Entry {
		not_null<AbstractButton*> button;
		QRect geometry;
	};
	std::vector<Entry> entries;
	int height = 0;
};

struct BoxLayerWidget::LoadingProgress {
	LoadingProgress(
		Fn<void()> &&callback,
		const style::InfiniteRadialAnimation &st);

	InfiniteRadialAnimation animation;
	base::Timer removeTimer;
};

BoxLayerWidget::LoadingProgress::LoadingProgress(
	Fn<void()> &&callback,
	const style::InfiniteRadialAnimation &st)
: animation(std::move(callback), st) {
}

BoxLayerWidget::BoxLayerWidget(
	QWidget *parent,
	not_null<LayerStackDelegate*> delegate,
	object_ptr<BoxContent> content)
: LayerWidget(parent)
, _layer(delegate)
, _content(std::move(content))
, _roundRect(st::boxRadius, st().bg)
, _drawerRoundRect(st::boxDrawerRadius, st().bg) {
	_content->setParent(this);

	_additionalTitle.changes(
	) | rpl::on_next([=] {
		updateSize();
		updateTitlePosition();
		update();
	}, lifetime());

	updateMaxRealHeight();
	_content->setDelegate(this);
}

BoxLayerWidget::~BoxLayerWidget() = default;

void BoxLayerWidget::setLayerType(bool layerType) {
	if (_layerType == layerType) {
		return;
	}
	_layerType = layerType;
	updateTitlePosition();
	if (_maxContentHeight) {
		setDimensions(width(), _maxContentHeight);
	}
}

int BoxLayerWidget::titleHeight() const {
	return st::boxTitleHeight;
}

const style::Box &BoxLayerWidget::st() const {
	if (_st) {
		return *_st;
	}
	const auto override = _layerType
		? _layer->boxStyleOverrideLayer()
		: _layer->boxStyleOverride();
	if (override) {
		return *override;
	}
	return _layerType ? st::layerBox : st::defaultBox;
}

void BoxLayerWidget::setStyle(const style::Box &st) {
	_st = &st;
	_roundRect.setColor(st.bg);
	_drawerRoundRect.setColor(st.bg);
	updateSize();
}

const style::Box &BoxLayerWidget::style() {
	return st();
}

const style::RoundButton &BoxLayerWidget::buttonStyle() const {
	return _buttons.empty() ? st().button : st().buttonSecondary;
}

auto BoxLayerWidget::buttonsLayout() const -> ButtonsLayout {
	const auto padding = st().buttonPadding;
	const auto parent = parentWidget();
	const auto responsive = parent && _layer->centerWithinOuter()
		&& _content->property("responsiveBoxWidth").toBool();
	const auto boxWidth = !responsive ? _boxWidth : bottomAnchored()
		? parent->width() : std::min(_boxWidth, parent->width());
	const auto regularHeight = padding.top()
		+ st().buttonHeight
		+ padding.bottom();
	auto result = ButtonsLayout();
	result.height = regularHeight;
	if (!_dimensionsSet) {
		return result;
	}
	const auto available = std::max(
		1,
		boxWidth - padding.left() - padding.right());
	if (st().buttonWide || !st().buttonHeight) {
		auto right = boxWidth - padding.right();
		for (const auto &button : _buttons) {
			const auto width = st().buttonWide ? available : button->width();
			right -= width;
			result.entries.push_back({ button.data(), {
				right, padding.top(), width, button->height(),
			} });
			right -= st().buttonSkip;
		}
		if (_leftButton) {
			result.entries.push_back({ _leftButton.data(), {
				padding.left(),
				padding.top(),
				_leftButton->width(),
				_leftButton->height(),
			} });
		}
		return result;
	}
	struct Row {
		std::vector<std::pair<not_null<AbstractButton*>, int>> right;
		std::optional<std::pair<not_null<AbstractButton*>, int>> left;
		int width = 0;
		int height = 0;
	};
	auto rows = std::vector<Row>();
	const auto append = [&](not_null<AbstractButton*> button, bool left) {
		if (button->isHidden()) {
			return;
		}
		const auto natural = button->naturalWidth();
		const auto width = std::clamp(
			(natural > 0) ? natural : button->width(),
			1,
			available);
		if (rows.empty()
			|| (rows.back().width
				&& rows.back().width + st().buttonSkip + width > available)) {
			rows.emplace_back();
		}
		auto &row = rows.back();
		row.width += width + (row.width ? st().buttonSkip : 0);
		row.height = std::max({ row.height, st().buttonHeight, button->height() });
		if (left) {
			row.left.emplace(button, width);
		} else {
			row.right.emplace_back(button, width);
		}
	};
	for (const auto &button : _buttons) {
		append(button.data(), false);
	}
	if (_leftButton) {
		append(_leftButton.data(), true);
	}
	if (rows.empty()) {
		// 定制弹窗仍保留各自样式约定的底部空间。
		if (&st() == &st::defaultBox || &st() == &st::layerBox) {
			result.height = _noContentMargin ? 0 : st::boxPadding.bottom();
		}
		return result;
	}
	auto top = padding.top();
	for (auto i = rows.rbegin(); i != rows.rend(); ++i) {
		auto right = boxWidth - padding.right();
		const auto place = [&](const auto &entry, int left) {
			const auto [button, width] = entry;
			result.entries.push_back({ button, {
				left,
				top + (i->height - button->height()) / 2,
				width,
				button->height(),
			} });
		};
		for (const auto &entry : i->right) {
			right -= entry.second;
			place(entry, right);
			right -= st().buttonSkip;
		}
		if (i->left) {
			place(*i->left, padding.left());
		}
		top += i->height + st().buttonSkip;
	}
	result.height = top - st().buttonSkip + padding.bottom();
	return result;
}

int BoxLayerWidget::buttonsHeight() const {
	return buttonsLayout().height;
}

QRect BoxLayerWidget::loadingRect() const {
	const auto padding = st().buttonPadding;
	const auto size = st::boxLoadingSize;
	const auto skipx = st::boxTitlePosition.x();
	const auto skipy = (st().buttonHeight - size) / 2;
	return QRect(
		skipx,
		height() - padding.bottom() - skipy - size,
		size,
		size);
}

void BoxLayerWidget::paintEvent(QPaintEvent *e) {
	Painter p(this);

	const auto clip = e->rect();
	const auto drawer = bottomAnchored();
	const auto radius = cornerRadius();
	const auto &roundRect = drawer ? _drawerRoundRect : _roundRect;
	const auto paintTopRounded = !(_customCornersFilling & RectPart::FullTop)
		&& clip.intersects(QRect(0, 0, width(), radius));
	const auto paintBottomRounded = !drawer && !(_customCornersFilling
		& RectPart::FullBottom)
		&& clip.intersects(
			QRect(0, height() - radius, width(), radius));
	if (paintTopRounded || paintBottomRounded) {
		roundRect.paint(p, rect(), RectPart::None
			| (paintTopRounded ? RectPart::FullTop : RectPart::None)
			| (paintBottomRounded ? RectPart::FullBottom : RectPart::None));
	}
	const auto other = e->region().intersected(
		QRect(0, radius, width(), height() - radius * (drawer ? 1 : 2)));
	if (!other.isEmpty()) {
		for (const auto &rect : other) {
			p.fillRect(rect, st().bg);
		}
	}
	if (!_additionalTitle.current().isEmpty()
		&& clip.intersects(QRect(0, 0, width(), titleHeight()))) {
		paintAdditionalTitle(p);
	}
	if (_loadingProgress) {
		const auto rect = loadingRect();
		_loadingProgress->animation.draw(
			p,
			rect.topLeft(),
			rect.size(),
			width());
	}
}

void BoxLayerWidget::paintAdditionalTitle(Painter &p) {
	const auto left = _titleLeft
		+ (_title ? _title->width() + st::boxTitleAdditionalSkip : 0);
	const auto available = std::max(0, width() - left - titleRightSkip());
	p.setFont(st::boxTitleAdditionalFont);
	p.setPen(st().titleAdditionalFg);
	p.drawTextLeft(
		left,
		_titleTop + st::boxTitleFont->ascent - st::boxTitleAdditionalFont->ascent,
		width(),
		st::boxTitleAdditionalFont->elided(
			_additionalTitle.current(),
			available));
}

void BoxLayerWidget::parentResized() {
	const auto parent = parentWidget();
	if (!parent || !_layer->centerWithinOuter()) {
		return;
	}
	if (_content->property("responsiveBoxWidth").toBool()) {
		setDimensions(_boxWidth, _maxContentHeight);
	}
	updateMaxRealHeight();
	const auto newHeight = countRealHeight();
	const auto parentSize = parent->size();
	setGeometry(
		(parentSize.width() - width()) / 2,
		bottomAnchored()
			? parentSize.height() - newHeight
			: (parentSize.height() - newHeight) / 2,
		width(),
		newHeight);
	update();
}

bool BoxLayerWidget::bottomAnchored() const {
	const auto parent = parentWidget();
	return parent
		&& _layer->centerWithinOuter()
		&& parent->width() < st::boxNarrowWidth;
}

void BoxLayerWidget::updateMaxRealHeight() {
	const auto &margin = st().margin;
	const auto outer = _layer->layerOuterSize();
	const auto parent = parentWidget();
	const auto containerHeight = outer
		? outer->height()
		: parent
		? parent->height()
		: std::numeric_limits<int>::max() / 2;
	const auto max = bottomAnchored()
		? containerHeight * 92 / 100
		: containerHeight - margin.top() - margin.bottom();
	_realHeightMax = max;
	_contentHeightMax = std::max(0, max - contentTop() - buttonsHeight());
}

void BoxLayerWidget::setTitle(
		rpl::producer<TextWithEntities> title,
		Text::MarkedContext context) {
	const auto wasTitle = hasTitle();
	if (title) {
		_title.create(
			this,
			rpl::duplicate(title),
			st().title,
			st::defaultPopupMenu,
			context);
		_title->show();
		std::move(
			title
		) | rpl::on_next([=] {
			updateTitlePosition();
		}, _title->lifetime());
	} else {
		_title.destroy();
	}
	if (wasTitle != hasTitle()) {
		updateSize();
	}
}

void BoxLayerWidget::setAdditionalTitle(rpl::producer<QString> additional) {
	_additionalTitle = std::move(additional);
}

void BoxLayerWidget::triggerButton(int index) {
	if (index < _buttons.size()) {
		_buttons[index]->clicked(Qt::KeyboardModifiers(), Qt::LeftButton);
	}
}

void BoxLayerWidget::setCloseByOutsideClick(bool close) {
	_closeByOutsideClick = close;
}

bool BoxLayerWidget::closeByOutsideClick() const {
	return _closeByOutsideClick;
}

rpl::producer<int> BoxLayerWidget::layerHeightMaxValue() {
	return _realHeightMax.value();
}

rpl::producer<int> BoxLayerWidget::contentHeightMaxValue() {
	return _contentHeightMax.value();
}

bool BoxLayerWidget::hasTitle() const {
	return (_title != nullptr) || !_additionalTitle.current().isEmpty();
}

void BoxLayerWidget::showBox(
		object_ptr<BoxContent> box,
		LayerOptions options,
		anim::type animated) {
	_layer->showBox(std::move(box), options, animated);
}

void BoxLayerWidget::hideLayer() {
	_layer->hideLayers(anim::type::normal);
}

ShowFactory BoxLayerWidget::showFactory() {
	return _layer->showFactory();
}

QPointer<QWidget> BoxLayerWidget::outerContainer() {
	if (const auto fromDelegate = _layer->layerOuterContainer()) {
		return fromDelegate;
	}
	if (const auto parent = parentWidget()) {
		return parent;
	}
	return this;
}

void BoxLayerWidget::updateSize() {
	if (!_dimensionsSet) {
		updateMaxRealHeight();
		return;
	}
	setDimensions(_boxWidth, _maxContentHeight);
	updateButtonsPositions();
	updateTitlePosition();
}

void BoxLayerWidget::scheduleButtonsUpdate() {
	if (_buttonsUpdateScheduled) {
		return;
	}
	_buttonsUpdateScheduled = true;
	// 等文字和自然宽度都更新完成，再计算按钮换行。
	crl::on_main(this, [=] {
		_buttonsUpdateScheduled = false;
		updateSize();
	});
}

void BoxLayerWidget::updateButtonsPositions() {
	if (!_dimensionsSet || _updatingButtons) {
		return;
	}
	_updatingButtons = true;
	const auto layout = buttonsLayout();
	const auto top = height() - layout.height;
	for (const auto &entry : layout.entries) {
		entry.button->setGeometryToLeft(
			entry.geometry.x(),
			top + entry.geometry.y(),
			entry.geometry.width(),
			entry.geometry.height());
	}
	auto right = st::boxTitleButtonsRight;
	for (const auto &button : _topButtons) {
		if (button->isHidden()) {
			continue;
		}
		button->moveToRight(
			right,
			std::max(0, (titleHeight() - button->height()) / 2));
		right += button->width() + st::boxTitleButtonsSkip;
	}
	_updatingButtons = false;
}

int BoxLayerWidget::titleRightSkip() const {
	if (_topButtons.empty()) {
		return st::boxTitlePosition.x();
	}
	auto result = st::boxTitleButtonsRight;
	for (const auto &button : _topButtons) {
		if (button->isHidden()) {
			continue;
		}
		result += button->width() + st::boxTitleButtonsSkip;
	}
	return (result == st::boxTitleButtonsRight)
		? st::boxTitlePosition.x()
		: result;
}

void BoxLayerWidget::updateTitlePosition() {
	_titleLeft = st::boxTitlePosition.x();
	_titleTop = st::boxTitlePosition.y();
	if (_title) {
		const auto available = std::max(
			0,
			width() - _titleLeft - titleRightSkip());
		const auto additionalWidth = _additionalTitle.current().isEmpty()
			? 0
			: std::min(
				st::boxTitleAdditionalFont->width(_additionalTitle.current())
					+ st::boxTitleAdditionalSkip,
				available / 2);
		_title->resizeToNaturalWidth(available - additionalWidth);
		_title->moveToLeft(_titleLeft, _titleTop);
	}
}

void BoxLayerWidget::setCustomCornersFilling(RectParts corners) {
	_customCornersFilling = corners;
	updateContentOpaque();
	update();
}

void BoxLayerWidget::clearButtons() {
	for (auto &button : base::take(_buttons)) {
		button.destroy();
	}
	_leftButton.destroy();
	base::take(_topButtons);
	updateTitlePosition();
	updateSize();
}

void BoxLayerWidget::addButton(object_ptr<AbstractButton> button) {
	_buttons.push_back(std::move(button));
	const auto raw = _buttons.back().data();
	raw->setParent(this);
	raw->show();
	rpl::combine(
		raw->sizeValue(),
		raw->naturalWidthValue(),
		raw->shownValue()
	) | rpl::on_next([=] {
		scheduleButtonsUpdate();
	}, raw->lifetime());
	updateSize();
}

void BoxLayerWidget::addLeftButton(object_ptr<AbstractButton> button) {
	_leftButton = std::move(button);
	const auto raw = _leftButton.data();
	raw->setParent(this);
	raw->show();
	rpl::combine(
		raw->sizeValue(),
		raw->naturalWidthValue(),
		raw->shownValue()
	) | rpl::on_next([=] {
		scheduleButtonsUpdate();
	}, raw->lifetime());
	updateSize();
}

void BoxLayerWidget::addTopButton(object_ptr<AbstractButton> button) {
	_topButtons.push_back(base::unique_qptr<AbstractButton>(button.release()));
	const auto raw = _topButtons.back().get();
	raw->setParent(this);
	raw->show();
	rpl::combine(
		raw->sizeValue(),
		raw->shownValue()
	) | rpl::on_next([=] {
		scheduleButtonsUpdate();
	}, raw->lifetime());
	updateButtonsPositions();
	updateTitlePosition();
}

void BoxLayerWidget::showLoading(bool show) {
	const auto &st = st::boxLoadingAnimation;
	if (!show) {
		if (_loadingProgress && !_loadingProgress->removeTimer.isActive()) {
			_loadingProgress->removeTimer.callOnce(
				st.sineDuration + st.sinePeriod);
			_loadingProgress->animation.stop();
		}
		return;
	}
	if (!_loadingProgress) {
		const auto callback = [=] {
			if (!anim::Disabled()) {
				const auto t = st::boxLoadingAnimation.thickness;
				update(loadingRect().marginsAdded({ t, t, t, t }));
			}
		};
		_loadingProgress = std::make_unique<LoadingProgress>(
			callback,
			st::boxLoadingAnimation);
		_loadingProgress->removeTimer.setCallback([=] {
			_loadingProgress = nullptr;
		});
	} else {
		_loadingProgress->removeTimer.cancel();
	}
	_loadingProgress->animation.start();
}


void BoxLayerWidget::setDimensions(
		int newWidth,
		int maxHeight,
		bool forceCenterPosition) {
	_boxWidth = newWidth;
	if (const auto parent = parentWidget(); parent
		&& _layer->centerWithinOuter()
		&& _content->property("responsiveBoxWidth").toBool()) {
		newWidth = bottomAnchored()
			? parent->width()
			: std::min(newWidth, parent->width());
	}
	_dimensionsSet = true;
	_maxContentHeight = maxHeight;
	updateMaxRealHeight();

	auto fullHeight = countFullHeight();
	if (width() != newWidth || _fullHeight != fullHeight) {
		_fullHeight = fullHeight;
		const auto oldGeometry = geometry();
		const auto parent = parentWidget();
		if (parent && _layer->centerWithinOuter()) {
			resize(newWidth, countRealHeight());
			auto newGeometry = geometry();
			auto parentHeight = parent->height();
			const auto bottomMargin = st().margin.bottom();
			if (bottomAnchored()) {
				move(
					(parent->width() - newWidth) / 2,
					parentHeight - newGeometry.height());
			} else if (newGeometry.top() + newGeometry.height() + bottomMargin > parentHeight
				|| forceCenterPosition) {
				const auto top1 = parentHeight - bottomMargin - newGeometry.height();
				const auto top2 = (parentHeight - newGeometry.height()) / 2;
				const auto newTop = forceCenterPosition
					? std::min(top1, top2)
					: std::max(top1, top2);
				if (newTop != newGeometry.top()) {
					move(newGeometry.left(), newTop);
				}
			}
			parent->update(oldGeometry.united(geometry()).marginsAdded(
				BoxLayerShadowExtend()));
		} else {
			resize(newWidth, countRealHeight());
		}
		// 外框受限时仍要重排正文，移动后也要同步阴影位置。
		if (size() == oldGeometry.size()
			|| pos() != oldGeometry.topLeft()) {
			resizeEvent(nullptr);
		}
	}
}

int BoxLayerWidget::countRealHeight() const {
	return std::min(_fullHeight, _realHeightMax.current());
}

int BoxLayerWidget::countFullHeight() const {
	return contentTop() + _maxContentHeight + buttonsHeight();
}

int BoxLayerWidget::cornerRadius() const {
	return bottomAnchored() ? st::boxDrawerRadius : st::boxRadius;
}

// 内容区伸进本控件负责的圆角范围时不能整块填矩形底色，否则盖住圆角。
void BoxLayerWidget::updateContentOpaque() {
	const auto radius = cornerRadius();
	const auto topRounded = !(_customCornersFilling & RectPart::FullTop);
	const auto bottomRounded = !bottomAnchored()
		&& !(_customCornersFilling & RectPart::FullBottom);
	_content->setAttribute(
		Qt::WA_OpaquePaintEvent,
		!_noContentMargin
			&& (!topRounded || contentTop() >= radius)
			&& (!bottomRounded || buttonsHeight() >= radius));
}

int BoxLayerWidget::contentTop() const {
	return hasTitle()
		? titleHeight()
		: _noContentMargin
		? 0
		: st::boxTopMargin;
}

void BoxLayerWidget::resizeEvent(QResizeEvent *e) {
	updateButtonsPositions();
	updateTitlePosition();

	const auto top = contentTop();
	_content->resize(width(), std::max(0, height() - top - buttonsHeight()));
	_content->moveToLeft(0, top);
	updateContentOpaque();

	LayerWidget::resizeEvent(e);
}

void BoxLayerWidget::keyPressEvent(QKeyEvent *e) {
	if (e->key() == Qt::Key_Escape) {
		closeBox();
	} else {
		LayerWidget::keyPressEvent(e);
	}
}

bool BoxLayerWidget::closeByBackButton() {
	if (_content->closeByEscape()) {
		closeBox();
	}
	return true;
}

void BoxLayerWidget::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton
		&& _layer->dragByTitle()
		&& e->pos().y() < titleHeight()) {
		if (const auto top = window()) {
			if (const auto handle = top->windowHandle()) {
				if (handle->startSystemMove()) {
					e->accept();
					return;
				}
			}
		}
	}
	LayerWidget::mousePressEvent(e);
}

} // namespace Ui
